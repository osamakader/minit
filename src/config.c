#include "config_internal.h"
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
	svc->umask = -1;
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

void config_error(const char *path, int lineno, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "minit: %s:%d: ", path, lineno);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Strict parse in the given base into [min, max]. Returns 0 on success. */
static int parse_int_base(const char *v, int base, int min, int max, int *out)
{
	char *end;
	long n;
	errno = 0;
	n = strtol(v, &end, base);
	if (errno || end == v || *end || n < min || n > max)
		return -1;
	*out = (int)n;
	return 0;
}

static int parse_int(const char *v, int min, int max, int *out)
{
	return parse_int_base(v, 10, min, max, out);
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

/* user and group: a name or a number */
static int valid_id(const char *v)
{
	return v[0] && strlen(v) < MINIT_ID_LEN && valid_name(v);
}

/* KEY=VALUE with KEY matching [A-Za-z_][A-Za-z0-9_]* */
int config_valid_env(const char *v)
{
	const char *p = v;
	if (!isalpha((unsigned char)*p) && *p != '_')
		return 0;
	while (isalnum((unsigned char)*p) || *p == '_')
		p++;
	return *p == '=';
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
int config_system_key(const char *path, int lineno, config_t *cfg,
		      const char *key, const char *val)
{
	if (strcmp(key, "watchdog") == 0) {
		if (val[0] != '/' || copy_value(cfg->watchdog, MINIT_EXEC_LEN, val) < 0) {
			config_error(path, lineno, "watchdog must be an absolute path");
			return 1;
		}
	} else if (strcmp(key, "watchdog_timeout") == 0) {
		if (parse_int(val, 1, 600, &cfg->watchdog_timeout) < 0) {
			config_error(path, lineno, "watchdog_timeout must be 1..600 (seconds)");
			return 1;
		}
	} else {
		config_error(path, lineno, "unknown [system] key '%s'", key);
		return 1;
	}
	return 0;
}

/* One key of a service. Returns the number of errors (0 or 1). */
int config_service_key(const char *path, int lineno, service_t *cur,
		       const char *key, const char *val)
{
	const char *err;
	int r;

	if (strcmp(key, "exec") == 0) {
		char buf[MINIT_EXEC_LEN];
		char *argv[MINIT_MAX_ARGS + 1];
		if (copy_value(cur->exec, MINIT_EXEC_LEN, val) < 0) {
			config_error(path, lineno, "exec too long (max %d)", MINIT_EXEC_LEN - 1);
			return 1;
		} else if (exec_split(val, buf, sizeof(buf), argv) <= 0) {
			config_error(path, lineno, "exec is empty or has an unterminated quote");
			return 1;
		}
	} else if (strcmp(key, "restart") == 0) {
		r = parse_restart(val);
		if (r < 0) {
			config_error(path, lineno, "restart must be always, on-failure or never");
			return 1;
		} else {
			cur->restart = r;
		}
	} else if (strcmp(key, "type") == 0) {
		if (strcmp(val, "simple") == 0) {
			cur->oneshot = 0;
		} else if (strcmp(val, "oneshot") == 0) {
			cur->oneshot = 1;
		} else {
			config_error(path, lineno, "type must be simple or oneshot");
			return 1;
		}
	} else if (strcmp(key, "ready_fd") == 0) {
		if (parse_int(val, 3, 63, &cur->ready_fd) < 0) {
			config_error(path, lineno, "ready_fd must be 3..63");
			return 1;
		}
	} else if (strcmp(key, "ready_file") == 0) {
		if (val[0] != '/' || copy_value(cur->ready_file, MINIT_EXEC_LEN, val) < 0) {
			config_error(path, lineno, "ready_file must be an absolute path");
			return 1;
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
			config_error(path, lineno, "log must be console, null, file or kmsg");
			return 1;
		}
	} else if (strcmp(key, "log_size") == 0) {
		if (parse_int(val, 4, 65536, &cur->log_size_kb) < 0) {
			config_error(path, lineno, "log_size must be 4..65536 (KB)");
			return 1;
		}
	} else if (strcmp(key, "ready_timeout") == 0) {
		if (parse_int(val, 1, 3600, &cur->ready_timeout) < 0) {
			config_error(path, lineno, "ready_timeout must be 1..3600 (seconds)");
			return 1;
		}
	} else if (strcmp(key, "critical") == 0) {
		if (strcmp(val, "yes") == 0) {
			cur->critical = 1;
		} else if (strcmp(val, "no") == 0) {
			cur->critical = 0;
		} else {
			config_error(path, lineno, "critical must be yes or no");
			return 1;
		}
	} else if (strcmp(key, "user") == 0 || strcmp(key, "group") == 0) {
		if (!valid_id(val)) {
			config_error(path, lineno, "%s must be a name or a number", key);
			return 1;
		} else {
			strcpy(key[0] == 'u' ? cur->user : cur->group, val);
		}
	} else if (strcmp(key, "workdir") == 0) {
		if (val[0] != '/' || copy_value(cur->workdir, MINIT_EXEC_LEN, val) < 0) {
			config_error(path, lineno, "workdir must be an absolute path");
			return 1;
		}
	} else if (strcmp(key, "umask") == 0) {
		if (parse_int_base(val, 8, 0, 0777, &cur->umask) < 0) {
			config_error(path, lineno, "umask must be octal, 0..0777");
			return 1;
		}
	} else if (strcmp(key, "nice") == 0) {
		if (parse_int(val, -20, 19, &cur->nice) < 0) {
			config_error(path, lineno, "nice must be -20..19");
			return 1;
		}
		cur->set_nice = 1;
	} else if (strcmp(key, "env") == 0) {
		if (!config_valid_env(val)) {
			config_error(path, lineno, "env must be KEY=VALUE");
			return 1;
		} else if (cur->nenv >= MINIT_MAX_ENV) {
			config_error(path, lineno, "too many env lines (max %d)", MINIT_MAX_ENV);
			return 1;
		} else if (copy_value(cur->env[cur->nenv], MINIT_ENV_LEN, val) < 0) {
			config_error(path, lineno, "env too long (max %d)", MINIT_ENV_LEN - 1);
			return 1;
		} else {
			cur->nenv++;
		}
	} else if (strcmp(key, "env_file") == 0) {
		if (val[0] != '/' || copy_value(cur->env_file, MINIT_EXEC_LEN, val) < 0) {
			config_error(path, lineno, "env_file must be an absolute path");
			return 1;
		}
	} else if (strcmp(key, "depends") == 0) {
		err = parse_names(val, cur->depends, MINIT_MAX_DEPS, &cur->ndeps);
		if (err) {
			config_error(path, lineno, "depends: %s (max %d)", err, MINIT_MAX_DEPS);
			return 1;
		}
	} else if (strcmp(key, "provides") == 0) {
		err = parse_names(val, cur->provides, MINIT_MAX_PROVIDES, &cur->nprovides);
		if (err) {
			config_error(path, lineno, "provides: %s (max %d)", err, MINIT_MAX_PROVIDES);
			return 1;
		}
	} else if (strcmp(key, "respawn_delay") == 0) {
		if (parse_int(val, 1, 60, &cur->respawn_delay) < 0) {
			config_error(path, lineno, "respawn_delay must be 1..60");
			return 1;
		}
	} else if (strcmp(key, "respawn_max") == 0) {
		if (parse_int(val, 0, 1000000, &cur->respawn_max) < 0) {
			config_error(path, lineno, "respawn_max must be a number >= 0");
			return 1;
		}
	} else if (strcmp(key, "respawn_window") == 0) {
		if (parse_int(val, 1, 86400, &cur->respawn_window) < 0) {
			config_error(path, lineno, "respawn_window must be 1..86400");
			return 1;
		}
	} else {
		config_error(path, lineno, "unknown key '%s'", key);
		return 1;
	}
	return 0;
}

/* Start a new service called name. Returns NULL (after reporting why) if it cannot be added. */
service_t *config_add_service(const char *path, int lineno, config_t *cfg, const char *name)
{
	service_t *svc;

	if (!*name) {
		config_error(path, lineno, "service has no name");
		return NULL;
	}
	if (cfg->nservices >= MINIT_MAX_SERVICES) {
		config_error(path, lineno, "too many services (max %d)", MINIT_MAX_SERVICES);
		return NULL;
	}
	if (strlen(name) >= MINIT_NAME_LEN) {
		config_error(path, lineno, "service name too long");
		return NULL;
	}
	if (!valid_name(name)) {
		config_error(path, lineno, "service name may only use A-Z a-z 0-9 . _ -");
		return NULL;
	}
	if (service_exists(cfg, name)) {
		config_error(path, lineno, "duplicate service '%s'", name);
		return NULL;
	}
	svc = &cfg->services[cfg->nservices++];
	service_defaults(svc);
	strcpy(svc->name, name);
	return svc;
}

/* Checks that need the whole service. Returns the number of errors. */
int config_check_services(const char *path, const config_t *cfg)
{
	int i, errors = 0;

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
	return errors;
}

int config_parse(const char *path, config_t *cfg)
{
	FILE *f;
	char line[CONFIG_LINE_LEN];
	service_t *cur = NULL;
	int lineno = 0, errors = 0;
	int bad_section = 0;  /* inside a [section] that was already reported */
	int in_system = 0, seen_system = 0;  /* [system] section */

	memset(cfg, 0, sizeof(*cfg));

	f = fopen(path, "r");
	if (!f)
		return -1;

	/* JSON if the name ends in .json or the first non-blank character is '{' */
	{
		size_t n = strlen(path);
		int c;
		while ((c = fgetc(f)) != EOF && isspace(c))
			;
		if (c == '{' || (n > 5 && strcmp(path + n - 5, ".json") == 0)) {
			fclose(f);
#ifdef MINIT_JSON
			return config_parse_json(path, cfg);
#else
			fprintf(stderr, "minit: %s: JSON support is not compiled in\n", path);
			return -1;
#endif
		}
		rewind(f);
	}

	while (fgets(line, sizeof(line), f)) {
		char *eq, *key, *val;

		lineno++;
		if (!strchr(line, '\n') && !feof(f)) {
			int c;
			config_error(path, lineno, "line too long (max %d)", CONFIG_LINE_LEN - 2);
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
					config_error(path, lineno, "duplicate [system] section");
					errors++;
					continue;
				}
				seen_system = in_system = 1;
				bad_section = 0;
				continue;
			}
			if (!end || end[1] || strncmp(line, "[service ", 9) != 0) {
				config_error(path, lineno, "expected [system] or [service <name>]");
				errors++;
				continue;
			}
			key = line + 9;
			*end = '\0';
			trim(key);
			cur = config_add_service(path, lineno, cfg, key);
			if (!cur) {
				errors++;
				continue;
			}
			bad_section = 0;
			continue;
		}

		eq = strchr(line, '=');
		if (!eq) {
			config_error(path, lineno, "expected key = value");
			errors++;
			continue;
		}
		if (!cur && !in_system) {
			if (!bad_section) {
				config_error(path, lineno, "key outside a [service] section");
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
			errors += config_system_key(path, lineno, cfg, key, val);
			continue;
		}

		errors += config_service_key(path, lineno, cur, key, val);
	}

	fclose(f);

	errors += config_check_services(path, cfg);
	return errors ? -1 : 0;
}
