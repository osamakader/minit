#include "minit.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG "/etc/minit.conf"
#define RETRY_ENV      "MINIT_RETRY"  /* set across the re-exec after a fatal error */

static supervisor_t *g_supervisor;
static int is_pid1;
static char **g_argv;
static volatile sig_atomic_t g_action = SHUTDOWN_POWEROFF;

static int fail(void)
{
	if (is_pid1) {
		fprintf(stderr, "minit: fatal error during boot\n");
		if (system_emergency_shell() == 0) {
			sleep(1);  /* don't spin if the shell exits at once, e.g. no console */
			setenv(RETRY_ENV, "1", 1);
			/* argv[0] first: exec via /proc/self/exe renames the process to "exe" */
			execv(g_argv[0], g_argv);
			execv("/proc/self/exe", g_argv);
		}
		fprintf(stderr, "minit: no emergency shell, halting\n");
		system_hang();
	}
	return 1;
}

/* BusyBox init convention, so its reboot/halt/poweroff applets work as PID 1 */
static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		g_action = SHUTDOWN_HALT;
	else if (sig == SIGUSR2)
		g_action = SHUTDOWN_POWEROFF;
	else
		g_action = SHUTDOWN_REBOOT;  /* SIGTERM, SIGINT (Ctrl-Alt-Del) */
	if (g_supervisor)
		supervisor_request_shutdown(g_supervisor);
}

static void on_child(int sig)
{
	(void)sig;
	if (g_supervisor)
		supervisor_wake(g_supervisor);
}

int main(int argc, char **argv)
{
	config_t cfg;
	int order[MINIT_MAX_SERVICES];
	struct timespec t0, t1;
	int boot_ms;
	int started = 0;
	const char *config_path;
	char *env_config;
	char cmdline_config[MINIT_EXEC_LEN] = "";
	int single = 0, retry;
	struct sigaction sa;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	g_argv = argv;
	is_pid1 = (getpid() == 1);
	if (is_pid1)
		system_mount_early();
	system_setup_stdio(is_pid1);

	retry = getenv(RETRY_ENV) != NULL;
	unsetenv(RETRY_ENV);  /* keep it out of the services' environment */
	if (is_pid1)
		system_read_cmdline(cmdline_config, sizeof(cmdline_config), &single);
	if (single && !retry) {
		fprintf(stderr, "minit: single-user mode\n");
		if (system_emergency_shell() < 0)
			fprintf(stderr, "minit: no emergency shell, continuing boot\n");
	}

	/* as PID 1, argv holds leftover kernel cmdline words, not our arguments */
	if (!is_pid1 && (argc > 2 || (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)))) {
		fprintf(stderr, "Usage: %s [config file]\n", argv[0]);
		fprintf(stderr, "Default config: %s\n", DEFAULT_CONFIG);
		fprintf(stderr, "Environment variable: MINIT_CONFIG\n");
		fprintf(stderr, "Example: MINIT_CONFIG=/etc/minit.conf %s\n", argv[0]);
		return 1;
	}

	env_config = getenv("MINIT_CONFIG");
	if (!is_pid1 && argc == 2)
		config_path = argv[1];
	else if (cmdline_config[0])
		config_path = cmdline_config;
	else
		config_path = (env_config && env_config[0]) ? env_config : DEFAULT_CONFIG;

	if (access(config_path, F_OK) != 0) {
		fprintf(stderr, "minit: config file does not exist: %s\n", config_path);
		return fail();
	}

	if (config_parse(config_path, &cfg) < 0) {
		fprintf(stderr, "minit: cannot read or parse config %s\n", config_path);
		return fail();
	}

	if (cfg.nservices == 0) {
		fprintf(stderr, "minit: no services in config\n");
		return fail();
	}

	if (deps_resolve(&cfg, order) < 0) {
		fprintf(stderr, "minit: invalid service dependencies\n");
		return fail();
	}

	if (log_init(NULL) < 0) {
		fprintf(stderr, "minit: cannot init log\n");
		return fail();
	}

	g_supervisor = supervisor_create(&cfg, order);
	if (!g_supervisor) {
		fprintf(stderr, "minit: out of memory\n");
		return fail();
	}

	/* handlers only set flags and write to the supervisor's self-pipe */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	if (is_pid1) {
		sigaction(SIGUSR1, &sa, NULL);
		sigaction(SIGUSR2, &sa, NULL);
		reboot(RB_DISABLE_CAD);  /* Ctrl-Alt-Del sends SIGINT instead of rebooting */
	}
	sa.sa_handler = on_child;
	sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);

	started = supervisor_start_all(g_supervisor);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	boot_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
			(t1.tv_nsec - t0.tv_nsec) / 1000000);
	log_boot(boot_ms, cfg.nservices, started);

	supervisor_run(g_supervisor);
	supervisor_destroy(g_supervisor);
	g_supervisor = NULL;

	if (is_pid1)
		system_shutdown(g_action);
	return 0;
}
