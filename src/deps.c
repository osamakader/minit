#include "minit.h"
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

/* Kahn's algorithm for topological sort. Returns 0 on success, -1 on cycle. */
int deps_resolve(config_t *cfg, int order[MINIT_MAX_SERVICES])
{
	int in_degree[MINIT_MAX_SERVICES];
	int queue[MINIT_MAX_SERVICES];
	int qhead = 0, qtail = 0;
	int sorted = 0;
	int i, j, prov;

	memset(in_degree, 0, sizeof(in_degree));

	/* Build in-degree: for each service, count how many deps it has that are satisfied by other services */
	for (i = 0; i < cfg->nservices; i++) {
		service_t *s = &cfg->services[i];
		for (j = 0; j < s->ndeps; j++) {
			prov = find_provider(cfg, s->depends[j]);
			if (prov >= 0 && prov != i)
				in_degree[i]++;
		}
	}

	/* Enqueue services with no dependencies */
	for (i = 0; i < cfg->nservices; i++) {
		if (in_degree[i] == 0)
			queue[qtail++] = i;
	}

	while (qhead < qtail) {
		int u = queue[qhead++];
		order[sorted++] = u;

		/* Find services that depend on what u provides */
		if (cfg->services[u].provides[0]) {
			for (i = 0; i < cfg->nservices; i++) {
				if (in_degree[i] <= 0) continue;
				service_t *s = &cfg->services[i];
				for (j = 0; j < s->ndeps; j++) {
					prov = find_provider(cfg, s->depends[j]);
					if (prov == u) {
						in_degree[i]--;
						if (in_degree[i] == 0)
							queue[qtail++] = i;
						break;
					}
				}
			}
		}
	}

	if (sorted != cfg->nservices)
		return -1;  /* cycle */
	return 0;
}
