#include "minit.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
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
#define MAX_POLL_FDS     (2 + 2 * MINIT_MAX_SERVICES)  /* wake, control, ready_fd, output */
#define OWNER_WAKE       (-1)
#define OWNER_CONTROL    (-2)
#define STOP_TIMEOUT_MS  10000  /* minitctl stop/restart: SIGKILL after this */
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
	int held[MINIT_MAX_SERVICES];       /* stopped by minitctl: never restarted */
	int restart_req[MINIT_MAX_SERVICES];  /* minitctl restart: start again on exit */
	long long kill_at[MINIT_MAX_SERVICES];  /* stop requested: SIGKILL then, 0 = none */
	/* still running after being removed by a reload, until reaped */
	pid_t retired[MINIT_MAX_SERVICES];
	long long retired_kill_at[MINIT_MAX_SERVICES];
	long long boot_start_ms;
	int boot_logged;
	int action;      /* shutdown action requested through the supervisor, -1 = none */
	int ctl_fd;      /* minitctl listening socket, -1 = none */
	char ctl_path[108];  /* sun_path size */
	int wake_fd[2];  /* self-pipe: signal handlers wake the poll() loop */
	volatile sig_atomic_t shutdown;
	volatile sig_atomic_t reload;
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

/* What the child changes before exec; resolved in the parent so errors are clear. */
struct child_ids {
	int set_uid, set_gid;
	uid_t uid;
	gid_t gid;
	char initgroups[MINIT_ID_LEN];  /* user name for supplementary groups, or empty */
};

/* Steps of the child setup, reported back with errno when one fails. */
enum { STEP_EXEC, STEP_SETGROUPS, STEP_SETGID, STEP_SETUID };
static const char *const step_names[] = { "exec", "setgroups", "setgid", "setuid" };

static int numeric_id(const char *v, unsigned long *out)
{
	char *end;
	*out = strtoul(v, &end, 10);
	return isdigit((unsigned char)v[0]) && !*end;
}

/* Returns -1 (after reporting it) if the user or group does not exist. */
static int resolve_ids(const service_t *svc, struct child_ids *ids)
{
	struct passwd *pw = NULL;
	struct group *gr;
	unsigned long n;

	memset(ids, 0, sizeof(*ids));
	if (svc->user[0]) {
		pw = numeric_id(svc->user, &n) ? getpwuid((uid_t)n) : getpwnam(svc->user);
		if (!pw && !numeric_id(svc->user, &n)) {
			fprintf(stderr, "minit: %s: no user '%s'\n", svc->name, svc->user);
			return -1;
		}
		ids->set_uid = 1;
		ids->uid = pw ? pw->pw_uid : (uid_t)n;
		if (pw) {
			ids->set_gid = 1;
			ids->gid = pw->pw_gid;
			snprintf(ids->initgroups, sizeof(ids->initgroups), "%s", pw->pw_name);
		}
	}
	if (svc->group[0]) {
		gr = numeric_id(svc->group, &n) ? NULL : getgrnam(svc->group);
		if (!gr && !numeric_id(svc->group, &n)) {
			fprintf(stderr, "minit: %s: no group '%s'\n", svc->name, svc->group);
			return -1;
		}
		ids->set_gid = 1;
		ids->gid = gr ? gr->gr_gid : (gid_t)n;
	}
	return 0;
}

/* In the child: drop to the service's group and user. Returns the failed step, or -1. */
static int apply_ids(const struct child_ids *ids)
{
	if (ids->set_gid) {
		/* only root can change supplementary groups; anyone else keeps their own */
		if (geteuid() == 0 && (ids->initgroups[0] ? initgroups(ids->initgroups, ids->gid)
							   : setgroups(1, &ids->gid)) < 0)
			return STEP_SETGROUPS;
		if (setgid(ids->gid) < 0)
			return STEP_SETGID;
	}
	if (ids->set_uid && setuid(ids->uid) < 0)
		return STEP_SETUID;
	return -1;
}

/* 0 = spawned, -1 = try again later, -2 = cannot work until the config changes */
static int spawn(supervisor_t *s, int idx)
{
	service_t *svc = &s->config.services[idx];
	char exec_buf[MINIT_EXEC_LEN];
	char *argv[MINIT_MAX_ARGS + 1];
	struct child_ids ids;
	pid_t pid;
	int argc, err[2], errpipe[2], notify[2] = { -1, -1 }, out_fd;
	ssize_t n;

	argc = exec_split(svc->exec, exec_buf, sizeof(exec_buf), argv);
	if (argc <= 0 || !argv[0][0])
		return -1;
	if (resolve_ids(svc, &ids) < 0)
		return -2;

	/* close-on-exec pipe: EOF means exec worked, { step, errno } means that step failed */
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
		err[0] = apply_ids(&ids);
		if (err[0] < 0) {
			/* searches PATH when argv[0] has no slash (default PATH if unset) */
			execvp(argv[0], argv);
			err[0] = STEP_EXEC;
		}
		err[1] = errno;
		(void)!write(errpipe[1], err, sizeof(err));
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
	if (n == sizeof(err) && err[0] == STEP_EXEC)
		fprintf(stderr, "minit: %s: cannot exec %s: %s\n",
			svc->name, argv[0], strerror(err[1]));
	else if (n == sizeof(err))
		fprintf(stderr, "minit: %s: cannot start: %s: %s\n",
			svc->name, step_names[err[0]], strerror(err[1]));

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
	s->action = -1;
	s->ctl_fd = -1;
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
	if (s->ctl_fd >= 0) {
		close(s->ctl_fd);
		unlink(s->ctl_path);
	}
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
		switch (spawn(s, idx)) {
		case 0:
			started++;
			break;
		case -2:
			s->states[idx] = STATE_FAILED;
			break;
		default:
			/* e.g. fork() failed: the main loop retries it */
			s->restart_at[idx] = now_ms() + SPAWN_RETRY_MS;
		}
	}
	return started;
}

static void critical_failed(supervisor_t *s, int idx)
{
	fprintf(stderr, "minit: critical service %s failed, rebooting\n",
		s->config.services[idx].name);
	supervisor_request_action(s, SHUTDOWN_REBOOT);
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
	s->kill_at[idx] = 0;

	/* stopped or restarted on request: no restart policy, backoff or critical reboot */
	if (s->held[idx])
		return;
	if (s->restart_req[idx]) {
		s->restart_req[idx] = 0;
		s->restart_count[idx] = 0;
		s->states[idx] = STATE_PENDING;
		s->restart_at[idx] = 0;
		return;
	}

	if (svc->oneshot && WIFEXITED(status) && exit_code == 0) {
		s->states[idx] = STATE_DONE;
		return;
	}
	if (!restart_wanted(svc, WIFEXITED(status) && exit_code == 0)) {
		if (svc->critical && !(WIFEXITED(status) && exit_code == 0))
			critical_failed(s, idx);
		return;
	}

	/* Reset restart count if service ran for > respawn_window */
	if (now - s->spawn_ms[idx] > (long long)svc->respawn_window * 1000)
		s->restart_count[idx] = 0;

	if (svc->respawn_max > 0 && s->restart_count[idx] >= svc->respawn_max) {
		s->states[idx] = STATE_FAILED;
		log_giveup(svc->name, s->restart_count[idx]);
		if (svc->critical)
			critical_failed(s, idx);
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
		if (idx >= 0) {
			handle_exit(s, idx, status);
			continue;
		}
		for (idx = 0; idx < MINIT_MAX_SERVICES; idx++) {
			if (s->retired[idx] == pid)
				s->retired[idx] = 0;
		}
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

/* SIGKILL services that ignored a stop request. Returns ms until the next deadline, or -1. */
static int check_stop_deadlines(supervisor_t *s)
{
	long long now = now_ms();
	int i, next = -1;

	for (i = 0; i < s->nservices; i++) {
		if (!s->kill_at[i] || s->pids[i] <= 0)
			continue;
		if (now < s->kill_at[i]) {
			next = min_timeout(next, (int)(s->kill_at[i] - now));
			continue;
		}
		kill(-s->pids[i], SIGKILL);
		s->kill_at[i] = 0;
	}
	for (i = 0; i < MINIT_MAX_SERVICES; i++) {
		if (!s->retired[i] || !s->retired_kill_at[i])
			continue;
		if (now < s->retired_kill_at[i]) {
			next = min_timeout(next, (int)(s->retired_kill_at[i] - now));
			continue;
		}
		kill(-s->retired[i], SIGKILL);
		s->retired_kill_at[i] = 0;
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
			int r = spawn(s, idx);
			if (r == 0)
				continue;
			if (r == -2) {
				s->states[idx] = STATE_FAILED;
				continue;
			}
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

static const char *const state_names[] = {
	"pending", "starting", "running", "done", "stopped", "failed"
};

static void cmd_status(const supervisor_t *s, char *out, size_t len)
{
	size_t used;
	int i, idx;

	used = (size_t)snprintf(out, len, "%-20s %-9s %7s %8s\n", "SERVICE", "STATE", "PID", "RESTARTS");
	for (i = 0; i < s->nservices && used < len; i++) {
		idx = s->order[i];
		used += (size_t)snprintf(out + used, len - used, "%-20s %-9s %7d %8d\n",
			s->config.services[idx].name, state_names[s->states[idx]],
			(int)s->pids[idx], s->restart_count[idx]);
	}
}

static int find_by_name(const supervisor_t *s, const char *name)
{
	int i;
	for (i = 0; i < s->nservices; i++) {
		if (strcmp(s->config.services[i].name, name) == 0)
			return i;
	}
	return -1;
}

static void stop_service(supervisor_t *s, int idx)
{
	kill(-s->pids[idx], SIGTERM);
	s->kill_at[idx] = now_ms() + STOP_TIMEOUT_MS;
}

/* start, stop or restart one service; writes the reply */
static void cmd_service(supervisor_t *s, const char *cmd, const char *name, char *out, size_t len)
{
	int idx = find_by_name(s, name);
	int running;

	if (idx < 0) {
		snprintf(out, len, "error: no service '%.64s'\n", name);
		return;
	}
	running = s->pids[idx] > 0;
	if (strcmp(cmd, "stop") == 0) {
		s->held[idx] = 1;
		s->restart_req[idx] = 0;
		if (running) {
			stop_service(s, idx);
			snprintf(out, len, "stopping %s\n", name);
		} else {
			s->states[idx] = STATE_STOPPED;
			snprintf(out, len, "%s stopped\n", name);
		}
		return;
	}
	/* start and restart */
	s->held[idx] = 0;
	if (running && strcmp(cmd, "start") == 0) {
		snprintf(out, len, "%s is already running\n", name);
	} else if (running) {
		s->restart_req[idx] = 1;
		stop_service(s, idx);
		snprintf(out, len, "restarting %s\n", name);
	} else {
		s->restart_count[idx] = 0;
		s->states[idx] = STATE_PENDING;
		s->restart_at[idx] = 0;
		snprintf(out, len, "starting %s\n", name);
	}
}

static void handle_control(supervisor_t *s)
{
	char req[256], reply[CONTROL_REPLY_LEN];
	char *arg;
	int fd;

	fd = control_accept(s->ctl_fd, req, sizeof(req));
	if (fd < 0)
		return;
	arg = strchr(req, ' ');
	if (arg)
		*arg++ = '\0';
	if (strcmp(req, "status") == 0)
		cmd_status(s, reply, sizeof(reply));
	else if (!arg && (strcmp(req, "reboot") == 0 || strcmp(req, "poweroff") == 0 ||
			  strcmp(req, "halt") == 0)) {
		snprintf(reply, sizeof(reply), "%s requested\n", req);
		supervisor_request_action(s, req[0] == 'r' ? SHUTDOWN_REBOOT :
					     req[0] == 'p' ? SHUTDOWN_POWEROFF : SHUTDOWN_HALT);
	}
	else if (arg && (strcmp(req, "start") == 0 || strcmp(req, "stop") == 0 ||
			 strcmp(req, "restart") == 0))
		cmd_service(s, req, arg, reply, sizeof(reply));
	else if (!arg && strcmp(req, "reload") == 0) {
		snprintf(reply, sizeof(reply), "reload requested\n");
		supervisor_request_reload(s);
	}
	else
		snprintf(reply, sizeof(reply), "error: unknown command '%.64s'\n", req);
	control_reply(fd, reply);
}

void supervisor_listen(supervisor_t *s, const char *socket_path)
{
	s->ctl_fd = control_open(socket_path);
	snprintf(s->ctl_path, sizeof(s->ctl_path), "%s", socket_path);
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
	for (idx = 0; idx < MINIT_MAX_SERVICES; idx++) {
		if (s->retired[idx] > 0)
			kill(-s->retired[idx], SIGKILL);
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
	/* per pfd: OWNER_*, i = ready_fd of service i, MAX + i = output of service i */
	int owner[MAX_POLL_FDS];
	int timeout, nfds, i;

	while (!s->shutdown) {
		if (s->reload) {
			s->reload = 0;
			return SUPERVISOR_RELOAD;
		}
		reap_children(s);
		check_ready_files(s);
		timeout = start_due(s);
		/* files can't be polled: check again soon while someone waits on one */
		if (check_ready_files(s))
			timeout = min_timeout(timeout, READY_FILE_POLL_MS);
		timeout = min_timeout(timeout, check_ready_timeouts(s));
		timeout = min_timeout(timeout, watchdog_due());
		timeout = min_timeout(timeout, check_stop_deadlines(s));
		check_boot_done(s);
		if (s->shutdown)
			break;

		nfds = 0;
		pfd[nfds].fd = s->wake_fd[0];
		pfd[nfds].events = POLLIN;
		owner[nfds++] = OWNER_WAKE;
		if (s->ctl_fd >= 0) {
			pfd[nfds].fd = s->ctl_fd;
			pfd[nfds].events = POLLIN;
			owner[nfds++] = OWNER_CONTROL;
		}
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
			if (owner[i] == OWNER_WAKE)
				drain_wake(s);
			else if (owner[i] == OWNER_CONTROL)
				handle_control(s);
			else if (owner[i] >= MINIT_MAX_SERVICES)
				output_handle(owner[i] - MINIT_MAX_SERVICES,
					      &s->config.services[owner[i] - MINIT_MAX_SERVICES]);
			else
				handle_notify(s, owner[i]);
		}
	}

	shutdown_all(s);
	return SUPERVISOR_SHUTDOWN;
}

void supervisor_request_reload(supervisor_t *s)
{
	s->reload = 1;
	supervisor_wake(s);
}

/* Stop a service that a reload removed; keep track of it until it is reaped. */
static void retire(supervisor_t *s, pid_t pid)
{
	int i;

	kill(-pid, SIGTERM);
	for (i = 0; i < MINIT_MAX_SERVICES; i++) {
		if (!s->retired[i]) {
			s->retired[i] = pid;
			s->retired_kill_at[i] = now_ms() + STOP_TIMEOUT_MS;
			return;
		}
	}
	kill(-pid, SIGKILL);  /* no slot left */
}

void supervisor_reload(supervisor_t *s, const config_t *cfg, const int *order)
{
	int old_of_new[MINIT_MAX_SERVICES], claimed[MINIT_MAX_SERVICES] = { 0 };
	int changed[MINIT_MAX_SERVICES] = { 0 };
	int i, j, added = 0, nchanged = 0, removed = 0;
	supervisor_t *o;

	o = malloc(sizeof(*o));  /* the old state; too big for PID 1's stack */
	if (!o) {
		fprintf(stderr, "minit: reload: out of memory\n");
		return;
	}
	memcpy(o, s, sizeof(*o));

	for (j = 0; j < cfg->nservices; j++) {
		i = find_by_name(o, cfg->services[j].name);
		old_of_new[j] = i;
		if (i < 0) {
			s->pids[j] = 0;
			s->states[j] = STATE_PENDING;
			s->restart_count[j] = 0;
			s->spawn_ms[j] = 0;
			s->restart_at[j] = 0;
			s->notify_fd[j] = -1;
			s->timed_out[j] = s->held[j] = s->restart_req[j] = 0;
			s->kill_at[j] = 0;
			added++;
			continue;
		}
		claimed[i] = 1;
		s->pids[j] = o->pids[i];
		s->states[j] = o->states[i];
		s->restart_count[j] = o->restart_count[i];
		s->spawn_ms[j] = o->spawn_ms[i];
		s->restart_at[j] = o->restart_at[i];
		s->notify_fd[j] = o->notify_fd[i];
		s->timed_out[j] = o->timed_out[i];
		s->held[j] = o->held[i];
		s->restart_req[j] = o->restart_req[i];
		s->kill_at[j] = o->kill_at[i];
		/* config_parse() memsets every service, so memcmp is exact */
		changed[j] = memcmp(&o->config.services[i], &cfg->services[j], sizeof(service_t)) != 0;
		nchanged += changed[j];
	}
	for (i = 0; i < o->nservices; i++) {
		if (claimed[i])
			continue;
		removed++;
		if (o->pids[i] > 0)
			retire(s, o->pids[i]);
		if (o->notify_fd[i] >= 0)
			close(o->notify_fd[i]);
	}

	if (strcmp(o->config.watchdog, cfg->watchdog) != 0 ||
	    o->config.watchdog_timeout != cfg->watchdog_timeout)
		fprintf(stderr, "minit: reload: [system] changes take effect after a restart\n");
	memcpy(&s->config, cfg, sizeof(s->config));
	s->nservices = cfg->nservices;
	memcpy(s->order, order, sizeof(s->order));
	output_remap(old_of_new, cfg->nservices, o->nservices);

	for (j = 0; j < s->nservices; j++) {
		if (!changed[j] || s->held[j])
			continue;
		if (o->config.services[old_of_new[j]].log != cfg->services[j].log)
			output_reset(j);
		if (s->pids[j] > 0) {
			s->restart_req[j] = 1;  /* start again with the new config once it exits */
			stop_service(s, j);
		} else if (s->states[j] != STATE_STARTING) {
			/* stopped, failed or done: give the new config a fresh try */
			s->states[j] = STATE_PENDING;
			s->restart_count[j] = 0;
			s->restart_at[j] = 0;
		}
	}
	free(o);
	fprintf(stderr, "minit: reloaded: %d added, %d changed, %d removed\n",
		added, nchanged, removed);
}

void supervisor_wake(supervisor_t *s)
{
	int saved = errno;
	char c = 0;
	(void)!write(s->wake_fd[1], &c, 1);
	errno = saved;
}

void supervisor_request_action(supervisor_t *s, shutdown_action_t action)
{
	s->action = (int)action;
	supervisor_request_shutdown(s);
}

int supervisor_action(const supervisor_t *s)
{
	return s->action;
}

void supervisor_request_shutdown(supervisor_t *s)
{
	s->shutdown = 1;
	supervisor_wake(s);
}
