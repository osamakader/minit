#ifndef MINIT_H
#define MINIT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MINIT_MAX_SERVICES  16
#define MINIT_MAX_DEPS      8
#define MINIT_MAX_PROVIDES  4
#define MINIT_MAX_ARGS      32
#define MINIT_NAME_LEN      64
#define MINIT_EXEC_LEN      256
#define MINIT_PROVIDES_LEN  64
#define CONFIG_LINE_LEN     512

typedef enum {
	RESTART_NEVER,
	RESTART_ON_FAILURE,
	RESTART_ALWAYS
} restart_policy_t;

typedef enum {
	LOG_CONSOLE,  /* inherit minit's stdout/stderr */
	LOG_NULL,
	LOG_FILE,     /* <log dir>/<name>.log, rotated to .log.1 at log_size_kb */
	LOG_KMSG      /* one kernel log record per line */
} log_mode_t;

typedef enum {
	STATE_PENDING,   /* waiting for dependencies or a restart delay */
	STATE_STARTING,  /* spawned, not ready yet */
	STATE_RUNNING,   /* ready: dependents may start */
	STATE_DONE,      /* oneshot exited 0: dependents may start */
	STATE_STOPPED,
	STATE_FAILED     /* gave up after respawn_max quick restarts */
} service_state_t;

typedef struct {
	char name[MINIT_NAME_LEN];
	char exec[MINIT_EXEC_LEN];
	restart_policy_t restart;
	int oneshot;         /* type = oneshot: ready once it exits 0 */
	int ready_fd;        /* >= 3: ready once it writes a newline to this fd; 0 = at spawn */
	char ready_file[MINIT_EXEC_LEN];  /* ready once this path exists; removed at spawn */
	int ready_timeout;   /* seconds a service may stay STARTING before it is killed; 0 = no limit */
	int critical;        /* reboot if it fails for good */
	log_mode_t log;      /* where stdout/stderr go */
	int log_size_kb;     /* log = file: rotate at this size */
	/* the service name is always provided too, without being listed here */
	char provides[MINIT_MAX_PROVIDES][MINIT_PROVIDES_LEN];
	int nprovides;
	char depends[MINIT_MAX_DEPS][MINIT_PROVIDES_LEN];
	int ndeps;
	int respawn_delay;   /* seconds, default 1 */
	int respawn_max;     /* 0 = unlimited */
	int respawn_window;  /* seconds */
} service_t;

typedef struct {
	service_t services[MINIT_MAX_SERVICES];
	int nservices;
	/* [system] section */
	char watchdog[MINIT_EXEC_LEN];  /* device to keep alive, e.g. /dev/watchdog; empty = none */
	int watchdog_timeout;  /* seconds to program into it; 0 = leave the driver's */
} config_t;

/* config.c */
int config_parse(const char *path, config_t *cfg);

/* deps.c */
int deps_resolve(config_t *cfg, int order[MINIT_MAX_SERVICES]);
int deps_provides(const service_t *svc, const char *name);  /* its name or a provides entry */
int deps_find_provider(const config_t *cfg, const char *name);  /* service index or -1 */

/* log.c */
int log_init(const char *dir);  /* NULL = try /var/log/minit, then /tmp/minit */
void log_crash(const char *name, pid_t pid, int exit_code, int signal, int restarts);
void log_giveup(const char *name, int restarts);
void log_boot(int boot_ms, int total, int ready);
const char *log_dir(void);  /* directory chosen by log_init() */

/* watchdog.c: hardware watchdog, pinged at half its timeout */
int watchdog_open(const char *path, int timeout);
void watchdog_ping(void);
int watchdog_due(void);  /* ping if due; ms until the next ping, -1 if no watchdog */
void watchdog_disarm(void);  /* magic close, so a halted system is not reset */

/* control.c: unix socket for minitctl, one request line and one reply per connection */
#define MINIT_SOCKET "/run/minit.sock"
#define CONTROL_REPLY_LEN 2048
int control_open(const char *path);  /* listening fd, or -1 */
int control_accept(int listen_fd, char *req, size_t len);  /* client fd with req read, or -1 */
void control_reply(int fd, const char *text);  /* send and close */

/* output.c: capture service stdout/stderr (log = file | kmsg) */
int output_prepare(int idx, const service_t *svc);  /* fd for the child's 1/2, or -1 */
int output_poll_fd(int idx);  /* fd to poll for output, or -1 */
void output_handle(int idx, const service_t *svc);  /* read what is there and store it */
void output_reset(int idx);  /* close it; the next output_prepare() starts over */
/* after a reload: new service i takes over old_of_new[i] (-1 = new); unclaimed ones close */
void output_remap(const int *old_of_new, int nnew, int nold);

/* system.c: PID 1 setup */
typedef enum {
	SHUTDOWN_POWEROFF,
	SHUTDOWN_REBOOT,
	SHUTDOWN_HALT
} shutdown_action_t;

void system_mount_early(void);  /* /proc, /sys, /dev, /run, ...; skips existing mounts */
void system_setup_stdio(int pid1);  /* PID 1: fds 0-2 on /dev/console; always: 0-2 open */
void system_hang(void);  /* never returns; reaps orphans */
int system_emergency_shell(void);  /* run /bin/sh on the console until it exits; -1 if none */
/* parse /proc/cmdline: minit.config=PATH fills config, single/minit.single sets *single */
void system_read_cmdline(char *config, size_t config_len, int *single);
void system_set_hostname(void);  /* from /etc/hostname, if present */
void system_seed_load(void);  /* mix the saved random seed into /dev/urandom, then refresh it */
void system_seed_save(void);
void system_shutdown(shutdown_action_t action);  /* PID 1 only: kill all, unmount, reboot(); no return */

/* supervisor.c */
typedef struct supervisor supervisor_t;
/* boot_start_ms: CLOCK_MONOTONIC at process entry; boot ends when all services are ready */
supervisor_t *supervisor_create(config_t *cfg, const int *order, long long boot_start_ms);
void supervisor_destroy(supervisor_t *s);
void supervisor_request_shutdown(supervisor_t *s);
/* stop all services, then have main() perform action; also used by the supervisor itself */
void supervisor_request_action(supervisor_t *s, shutdown_action_t action);
int supervisor_action(const supervisor_t *s);  /* requested action, or -1 */
void supervisor_listen(supervisor_t *s, const char *socket_path);  /* serve minitctl */
void supervisor_wake(supervisor_t *s);  /* async-signal-safe, e.g. from SIGCHLD */
int supervisor_start_all(supervisor_t *s);  /* returns count started; failures are retried later */
#define SUPERVISOR_SHUTDOWN 0
#define SUPERVISOR_RELOAD   1
int supervisor_run(supervisor_t *s);  /* main loop: SUPERVISOR_SHUTDOWN after stopping all, or RELOAD */
void supervisor_request_reload(supervisor_t *s);  /* async-signal-safe */
/* switch to a new, valid config: keep unchanged services, restart changed ones, start new ones */
void supervisor_reload(supervisor_t *s, const config_t *cfg, const int *order);
/* split exec into argv (shell-style quotes, no expansion); -1 on unterminated quote */
int exec_split(const char *exec, char *buf, size_t bufsz, char *argv[MINIT_MAX_ARGS + 1]);

#endif
