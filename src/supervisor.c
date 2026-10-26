#include "minit.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BACKOFF_CAP 64
#define SHUTDOWN_TICK_MS 100
#define SHUTDOWN_TICKS   300  /* 30s total before SIGKILL */
#define RESTART_MAX_MS   60000
#define SPAWN_RETRY_MS   1000

struct supervisor {
	config_t config;
	int order[MINIT_MAX_SERVICES];
	int nservices;
	pid_t pids[MINIT_MAX_SERVICES];
	service_state_t states[MINIT_MAX_SERVICES];
	int restart_count[MINIT_MAX_SERVICES];
	long long spawn_ms[MINIT_MAX_SERVICES];  /* monotonic */
	long long restart_at[MINIT_MAX_SERVICES];  /* monotonic ms, 0 = now */
	int wake_fd[2];  /* self-pipe: signal handlers wake the poll() loop */
	volatile sig_atomic_t shutdown;
};

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int find_by_pid(const supervisor_t *s, pid_t pid)
{
	int i;
	for (i = 0; i < s->nservices; i++) {
		if (s->pids[i] == pid)
			return i;
	}
	return -1;
}

static int deps_ready(const supervisor_t *s, int idx)
{
	const service_t *svc = &s->config.services[idx];
	int i, prov;
	for (i = 0; i < svc->ndeps; i++) {
		prov = deps_find_provider(&s->config, svc->depends[i]);
		if (prov >= 0 && s->states[prov] != STATE_RUNNING)
			return 0;
	}
	return 1;
}

/*
 * Split exec into argv like a shell would, without expansion: '...' is
 * literal, "..." allows \" and \\, and \ escapes the next char outside quotes.
 * Returns argc, or -1 on an unterminated quote. Caller provides buf.
 */
int exec_split(const char *exec, char *buf, size_t bufsz, char *argv[MINIT_MAX_ARGS + 1])
{
	const char *in = exec;
	char *out = buf, *end = buf + bufsz - 1;
	char quote;
	int argc = 0;

	while (argc < MINIT_MAX_ARGS) {
		while (isspace((unsigned char)*in)) in++;
		if (!*in || out >= end) break;
		argv[argc++] = out;
		quote = 0;
		while (*in && (quote || !isspace((unsigned char)*in)) && out < end) {
			if (quote && *in == quote) {
				quote = 0;
				in++;
			} else if (!quote && (*in == '"' || *in == '\'')) {
				quote = *in++;
			} else if (*in == '\\' && quote != '\'' && in[1] &&
				   (!quote || in[1] == '"' || in[1] == '\\')) {
				in++;
				*out++ = *in++;
			} else {
				*out++ = *in++;
			}
		}
		if (quote)
			return -1;
		*out++ = '\0';
	}
	argv[argc] = NULL;
	return argc;
}

static int spawn(supervisor_t *s, int idx)
{
	service_t *svc = &s->config.services[idx];
	char exec_buf[MINIT_EXEC_LEN];
	char *argv[MINIT_MAX_ARGS + 1];
	pid_t pid;
	int argc, err, errpipe[2];
	ssize_t n;

	argc = exec_split(svc->exec, exec_buf, sizeof(exec_buf), argv);
	if (argc <= 0 || !argv[0][0])
		return -1;

	/* close-on-exec pipe: EOF means exec worked, an int means it failed with that errno */
	if (pipe(errpipe) < 0)
		return -1;
	fcntl(errpipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);

	pid = fork();
	if (pid < 0) {
		close(errpipe[0]);
		close(errpipe[1]);
		return -1;
	}

	if (pid == 0) {
		/* child: own session, so console job control and hangups don't reach it */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		/* searches PATH when argv[0] has no slash (default PATH if unset) */
		execvp(argv[0], argv);
		err = errno;
		(void)!write(errpipe[1], &err, sizeof(err));
		_exit(127);
	}

	close(errpipe[1]);
	do {
		n = read(errpipe[0], &err, sizeof(err));
	} while (n < 0 && errno == EINTR);
	close(errpipe[0]);
	/* still record the pid: the exit is reaped and restarted with backoff as usual */
	if (n == sizeof(err))
		fprintf(stderr, "minit: %s: cannot exec %s: %s\n",
			svc->name, argv[0], strerror(err));

	s->pids[idx] = pid;
	s->states[idx] = STATE_RUNNING;
	s->spawn_ms[idx] = now_ms();
	return 0;
}

static int restart_wanted(const service_t *svc, int exit_ok)
{
	switch (svc->restart) {
	case RESTART_NEVER:
		return 0;
	case RESTART_ON_FAILURE:
		if (exit_ok) return 0;
		break;
	case RESTART_ALWAYS:
		break;
	}
	return 1;
}

static int backoff_delay(const service_t *svc, int restart_count)
{
	int mult = 1;
	/* shifting by a negative or huge count is undefined, so double instead */
	while (restart_count-- > 0 && mult < BACKOFF_CAP)
		mult *= 2;
	return svc->respawn_delay * mult;
}

supervisor_t *supervisor_create(config_t *cfg, const int *order)
{
	supervisor_t *s;
	int i;

	s = calloc(1, sizeof(*s));
	if (!s) return NULL;

	if (pipe(s->wake_fd) < 0) {
		free(s);
		return NULL;
	}
	for (i = 0; i < 2; i++) {
		fcntl(s->wake_fd[i], F_SETFD, FD_CLOEXEC);
		fcntl(s->wake_fd[i], F_SETFL, O_NONBLOCK);
	}

	memcpy(&s->config, cfg, sizeof(s->config));
	s->nservices = cfg->nservices;
	for (i = 0; i < s->nservices; i++) {
		s->order[i] = order[i];
		s->states[i] = STATE_PENDING;
		s->pids[i] = 0;
	}
	return s;
}

void supervisor_destroy(supervisor_t *s)
{
	close(s->wake_fd[0]);
	close(s->wake_fd[1]);
	free(s);
}

int supervisor_start_all(supervisor_t *s)
{
	int i, idx, started = 0;
	for (i = 0; i < s->nservices; i++) {
		idx = s->order[i];
		if (!deps_ready(s, idx))
			continue;
		if (spawn(s, idx) < 0) {
			/* e.g. fork() failed: the main loop retries it */
			s->restart_at[idx] = now_ms() + SPAWN_RETRY_MS;
			continue;
		}
		started++;
	}
	return started;
}

static void handle_exit(supervisor_t *s, int idx, int status)
{
	service_t *svc = &s->config.services[idx];
	int exit_code = 0, sig = 0;
	long long now = now_ms();
	long long delay_ms;

	if (WIFEXITED(status))
		exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		sig = WTERMSIG(status);

	log_crash(svc->name, s->pids[idx], exit_code, sig, s->restart_count[idx]);

	s->pids[idx] = 0;
	s->states[idx] = STATE_STOPPED;

	if (!restart_wanted(svc, WIFEXITED(status) && exit_code == 0))
		return;

	/* Reset restart count if service ran for > respawn_window */
	if (now - s->spawn_ms[idx] > (long long)svc->respawn_window * 1000)
		s->restart_count[idx] = 0;

	if (svc->respawn_max > 0 && s->restart_count[idx] >= svc->respawn_max) {
		s->states[idx] = STATE_FAILED;
		log_giveup(svc->name, s->restart_count[idx]);
		return;
	}
	s->restart_count[idx]++;
	s->states[idx] = STATE_PENDING;

	delay_ms = (long long)backoff_delay(svc, s->restart_count[idx] - 1) * 1000;
	if (delay_ms > RESTART_MAX_MS)
		delay_ms = RESTART_MAX_MS;
	s->restart_at[idx] = now_ms() + delay_ms;
}

static void reap_children(supervisor_t *s)
{
	pid_t pid;
	int status, idx;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		idx = find_by_pid(s, pid);
		if (idx >= 0)
			handle_exit(s, idx, status);
	}
}

/* Spawn pending services that are due. Returns ms until the next one is due, or -1. */
static int start_due(supervisor_t *s)
{
	long long now = now_ms(), next = -1, wait;
	int i, idx;

	/* topological order, so dependents of a service spawned here start in the same pass */
	for (i = 0; i < s->nservices && !s->shutdown; i++) {
		idx = s->order[i];
		if (s->states[idx] != STATE_PENDING || !deps_ready(s, idx))
			continue;
		if (s->restart_at[idx] <= now) {
			if (spawn(s, idx) == 0)
				continue;
			s->restart_at[idx] = now + SPAWN_RETRY_MS;
		}
		wait = s->restart_at[idx] - now;
		if (next < 0 || wait < next)
			next = wait;
	}
	return (int)next;
}

static void drain_wake(supervisor_t *s)
{
	char buf[64];
	while (read(s->wake_fd[0], buf, sizeof(buf)) > 0)
		;
}

static void shutdown_all(supervisor_t *s)
{
	int i, idx;
	for (i = s->nservices - 1; i >= 0; i--) {
		idx = s->order[i];
		if (s->pids[idx] > 0) {
			kill(s->pids[idx], SIGTERM);
		}
	}
	for (i = 0; i < SHUTDOWN_TICKS; i++) {
		struct timespec tick = { 0, SHUTDOWN_TICK_MS * 1000000L };
		pid_t pid;
		int any = 0;
		while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
			idx = find_by_pid(s, pid);
			if (idx >= 0) {
				s->pids[idx] = 0;
				s->states[idx] = STATE_STOPPED;
			}
		}
		for (idx = 0; idx < s->nservices; idx++) {
			if (s->pids[idx] > 0) any = 1;
		}
		if (!any) break;
		nanosleep(&tick, NULL);
	}
	for (idx = 0; idx < s->nservices; idx++) {
		if (s->pids[idx] > 0) {
			kill(s->pids[idx], SIGKILL);
			waitpid(s->pids[idx], NULL, 0);
		}
	}
}

int supervisor_run(supervisor_t *s)
{
	struct pollfd pfd;
	int timeout;

	pfd.fd = s->wake_fd[0];
	pfd.events = POLLIN;

	while (!s->shutdown) {
		reap_children(s);
		timeout = start_due(s);
		if (s->shutdown)
			break;
		/* a signal arriving after reap/start writes to the pipe, so poll() still wakes */
		if (poll(&pfd, 1, timeout) > 0)
			drain_wake(s);
	}

	shutdown_all(s);
	return 0;
}

void supervisor_wake(supervisor_t *s)
{
	int saved = errno;
	char c = 0;
	(void)!write(s->wake_fd[1], &c, 1);
	errno = saved;
}

void supervisor_request_shutdown(supervisor_t *s)
{
	s->shutdown = 1;
	supervisor_wake(s);
}
