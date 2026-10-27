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
#define MAX_POLL_FDS     (1 + 2 * MINIT_MAX_SERVICES)  /* wake, ready_fd, output */
#define READY_FILE_POLL_MS 100

struct supervisor {
	config_t config;
	int order[MINIT_MAX_SERVICES];
	int nservices;
	pid_t pids[MINIT_MAX_SERVICES];
	service_state_t states[MINIT_MAX_SERVICES];
	int restart_count[MINIT_MAX_SERVICES];
	long long spawn_ms[MINIT_MAX_SERVICES];  /* monotonic */
	long long restart_at[MINIT_MAX_SERVICES];  /* monotonic ms, 0 = now */
	int notify_fd[MINIT_MAX_SERVICES];  /* read end of the ready_fd pipe, -1 = none */
	int timed_out[MINIT_MAX_SERVICES];  /* killed for ready_timeout, waiting for the exit */
	long long boot_start_ms;
	int boot_logged;
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
		if (prov >= 0 && s->states[prov] != STATE_RUNNING && s->states[prov] != STATE_DONE)
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

static void close_notify(supervisor_t *s, int idx)
{
	if (s->notify_fd[idx] >= 0) {
		close(s->notify_fd[idx]);
		s->notify_fd[idx] = -1;
	}
}

/* Read from a ready_fd pipe: a newline means ready, EOF means it will never be. */
static void handle_notify(supervisor_t *s, int idx)
{
	char buf[64];
	ssize_t n = read(s->notify_fd[idx], buf, sizeof(buf));

	if (n < 0)
		return;  /* EAGAIN/EINTR: try again on the next wakeup */
	if (n > 0 && !memchr(buf, '\n', (size_t)n))
		return;
	if (n == 0)
		fprintf(stderr, "minit: %s: closed ready_fd without signalling readiness\n",
			s->config.services[idx].name);
	else if (s->states[idx] == STATE_STARTING)
		s->states[idx] = STATE_RUNNING;
	close_notify(s, idx);
}

static int spawn(supervisor_t *s, int idx)
{
	service_t *svc = &s->config.services[idx];
	char exec_buf[MINIT_EXEC_LEN];
	char *argv[MINIT_MAX_ARGS + 1];
	pid_t pid;
	int argc, err, errpipe[2], notify[2] = { -1, -1 }, out_fd;
	ssize_t n;

	argc = exec_split(svc->exec, exec_buf, sizeof(exec_buf), argv);
	if (argc <= 0 || !argv[0][0])
		return -1;

	/* close-on-exec pipe: EOF means exec worked, an int means it failed with that errno */
	if (pipe(errpipe) < 0)
		return -1;
	fcntl(errpipe[0], F_SETFD, FD_CLOEXEC);
	fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);

	out_fd = output_prepare(idx, svc);

	if (svc->ready_file[0])
		unlink(svc->ready_file);  /* a file left by the previous run must not count */

	if (svc->ready_fd && pipe(notify) < 0) {
		close(errpipe[0]);
		close(errpipe[1]);
		return -1;
	}
	if (svc->ready_fd) {
		fcntl(notify[0], F_SETFD, FD_CLOEXEC);
		fcntl(notify[0], F_SETFL, O_NONBLOCK);
		fcntl(notify[1], F_SETFD, FD_CLOEXEC);
	}

	pid = fork();
	if (pid < 0) {
		close(errpipe[0]);
		close(errpipe[1]);
		if (svc->ready_fd) {
			close(notify[0]);
			close(notify[1]);
		}
		return -1;
	}

	if (pid == 0) {
		/* child: own session, so console job control and hangups don't reach it */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		if (svc->log == LOG_NULL)
			out_fd = open("/dev/null", O_WRONLY);
		if (out_fd >= 0) {
			dup2(out_fd, 1);
			dup2(out_fd, 2);
		}
		if (svc->ready_fd) {
			/* move errpipe out of the way before dup2() replaces that fd */
			if (errpipe[1] == svc->ready_fd)
				errpipe[1] = fcntl(errpipe[1], F_DUPFD_CLOEXEC, svc->ready_fd + 1);
			if (notify[1] == svc->ready_fd)
				fcntl(notify[1], F_SETFD, 0);  /* dup2 to itself keeps CLOEXEC */
			else
				dup2(notify[1], svc->ready_fd);
		}
		/* searches PATH when argv[0] has no slash (default PATH if unset) */
		execvp(argv[0], argv);
		err = errno;
		(void)!write(errpipe[1], &err, sizeof(err));
		_exit(127);
	}

	close(errpipe[1]);
	if (svc->ready_fd) {
		close(notify[1]);
		close_notify(s, idx);
		s->notify_fd[idx] = notify[0];
	}
	do {
		n = read(errpipe[0], &err, sizeof(err));
	} while (n < 0 && errno == EINTR);
	close(errpipe[0]);
	/* still record the pid: the exit is reaped and restarted with backoff as usual */
	if (n == sizeof(err))
		fprintf(stderr, "minit: %s: cannot exec %s: %s\n",
			svc->name, argv[0], strerror(err));

	s->pids[idx] = pid;
	s->states[idx] = (svc->oneshot || svc->ready_fd || svc->ready_file[0]) ?
		STATE_STARTING : STATE_RUNNING;
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

supervisor_t *supervisor_create(config_t *cfg, const int *order, long long boot_start_ms)
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

	s->boot_start_ms = boot_start_ms;
	memcpy(&s->config, cfg, sizeof(s->config));
	s->nservices = cfg->nservices;
	for (i = 0; i < s->nservices; i++) {
		s->order[i] = order[i];
		s->states[i] = STATE_PENDING;
		s->pids[i] = 0;
		s->notify_fd[i] = -1;
	}
	return s;
}

void supervisor_destroy(supervisor_t *s)
{
	int i;
	for (i = 0; i < s->nservices; i++)
		close_notify(s, i);
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
	close_notify(s, idx);

	s->pids[idx] = 0;
	s->states[idx] = STATE_STOPPED;
	s->timed_out[idx] = 0;

	if (svc->oneshot && WIFEXITED(status) && exit_code == 0) {
		s->states[idx] = STATE_DONE;
		return;
	}
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

/* Mark STARTING services ready whose ready_file exists. Returns 1 if any still wait. */
static int check_ready_files(supervisor_t *s)
{
	int i, waiting = 0;
	for (i = 0; i < s->nservices; i++) {
		const service_t *svc = &s->config.services[i];
		if (s->states[i] != STATE_STARTING || !svc->ready_file[0])
			continue;
		if (access(svc->ready_file, F_OK) == 0)
			s->states[i] = STATE_RUNNING;
		else
			waiting = 1;
	}
	return waiting;
}

/* Earliest of two poll() timeouts, where -1 means none. */
static int min_timeout(int a, int b)
{
	if (a < 0)
		return b;
	if (b < 0)
		return a;
	return a < b ? a : b;
}

/* Kill services that stayed STARTING past ready_timeout. Returns ms until the next deadline, or -1. */
static int check_ready_timeouts(supervisor_t *s)
{
	long long now = now_ms(), deadline;
	int i, next = -1;

	for (i = 0; i < s->nservices; i++) {
		const service_t *svc = &s->config.services[i];
		if (s->states[i] != STATE_STARTING || !svc->ready_timeout || s->timed_out[i])
			continue;
		deadline = s->spawn_ms[i] + (long long)svc->ready_timeout * 1000;
		if (now < deadline) {
			next = min_timeout(next, (int)(deadline - now));
			continue;
		}
		/* the exit is then handled like any failure, including the restart policy */
		fprintf(stderr, "minit: %s: not ready after %ds, killing it\n",
			svc->name, svc->ready_timeout);
		kill(-s->pids[i], SIGKILL);  /* its process group: setsid() made it the leader */
		s->timed_out[i] = 1;
	}
	return next;
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

/*
 * Boot is complete when nothing is STARTING and every PENDING service can
 * never start because a dependency stopped or failed for good. Write the
 * boot report the first time that holds.
 */
static void check_boot_done(supervisor_t *s)
{
	int blocked[MINIT_MAX_SERVICES] = { 0 };
	int i, j, idx, prov, ready = 0;

	if (s->boot_logged)
		return;
	/* topological order: providers are classified before their dependents */
	for (i = 0; i < s->nservices; i++) {
		const service_t *svc;
		idx = s->order[i];
		svc = &s->config.services[idx];
		switch (s->states[idx]) {
		case STATE_STARTING:
			return;
		case STATE_PENDING:
			for (j = 0; j < svc->ndeps && !blocked[idx]; j++) {
				prov = deps_find_provider(&s->config, svc->depends[j]);
				blocked[idx] = s->states[prov] == STATE_STOPPED ||
					       s->states[prov] == STATE_FAILED || blocked[prov];
			}
			if (!blocked[idx])
				return;  /* still due to start */
			break;
		case STATE_RUNNING:
		case STATE_DONE:
			ready++;
			break;
		default:
			break;
		}
	}
	s->boot_logged = 1;
	log_boot((int)(now_ms() - s->boot_start_ms), s->nservices, ready);
}

static void drain_wake(supervisor_t *s)
{
	char buf[64];
	while (read(s->wake_fd[0], buf, sizeof(buf)) > 0)
		;
}

static void drain_outputs(supervisor_t *s)
{
	int i;
	for (i = 0; i < s->nservices; i++) {
		if (output_poll_fd(i) >= 0)
			output_handle(i, &s->config.services[i]);
	}
}

static void shutdown_all(supervisor_t *s)
{
	pid_t groups[MINIT_MAX_SERVICES];  /* each service leads its own process group */
	int i, idx;
	for (i = s->nservices - 1; i >= 0; i--) {
		idx = s->order[i];
		groups[idx] = s->pids[idx];
		if (s->pids[idx] > 0)
			kill(-s->pids[idx], SIGTERM);
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
		drain_outputs(s);  /* keep what services print while stopping */
		watchdog_due();
		if (!any) break;
		nanosleep(&tick, NULL);
	}
	for (idx = 0; idx < s->nservices; idx++) {
		/* also hits children that outlived the service's main process */
		if (groups[idx] > 0)
			kill(-groups[idx], SIGKILL);
		if (s->pids[idx] > 0)
			waitpid(s->pids[idx], NULL, 0);
	}
}

int supervisor_run(supervisor_t *s)
{
	struct pollfd pfd[MAX_POLL_FDS];
	/* per pfd: -1 = wake pipe, i = ready_fd of service i, MAX + i = output of service i */
	int owner[MAX_POLL_FDS];
	int timeout, nfds, i;

	while (!s->shutdown) {
		reap_children(s);
		check_ready_files(s);
		timeout = start_due(s);
		/* files can't be polled: check again soon while someone waits on one */
		if (check_ready_files(s))
			timeout = min_timeout(timeout, READY_FILE_POLL_MS);
		timeout = min_timeout(timeout, check_ready_timeouts(s));
		timeout = min_timeout(timeout, watchdog_due());
		check_boot_done(s);
		if (s->shutdown)
			break;

		nfds = 0;
		pfd[nfds].fd = s->wake_fd[0];
		pfd[nfds].events = POLLIN;
		owner[nfds++] = -1;
		for (i = 0; i < s->nservices; i++) {
			if (s->notify_fd[i] < 0)
				continue;
			pfd[nfds].fd = s->notify_fd[i];
			pfd[nfds].events = POLLIN;
			owner[nfds++] = i;
		}
		for (i = 0; i < s->nservices; i++) {
			if (output_poll_fd(i) < 0)
				continue;
			pfd[nfds].fd = output_poll_fd(i);
			pfd[nfds].events = POLLIN;
			owner[nfds++] = MINIT_MAX_SERVICES + i;
		}

		/* a signal arriving after reap/start writes to the pipe, so poll() still wakes */
		if (poll(pfd, (nfds_t)nfds, timeout) <= 0)
			continue;
		for (i = 0; i < nfds; i++) {
			if (!pfd[i].revents)
				continue;
			if (owner[i] < 0)
				drain_wake(s);
			else if (owner[i] >= MINIT_MAX_SERVICES)
				output_handle(owner[i] - MINIT_MAX_SERVICES,
					      &s->config.services[owner[i] - MINIT_MAX_SERVICES]);
			else
				handle_notify(s, owner[i]);
		}
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
