#include "minit.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG "/etc/minit.conf"

static supervisor_t *g_supervisor;
static int is_pid1;

/* PID 1 must never exit or the kernel panics: stay alive and keep reaping orphans. */
static void hang(void)
{
	for (;;) {
		if (waitpid(-1, NULL, 0) < 0)
			sleep(1);
	}
}

static int fail(void)
{
	if (is_pid1) {
		fprintf(stderr, "minit: fatal error, halting\n");
		hang();
	}
	return 1;
}

static void on_signal(int sig)
{
	(void)sig;
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
	struct sigaction sa;

	is_pid1 = (getpid() == 1);

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
	else
		config_path = (env_config && env_config[0]) ? env_config : DEFAULT_CONFIG;

	if (access(config_path, F_OK) != 0) {
		fprintf(stderr, "minit: config file does not exist: %s\n", config_path);
		return fail();
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (config_parse(config_path, &cfg) < 0) {
		fprintf(stderr, "minit: cannot read or parse config %s\n", config_path);
		return fail();
	}

	if (cfg.nservices == 0) {
		fprintf(stderr, "minit: no services in config\n");
		return fail();
	}

	if (deps_resolve(&cfg, order) < 0) {
		fprintf(stderr, "minit: dependency cycle detected\n");
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
	sa.sa_handler = on_child;
	sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);

	started = supervisor_start_all(g_supervisor);
	if (started < 0) {
		fprintf(stderr, "minit: failed to start services\n");
		supervisor_destroy(g_supervisor);
		return fail();
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	boot_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
			(t1.tv_nsec - t0.tv_nsec) / 1000000);
	log_boot(boot_ms, cfg.nservices, started);

	supervisor_run(g_supervisor);
	supervisor_destroy(g_supervisor);
	g_supervisor = NULL;

	if (is_pid1) {
		sync();
		reboot(RB_POWER_OFF);
		hang();
	}
	return 0;
}
