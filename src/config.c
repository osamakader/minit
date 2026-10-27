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
	svc->log = LOG_CONSOLE;
	svc->log_size_kb = 256;
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

/* Copy v into dst. Returns -1 instead of silently truncating. */
static int copy_value(char *dst, size_t size, const char *v)
{
	if (strlen(v) >= size)
		return -1;
	strcpy(dst, v);
	return 0;
}

/* Parse a comma-separated list of names into list[max]. Returns an error or NULL. */
static const char *parse_names(const char *v, char list[][MINIT_PROVIDES_LEN], int max, int *n)
{
	char buf[CONFIG_LINE_LEN];
	char *p, *tok;
	strcpy(buf, v);  /* v comes from a line of at most CONFIG_LINE_LEN */
	*n = 0;
	for (p = buf; (tok = strtok(p, ",")); p = NULL) {
		trim(tok);
		if (!*tok)
			continue;
		if (*n >= max)
			return "too many names";
		if (copy_value(list[*n], MINIT_PROVIDES_LEN, tok) < 0)
			return "name too long";
		(*n)++;
	}
	return NULL;
}

/* Names become log file names, so keep them to [A-Za-z0-9._-] and not "." or "..". */
static int valid_name(const char *name)
{
	const char *p;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	for (p = name; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-')
			return 0;
	}
	return 1;
}

static int service_exists(const config_t *cfg, const char *name)
{
	int i;
	for (i = 0; i < cfg->nservices; i++) {
		if (strcmp(cfg->services[i].name, name) == 0)
			return 1;
	}
	return 0;
}

/* One key of the [system] section. Returns the number of errors (0 or 1). */
static int parse_system_key(const char *path, int lineno, config_t *cfg,
			    const char *key, const char *val)
{
	if (strcmp(key, "watchdog") == 0) {
		if (val[0] != '/' || copy_value(cfg->watchdog, MINIT_EXEC_LEN, val) < 0) {
			parse_error(path, lineno, "watchdog must be an absolute path");
			return 1;
		}
	} else if (strcmp(key, "watchdog_timeout") == 0) {
		if (parse_int(val, 1, 600, &cfg->watchdog_timeout) < 0) {
			parse_error(path, lineno, "watchdog_timeout must be 1..600 (seconds)");
			return 1;
		}
	} else {
		parse_error(path, lineno, "unknown [system] key '%s'", key);
		return 1;
	}
	return 0;
}

int config_parse(const char *path, config_t *cfg)
{
	FILE *f;
	char line[CONFIG_LINE_LEN];
	service_t *cur = NULL;
	const char *err;
	int i, r, lineno = 0, errors = 0;
	int bad_section = 0;  /* inside a [section] that was already reported */
	int in_system = 0, seen_system = 0;  /* [system] section */

	memset(cfg, 0, sizeof(*cfg));

	f = fopen(path, "r");
	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		char *eq, *key, *val;

		lineno++;
		if (!strchr(line, '\n') && !feof(f)) {
			int c;
			parse_error(path, lineno, "line too long (max %d)", CONFIG_LINE_LEN - 2);
			errors++;
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
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
			in_system = 0;
			if (strcmp(line, "[system]") == 0) {
				if (seen_system) {
					parse_error(path, lineno, "duplicate [system] section");
					errors++;
					continue;
				}
				seen_system = in_system = 1;
				bad_section = 0;
				continue;
			}
			if (!end || end[1] || strncmp(line, "[service ", 9) != 0) {
				parse_error(path, lineno, "expected [system] or [service <name>]");
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
			if (cfg->nservices >= MINIT_MAX_SERVICES) {
				parse_error(path, lineno, "too many services (max %d)", MINIT_MAX_SERVICES);
				errors++;
				continue;
			}
			if (strlen(key) >= MINIT_NAME_LEN) {
				parse_error(path, lineno, "service name too long");
				errors++;
				continue;
			}
			if (!valid_name(key)) {
				parse_error(path, lineno, "service name may only use A-Z a-z 0-9 . _ -");
				errors++;
				continue;
			}
			if (service_exists(cfg, key)) {
				parse_error(path, lineno, "duplicate service '%s'", key);
				errors++;
				continue;
			}
			bad_section = 0;
			cur = &cfg->services[cfg->nservices++];
			service_defaults(cur);
			strcpy(cur->name, key);
			continue;
		}

		eq = strchr(line, '=');
		if (!eq) {
			parse_error(path, lineno, "expected key = value");
			errors++;
			continue;
		}
		if (!cur && !in_system) {
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

		if (in_system) {
			errors += parse_system_key(path, lineno, cfg, key, val);
			continue;
		}

		if (strcmp(key, "exec") == 0) {
			char buf[MINIT_EXEC_LEN];
			char *argv[MINIT_MAX_ARGS + 1];
			if (copy_value(cur->exec, MINIT_EXEC_LEN, val) < 0) {
				parse_error(path, lineno, "exec too long (max %d)", MINIT_EXEC_LEN - 1);
				errors++;
			} else if (exec_split(val, buf, sizeof(buf), argv) <= 0) {
				parse_error(path, lineno, "exec is empty or has an unterminated quote");
				errors++;
			}
		} else if (strcmp(key, "restart") == 0) {
			r = parse_restart(val);
			if (r < 0) {
				parse_error(path, lineno, "restart must be always, on-failure or never");
				errors++;
			} else {
				cur->restart = r;
			}
		} else if (strcmp(key, "type") == 0) {
			if (strcmp(val, "simple") == 0) {
				cur->oneshot = 0;
			} else if (strcmp(val, "oneshot") == 0) {
				cur->oneshot = 1;
			} else {
				parse_error(path, lineno, "type must be simple or oneshot");
				errors++;
			}
		} else if (strcmp(key, "ready_fd") == 0) {
			if (parse_int(val, 3, 63, &cur->ready_fd) < 0) {
				parse_error(path, lineno, "ready_fd must be 3..63");
				errors++;
			}
		} else if (strcmp(key, "ready_file") == 0) {
			if (val[0] != '/' || copy_value(cur->ready_file, MINIT_EXEC_LEN, val) < 0) {
				parse_error(path, lineno, "ready_file must be an absolute path");
				errors++;
			}
		} else if (strcmp(key, "log") == 0) {
			if (strcmp(val, "console") == 0) {
				cur->log = LOG_CONSOLE;
			} else if (strcmp(val, "null") == 0) {
				cur->log = LOG_NULL;
			} else if (strcmp(val, "file") == 0) {
				cur->log = LOG_FILE;
			} else if (strcmp(val, "kmsg") == 0) {
				cur->log = LOG_KMSG;
			} else {
				parse_error(path, lineno, "log must be console, null, file or kmsg");
				errors++;
			}
		} else if (strcmp(key, "log_size") == 0) {
			if (parse_int(val, 4, 65536, &cur->log_size_kb) < 0) {
				parse_error(path, lineno, "log_size must be 4..65536 (KB)");
				errors++;
			}
		} else if (strcmp(key, "ready_timeout") == 0) {
			if (parse_int(val, 1, 3600, &cur->ready_timeout) < 0) {
				parse_error(path, lineno, "ready_timeout must be 1..3600 (seconds)");
				errors++;
			}
		} else if (strcmp(key, "critical") == 0) {
			if (strcmp(val, "yes") == 0) {
				cur->critical = 1;
			} else if (strcmp(val, "no") == 0) {
				cur->critical = 0;
			} else {
				parse_error(path, lineno, "critical must be yes or no");
				errors++;
			}
		} else if (strcmp(key, "depends") == 0) {
			err = parse_names(val, cur->depends, MINIT_MAX_DEPS, &cur->ndeps);
			if (err) {
				parse_error(path, lineno, "depends: %s (max %d)", err, MINIT_MAX_DEPS);
				errors++;
			}
		} else if (strcmp(key, "provides") == 0) {
			err = parse_names(val, cur->provides, MINIT_MAX_PROVIDES, &cur->nprovides);
			if (err) {
				parse_error(path, lineno, "provides: %s (max %d)", err, MINIT_MAX_PROVIDES);
				errors++;
			}
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

	for (i = 0; i < cfg->nservices; i++) {
		const service_t *svc = &cfg->services[i];
		if (!svc->exec[0]) {
			fprintf(stderr, "minit: %s: service '%s' has no exec\n", path, svc->name);
			errors++;
		}
		if ((svc->oneshot + !!svc->ready_fd + !!svc->ready_file[0]) > 1) {
			fprintf(stderr, "minit: %s: service '%s': use only one of type = oneshot, "
				"ready_fd and ready_file\n", path, svc->name);
			errors++;
		}
		if (svc->ready_timeout && !svc->oneshot && !svc->ready_fd && !svc->ready_file[0]) {
			fprintf(stderr, "minit: %s: service '%s': ready_timeout needs type = oneshot, "
				"ready_fd or ready_file\n", path, svc->name);
			errors++;
		}
		if (svc->oneshot && svc->restart == RESTART_ALWAYS) {
			fprintf(stderr, "minit: %s: oneshot service '%s' cannot use restart = always\n",
				path, svc->name);
			errors++;
		}
	}
	return errors ? -1 : 0;
}
