#include "minit.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>

extern char **environ;
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BACKOFF_CAP 64

struct supervisor {
	config_t config;
	int order[MINIT_MAX_SERVICES];
	int nservices;
	pid_t pids[MINIT_MAX_SERVICES];
	service_state_t states[MINIT_MAX_SERVICES];
	int restart_count[MINIT_MAX_SERVICES];
	time_t spawn_time[MINIT_MAX_SERVICES];
	volatile sig_atomic_t shutdown;
};

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
	int delay = svc->respawn_delay;
	int mult = 1 << restart_count;
	if (mult > BACKOFF_CAP) mult = BACKOFF_CAP;
	return delay * mult;
}

supervisor_t *supervisor_create(config_t *cfg, const int *order)
{
	supervisor_t *s;
	int i;

	s = calloc(1, sizeof(*s));
	if (!s) return NULL;

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
}

static void schedule_restart(supervisor_t *s, int idx)
{
	service_t *svc = &s->config.services[idx];
	int delay = backoff_delay(svc, s->restart_count[idx] - 1);
	if (delay > 0 && delay <= 60)
		sleep((unsigned)delay);
	else if (delay > 60)
		sleep(60);
	if (!s->shutdown)
		spawn(s, idx);
}

static void do_restarts(supervisor_t *s)
{
	int i;
	for (i = 0; i < s->nservices && !s->shutdown; i++) {
		if (s->states[i] == STATE_PENDING && deps_ready(s, i))
			schedule_restart(s, i);
	}
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
	for (i = 0; i < 30; i++) {
		pid_t pid;
		int any = 0;
		while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
			;
		for (idx = 0; idx < s->nservices; idx++) {
			if (s->pids[idx] > 0) any = 1;
		}
		if (!any) break;
		sleep(1);
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
	pid_t pid;
	int status;

	while (!s->shutdown) {
		pid = waitpid(-1, &status, 0);
		if (pid <= 0) {
			if (errno == EINTR)
				continue;
			if (errno == ECHILD) {
				do_restarts(s);
				sleep(1);  /* avoid busy loop when no children */
			}
			continue;
		}

		{
			int idx = find_by_pid(s, pid);
			if (idx >= 0)
				handle_exit(s, idx, status);
		}

		do_restarts(s);
	}

	shutdown_all(s);
	return 0;
}

void supervisor_request_shutdown(supervisor_t *s)
{
	s->shutdown = 1;
}
