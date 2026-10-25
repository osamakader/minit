#include "minit.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

extern char **environ;
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
	time_t spawn_time[MINIT_MAX_SERVICES];
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

static int find_provider_idx(const supervisor_t *s, const char *name)
{
	int i;
	for (i = 0; i < s->nservices; i++) {
		if (s->config.services[i].provides[0] &&
		    strcmp(s->config.services[i].provides, name) == 0)
			return i;
	}
	return -1;
}

static int deps_ready(const supervisor_t *s, int idx)
{
	service_t *svc = &s->config.services[idx];
	int i, prov;
	for (i = 0; i < svc->ndeps; i++) {
		prov = find_provider_idx(s, svc->depends[i]);
		if (prov >= 0 && s->states[prov] != STATE_RUNNING)
			return 0;
	}
	return 1;
}

/* Split exec into argv. Returns argc, fills argv. Caller provides buf. */
static int parse_argv(const char *exec, char *buf, size_t bufsz, char *argv[MINIT_MAX_ARGS])
{
	char *p;
	int argc = 0;

	strncpy(buf, exec, bufsz - 1);
	buf[bufsz - 1] = '\0';

	for (p = buf; *p && argc < MINIT_MAX_ARGS; ) {
		while (*p && isspace((unsigned char)*p)) *p++ = '\0';
		if (!*p) break;
		argv[argc++] = p;
		while (*p && !isspace((unsigned char)*p)) p++;
	}
	argv[argc] = NULL;
	return argc;
}

static int spawn(supervisor_t *s, int idx)
{
	service_t *svc = &s->config.services[idx];
	char exec_buf[MINIT_EXEC_LEN];
	char *argv[MINIT_MAX_ARGS];
	pid_t pid;
	int argc;

	argc = parse_argv(svc->exec, exec_buf, sizeof(exec_buf), argv);
	if (argc == 0 || !argv[0][0])
		return -1;

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		/* child */
		execve(argv[0], argv, environ);
		_exit(127);
	}

	s->pids[idx] = pid;
	s->states[idx] = STATE_RUNNING;
	s->spawn_time[idx] = time(NULL);
	return 0;
}

static int should_restart(const supervisor_t *s, int idx, int exit_ok)
{
	service_t *svc = &s->config.services[idx];
	time_t now = time(NULL);
	int count = s->restart_count[idx];

	switch (svc->restart) {
	case RESTART_NEVER:
		return 0;
	case RESTART_ON_FAILURE:
		if (exit_ok) return 0;
		break;
	case RESTART_ALWAYS:
		break;
	}

	if (svc->respawn_max > 0) {
		time_t window_start = now - svc->respawn_window;
		if (s->spawn_time[idx] > window_start) {
			/* exited quickly, count it */
			if (count >= svc->respawn_max)
				return 0;
		}
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
		if (spawn(s, idx) < 0)
			return -1;
		started++;
	}
	return started;
}

static void handle_exit(supervisor_t *s, int idx, int status)
{
	service_t *svc = &s->config.services[idx];
	int exit_code = 0, sig = 0;
	time_t now = time(NULL);
	long long delay_ms;

	if (WIFEXITED(status))
		exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		sig = WTERMSIG(status);

	log_crash(svc->name, s->pids[idx], exit_code, sig, s->restart_count[idx]);

	s->pids[idx] = 0;
	s->states[idx] = STATE_STOPPED;

	if (!should_restart(s, idx, (exit_code == 0)))
		return;

	/* Reset restart count if service ran for > respawn_window */
	if (now - s->spawn_time[idx] > (time_t)svc->respawn_window)
		s->restart_count[idx] = 0;
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
