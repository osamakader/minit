#include "minit.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void service_defaults(service_t *svc)
{
	memset(svc, 0, sizeof(*svc));
	svc->restart = RESTART_ON_FAILURE;
	svc->respawn_delay = 1;
	svc->respawn_max = 0;
	svc->respawn_window = 60;
}

static void trim(char *s)
{
	char *p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	p = s + strlen(s);
	while (p > s && isspace((unsigned char)p[-1])) p--;
	*p = '\0';
}

static int parse_restart(const char *v)
{
	if (strcmp(v, "always") == 0) return RESTART_ALWAYS;
	if (strcmp(v, "on-failure") == 0) return RESTART_ON_FAILURE;
	if (strcmp(v, "never") == 0) return RESTART_NEVER;
	return -1;
}

static void parse_depends(const char *v, service_t *svc)
{
	char buf[256];
	char *p, *tok;
	strncpy(buf, v, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	svc->ndeps = 0;
	for (p = buf; svc->ndeps < MINIT_MAX_DEPS && (tok = strtok(p, ",")); p = NULL) {
		trim(tok);
		if (*tok) {
			strncpy(svc->depends[svc->ndeps], tok, MINIT_PROVIDES_LEN - 1);
			svc->depends[svc->ndeps][MINIT_PROVIDES_LEN - 1] = '\0';
			svc->ndeps++;
		}
	}
}

int config_parse(const char *path, config_t *cfg)
{
	FILE *f;
	char line[512];
	service_t *cur = NULL;
	int r;

	memset(cfg, 0, sizeof(*cfg));

	f = fopen(path, "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		char *eq, *key, *val;

		/* strip newline */
		line[strcspn(line, "\n")] = '\0';
		trim(line);

		if (!*line || *line == '#')
			continue;

		if (line[0] == '[') {
			/* [service name] */
			char *end = strchr(line, ']');
			if (!end || strncmp(line, "[service ", 9) != 0)
				continue;
			key = line + 9;
			*end = '\0';
			trim(key);
			if (!*key || cfg->nservices >= MINIT_MAX_SERVICES)
				continue;
			cur = &cfg->services[cfg->nservices++];
			service_defaults(cur);
			strncpy(cur->name, key, MINIT_NAME_LEN - 1);
			cur->name[MINIT_NAME_LEN - 1] = '\0';
			continue;
		}

		eq = strchr(line, '=');
		if (!eq || !cur)
			continue;

		*eq = '\0';
		key = line;
		val = eq + 1;
		trim(key);
		trim(val);

		if (strcmp(key, "exec") == 0) {
			strncpy(cur->exec, val, MINIT_EXEC_LEN - 1);
			cur->exec[MINIT_EXEC_LEN - 1] = '\0';
		} else if (strcmp(key, "restart") == 0) {
			r = parse_restart(val);
			if (r >= 0) cur->restart = r;
		} else if (strcmp(key, "depends") == 0) {
			parse_depends(val, cur);
		} else if (strcmp(key, "provides") == 0) {
			strncpy(cur->provides, val, MINIT_PROVIDES_LEN - 1);
			cur->provides[MINIT_PROVIDES_LEN - 1] = '\0';
		} else if (strcmp(key, "respawn_delay") == 0) {
			cur->respawn_delay = atoi(val);
			if (cur->respawn_delay < 1) cur->respawn_delay = 1;
			if (cur->respawn_delay > 60) cur->respawn_delay = 60;
		} else if (strcmp(key, "respawn_max") == 0) {
			cur->respawn_max = atoi(val);
		} else if (strcmp(key, "respawn_window") == 0) {
			cur->respawn_window = atoi(val);
			if (cur->respawn_window < 1) cur->respawn_window = 60;
		}
	}

	fclose(f);
	return 0;
}
