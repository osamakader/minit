#include "minit.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
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

static void parse_error(const char *path, int lineno, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "minit: %s:%d: ", path, lineno);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Strict decimal parse into [min, max]. Returns 0 on success. */
static int parse_int(const char *v, int min, int max, int *out)
{
	char *end;
	long n;
	errno = 0;
	n = strtol(v, &end, 10);
	if (errno || end == v || *end || n < min || n > max)
		return -1;
	*out = (int)n;
	return 0;
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
	int r, lineno = 0, errors = 0;
	int bad_section = 0;  /* inside a [section] that was already reported */

	memset(cfg, 0, sizeof(*cfg));

	f = fopen(path, "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		char *eq, *key, *val;

		lineno++;
		/* strip newline */
		line[strcspn(line, "\n")] = '\0';
		trim(line);

		if (!*line || *line == '#')
			continue;

		if (line[0] == '[') {
			/* [service name] */
			char *end = strchr(line, ']');
			cur = NULL;
			bad_section = 1;
			if (!end || end[1] || strncmp(line, "[service ", 9) != 0) {
				parse_error(path, lineno, "expected [service <name>]");
				errors++;
				continue;
			}
			key = line + 9;
			*end = '\0';
			trim(key);
			if (!*key) {
				parse_error(path, lineno, "service has no name");
				errors++;
				continue;
			}
			if (cfg->nservices >= MINIT_MAX_SERVICES)
				continue;
			bad_section = 0;
			cur = &cfg->services[cfg->nservices++];
			service_defaults(cur);
			strncpy(cur->name, key, MINIT_NAME_LEN - 1);
			cur->name[MINIT_NAME_LEN - 1] = '\0';
			continue;
		}

		eq = strchr(line, '=');
		if (!eq) {
			parse_error(path, lineno, "expected key = value");
			errors++;
			continue;
		}
		if (!cur) {
			if (!bad_section) {
				parse_error(path, lineno, "key outside a [service] section");
				errors++;
			}
			continue;
		}

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
			if (r < 0) {
				parse_error(path, lineno, "restart must be always, on-failure or never");
				errors++;
			} else {
				cur->restart = r;
			}
		} else if (strcmp(key, "depends") == 0) {
			parse_depends(val, cur);
		} else if (strcmp(key, "provides") == 0) {
			strncpy(cur->provides, val, MINIT_PROVIDES_LEN - 1);
			cur->provides[MINIT_PROVIDES_LEN - 1] = '\0';
		} else if (strcmp(key, "respawn_delay") == 0) {
			if (parse_int(val, 1, 60, &cur->respawn_delay) < 0) {
				parse_error(path, lineno, "respawn_delay must be 1..60");
				errors++;
			}
		} else if (strcmp(key, "respawn_max") == 0) {
			if (parse_int(val, 0, 1000000, &cur->respawn_max) < 0) {
				parse_error(path, lineno, "respawn_max must be a number >= 0");
				errors++;
			}
		} else if (strcmp(key, "respawn_window") == 0) {
			if (parse_int(val, 1, 86400, &cur->respawn_window) < 0) {
				parse_error(path, lineno, "respawn_window must be 1..86400");
				errors++;
			}
		} else {
			parse_error(path, lineno, "unknown key '%s'", key);
			errors++;
		}
	}

	fclose(f);
	return errors ? -1 : 0;
}
