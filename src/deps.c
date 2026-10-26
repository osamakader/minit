#include "minit.h"
#include <stdio.h>
#include <string.h>

int deps_provides(const service_t *svc, const char *name)
{
	int i;
	if (strcmp(svc->name, name) == 0)
		return 1;
	for (i = 0; i < svc->nprovides; i++) {
		if (strcmp(svc->provides[i], name) == 0)
			return 1;
	}
	return 0;
}

int deps_find_provider(const config_t *cfg, const char *name)
{
	int i;
	for (i = 0; i < cfg->nservices; i++) {
		if (deps_provides(&cfg->services[i], name))
			return i;
	}
	return -1;
}

/* Warn about every other service that also provides name. */
static int check_unique(const config_t *cfg, int idx, const char *name)
{
	int j, errors = 0;
	for (j = idx + 1; j < cfg->nservices; j++) {
		if (deps_provides(&cfg->services[j], name)) {
			fprintf(stderr, "minit: '%s' and '%s' both provide '%s'\n",
				cfg->services[idx].name, cfg->services[j].name, name);
			errors++;
		}
	}
	return errors;
}

/* Every name must have one provider, and every depends must name another service. */
static int deps_check(const config_t *cfg)
{
	int i, j, prov, errors = 0;

	for (i = 0; i < cfg->nservices; i++) {
		const service_t *s = &cfg->services[i];
		errors += check_unique(cfg, i, s->name);
		for (j = 0; j < s->nprovides; j++)
			errors += check_unique(cfg, i, s->provides[j]);
		for (j = 0; j < s->ndeps; j++) {
			prov = deps_find_provider(cfg, s->depends[j]);
			if (prov < 0) {
				fprintf(stderr, "minit: '%s' depends on '%s', which no service provides\n",
					s->name, s->depends[j]);
				errors++;
			} else if (prov == i) {
				fprintf(stderr, "minit: '%s' depends on itself\n", s->name);
				errors++;
			}
		}
	}
	return errors ? -1 : 0;
}

/* Kahn's algorithm for topological sort. Returns 0 on success, -1 on bad deps or cycle. */
int deps_resolve(config_t *cfg, int order[MINIT_MAX_SERVICES])
{
	int in_degree[MINIT_MAX_SERVICES];
	int queue[MINIT_MAX_SERVICES];
	int qhead = 0, qtail = 0;
	int sorted = 0;
	int i, j;

	if (deps_check(cfg) < 0)
		return -1;

	/* in-degree = number of depends entries; each names another service (checked above) */
	for (i = 0; i < cfg->nservices; i++)
		in_degree[i] = cfg->services[i].ndeps;

	/* Enqueue services with no dependencies */
	for (i = 0; i < cfg->nservices; i++) {
		if (in_degree[i] == 0)
			queue[qtail++] = i;
	}

	while (qhead < qtail) {
		int u = queue[qhead++];
		order[sorted++] = u;

		/* Find services that depend on something u provides; a name listed twice counts twice */
		for (i = 0; i < cfg->nservices; i++) {
			service_t *s = &cfg->services[i];
			if (in_degree[i] <= 0) continue;
			for (j = 0; j < s->ndeps; j++) {
				if (deps_provides(&cfg->services[u], s->depends[j]))
					in_degree[i]--;
			}
			if (in_degree[i] == 0)
				queue[qtail++] = i;
		}
	}

	if (sorted != cfg->nservices) {
		fprintf(stderr, "minit: dependency cycle among:");
		for (i = 0; i < cfg->nservices; i++) {
			if (in_degree[i] > 0)
				fprintf(stderr, " %s", cfg->services[i].name);
		}
		fputc('\n', stderr);
		return -1;
	}
	return 0;
}
