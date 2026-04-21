#ifndef MINIT_H
#define MINIT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MINIT_MAX_SERVICES  16
#define MINIT_MAX_DEPS      8
#define MINIT_MAX_ARGS      32
#define MINIT_NAME_LEN      64
#define MINIT_EXEC_LEN      256
#define MINIT_PROVIDES_LEN  64

typedef enum {
	RESTART_NEVER,
	RESTART_ON_FAILURE,
	RESTART_ALWAYS
} restart_policy_t;

typedef enum {
	STATE_PENDING,
	STATE_RUNNING,
	STATE_STOPPED
} service_state_t;

typedef struct {
	char name[MINIT_NAME_LEN];
	char exec[MINIT_EXEC_LEN];
	restart_policy_t restart;
	char provides[MINIT_PROVIDES_LEN];  /* empty = no provides */
	char depends[MINIT_MAX_DEPS][MINIT_PROVIDES_LEN];
	int ndeps;
	int respawn_delay;   /* seconds, default 1 */
	int respawn_max;     /* 0 = unlimited */
	int respawn_window;  /* seconds */
} service_t;

typedef struct {
	service_t services[MINIT_MAX_SERVICES];
	int nservices;
} config_t;

/* config.c */
int config_parse(const char *path, config_t *cfg);

/* deps.c */
int deps_resolve(config_t *cfg, int order[MINIT_MAX_SERVICES]);

/* log.c */
int log_init(const char *dir);  /* NULL = use default, try /var/log/minit then /tmp/minit */
void log_crash(const char *name, pid_t pid, int exit_code, int signal, int restarts);
void log_boot(int boot_ms, int total, int started);

/* supervisor.c */
typedef struct supervisor supervisor_t;
supervisor_t *supervisor_create(config_t *cfg, const int *order);
void supervisor_destroy(supervisor_t *s);
void supervisor_request_shutdown(supervisor_t *s);
int supervisor_start_all(supervisor_t *s);  /* returns count started, or -1 on error */
int supervisor_run(supervisor_t *s);  /* main loop, returns when shutdown */

#endif
