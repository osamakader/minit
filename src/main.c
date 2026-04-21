#include "minit.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG "/etc/minit.conf"

static supervisor_t *g_supervisor;

static void on_signal(int sig)
{
	(void)sig;
	if (g_supervisor)
		supervisor_request_shutdown(g_supervisor);
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

	if (argc > 2 || (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))) {
		fprintf(stderr, "Usage: %s [config file]\n", argv[0]);
		fprintf(stderr, "Default config: %s\n", DEFAULT_CONFIG);
		fprintf(stderr, "Environment variable: MINIT_CONFIG\n");
		fprintf(stderr, "Example: MINIT_CONFIG=/etc/minit.conf %s\n", argv[0]);
		return 1;
	}

	env_config = getenv("MINIT_CONFIG");
	if (argc == 2)
		config_path = argv[1];
	else
		config_path = (env_config && env_config[0]) ? env_config : DEFAULT_CONFIG;

	if (access(config_path, F_OK) != 0) {
		fprintf(stderr, "minit: config file does not exist: %s\n", config_path);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (config_parse(config_path, &cfg) < 0) {
		fprintf(stderr, "minit: cannot read or parse config %s\n", config_path);
		return 1;
	}

	if (cfg.nservices == 0) {
		fprintf(stderr, "minit: no services in config\n");
		return 1;
	}

	if (deps_resolve(&cfg, order) < 0) {
		fprintf(stderr, "minit: dependency cycle detected\n");
		return 1;
	}

	if (log_init(NULL) < 0) {
		fprintf(stderr, "minit: cannot init log\n");
		return 1;
	}

	g_supervisor = supervisor_create(&cfg, order);
	if (!g_supervisor) {
		fprintf(stderr, "minit: out of memory\n");
		return 1;
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGCHLD, SIG_DFL);

	started = supervisor_start_all(g_supervisor);
	if (started < 0) {
		fprintf(stderr, "minit: failed to start services\n");
		supervisor_destroy(g_supervisor);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	boot_ms = (int)((t1.tv_sec - t0.tv_sec) * 1000 +
			(t1.tv_nsec - t0.tv_nsec) / 1000000);
	log_boot(boot_ms, cfg.nservices, started);

	supervisor_run(g_supervisor);
	supervisor_destroy(g_supervisor);
	g_supervisor = NULL;

	return 0;
}
