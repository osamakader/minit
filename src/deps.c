#include "minit.h"
#include <stdio.h>
#include <string.h>

/* Find service index that provides the given name. -1 if not found. */
static int find_provider(const config_t *cfg, const char *name)
{
	int i;
	for (i = 0; i < cfg->nservices; i++) {
		if (cfg->services[i].provides[0] && strcmp(cfg->services[i].provides, name) == 0)
			return i;
	}
	return -1;
}

/* Every depends must name exactly one provider, and not the service itself. */
static int deps_check(const config_t *cfg)
{
	int i, j, prov, errors = 0;

	for (i = 0; i < cfg->nservices; i++) {
		const service_t *s = &cfg->services[i];
		prov = s->provides[0] ? find_provider(cfg, s->provides) : i;
		if (prov != i) {
			fprintf(stderr, "minit: '%s' and '%s' both provide '%s'\n",
				cfg->services[prov].name, s->name, s->provides);
			errors++;
		}
		for (j = 0; j < s->ndeps; j++) {
			prov = find_provider(cfg, s->depends[j]);
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

		/* Find services that depend on what u provides; a name listed twice counts twice */
		if (cfg->services[u].provides[0]) {
			for (i = 0; i < cfg->nservices; i++) {
				service_t *s = &cfg->services[i];
				if (in_degree[i] <= 0) continue;
				for (j = 0; j < s->ndeps; j++) {
					if (strcmp(s->depends[j], cfg->services[u].provides) == 0)
						in_degree[i]--;
				}
				if (in_degree[i] == 0)
					queue[qtail++] = i;
			}
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
