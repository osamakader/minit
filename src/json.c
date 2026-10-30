/*
 * JSON config format, mapped onto the same keys as the conf format:
 *
 *   { "system": { "watchdog": "/dev/watchdog" },
 *     "services": [ { "name": "web", "exec": ["httpd", "-f"], "depends": ["net"] } ] }
 *
 * Arrays are accepted for exec (one argument each), depends, provides and env;
 * env may also be an object. true/false become yes/no. No allocation: the file
 * is read into a static buffer and each service's values are flattened to
 * strings before they go through config_service_key().
 */
#ifdef MINIT_JSON
#include "config_internal.h"
#include <stdio.h>
#include <string.h>

#define JSON_MAX_FILE  65536
#define MAX_PAIRS      48   /* key/value pairs per service, env entries counted separately */

struct json {
	const char *p;
	const char *path;
	int line;
	int errors;
};

struct pair {
	char key[32];
	char val[CONFIG_LINE_LEN];
	int line;
};

static char file_buf[JSON_MAX_FILE + 1];
static struct pair pairs[MAX_PAIRS];

static int syntax(struct json *j, const char *what)
{
	config_error(j->path, j->line, "%s", what);
	return -1;
}

static void skip_ws(struct json *j)
{
	for (;; j->p++) {
		if (*j->p == '\n')
			j->line++;
		else if (*j->p != ' ' && *j->p != '\t' && *j->p != '\r')
			return;
	}
}

static int expect(struct json *j, char c)
{
	char msg[24];
	skip_ws(j);
	if (*j->p != c) {
		snprintf(msg, sizeof(msg), "expected '%c'", c);
		return syntax(j, msg);
	}
	j->p++;
	return 0;
}

/* After ',' or the opening bracket: 1 if another element follows, 0 at close, -1 on error. */
static int next(struct json *j, char close, int first)
{
	skip_ws(j);
	if (*j->p == close) {
		j->p++;
		return 0;
	}
	if (!first && expect(j, ',') < 0)
		return -1;
	return 1;
}

static int string(struct json *j, char *out, size_t len)
{
	size_t n = 0;
	char c;

	skip_ws(j);
	if (*j->p != '"')
		return syntax(j, "expected a string");
	j->p++;
	while ((c = *j->p++) != '"') {
		if (!c || c == '\n')
			return syntax(j, "unterminated string");
		if (c == '\\') {
			c = *j->p++;
			switch (c) {
			case '"': case '\\': case '/': break;
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'u':
				/* plain ASCII only: \u0000..\u007f */
				if (strncmp(j->p, "00", 2) != 0 || !strchr("01234567", j->p[2]) ||
				    !strchr("0123456789abcdefABCDEF", j->p[3]))
					return syntax(j, "only ASCII \\u escapes are supported");
				c = (char)((j->p[2] - '0') * 16 +
					   (j->p[3] <= '9' ? j->p[3] - '0' : (j->p[3] | 0x20) - 'a' + 10));
				j->p += 4;
				break;
			default:
				return syntax(j, "bad escape in string");
			}
		}
		if (n + 1 >= len)
			return syntax(j, "string too long");
		out[n++] = c;
	}
	out[n] = '\0';
	return 0;
}

/* A string, number, true or false, as text. */
static int scalar(struct json *j, char *out, size_t len)
{
	size_t n = 0;

	skip_ws(j);
	if (*j->p == '"')
		return string(j, out, len);
	if (strncmp(j->p, "true", 4) == 0) {
		j->p += 4;
		snprintf(out, len, "yes");
		return 0;
	}
	if (strncmp(j->p, "false", 5) == 0) {
		j->p += 5;
		snprintf(out, len, "no");
		return 0;
	}
	while (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
		if (n + 1 >= len)
			return syntax(j, "number too long");
		out[n++] = *j->p++;
	}
	if (!n)
		return syntax(j, "expected a string, number, true or false");
	out[n] = '\0';
	return 0;
}

/* Append s to out as one exec argument, single-quoted; ' itself goes in as '"'"'. */
static int quote_arg(char *out, size_t len, const char *s)
{
	size_t n = strlen(out);

	if (n && n + 1 < len)
		out[n++] = ' ';
	if (n + 1 >= len)
		return -1;
	out[n++] = '\'';
	for (; *s; s++) {
		if (*s == '\'') {
			if (n + 6 >= len)
				return -1;
			memcpy(out + n, "'\"'\"'", 5);
			n += 5;
		} else {
			if (n + 2 >= len)
				return -1;
			out[n++] = *s;
		}
	}
	out[n++] = '\'';
	out[n] = '\0';
	return 0;
}

static struct pair *add_pair(struct json *j, int *npairs, const char *key)
{
	struct pair *p;
	if (*npairs >= MAX_PAIRS) {
		syntax(j, "too many keys in one service");
		return NULL;
	}
	p = &pairs[(*npairs)++];
	snprintf(p->key, sizeof(p->key), "%s", key);
	p->val[0] = '\0';
	p->line = j->line;
	return p;
}

/* One "key": value of a service, flattened into one or more pairs. */
static int service_value(struct json *j, const char *key, int *npairs)
{
	char item[CONFIG_LINE_LEN], name[64];
	struct pair *p;
	int r, first = 1;

	skip_ws(j);
	if (strcmp(key, "env") == 0 && *j->p == '{') {
		/* { "KEY": "VALUE", ... } -> env = KEY=VALUE each */
		j->p++;
		while ((r = next(j, '}', first)) > 0) {
			first = 0;
			if (string(j, name, sizeof(name)) < 0 || expect(j, ':') < 0 ||
			    scalar(j, item, sizeof(item)) < 0 || !(p = add_pair(j, npairs, key)))
				return -1;
			if (strlen(name) + 1 + strlen(item) >= sizeof(p->val))
				return syntax(j, "env entry too long");
			snprintf(p->val, sizeof(p->val), "%s=%.*s", name,
				 (int)(sizeof(p->val) - strlen(name) - 2), item);
		}
		return r;
	}
	if (*j->p != '[') {
		if (!(p = add_pair(j, npairs, key)))
			return -1;
		return scalar(j, p->val, sizeof(p->val));
	}

	/* arrays: env -> one pair each; exec -> quoted args; depends/provides -> "a, b" */
	j->p++;
	p = NULL;
	if (strcmp(key, "env") != 0 && !(p = add_pair(j, npairs, key)))
		return -1;
	while ((r = next(j, ']', first)) > 0) {
		first = 0;
		if (string(j, item, sizeof(item)) < 0)
			return -1;
		if (!p) {
			struct pair *e = add_pair(j, npairs, key);
			if (!e)
				return -1;
			snprintf(e->val, sizeof(e->val), "%s", item);
		} else if (strcmp(key, "exec") == 0) {
			if (quote_arg(p->val, sizeof(p->val), item) < 0)
				return syntax(j, "exec too long");
		} else {
			if (strlen(p->val) + strlen(item) + 3 >= sizeof(p->val))
				return syntax(j, "list too long");
			if (p->val[0])
				strcat(p->val, ", ");
			strcat(p->val, item);
		}
	}
	return r;
}

static int service(struct json *j, config_t *cfg)
{
	char key[32];
	service_t *svc = NULL;
	int r, i, npairs = 0, first = 1, start;

	skip_ws(j);
	start = j->line;  /* where the object begins, for "no name" */
	if (expect(j, '{') < 0)
		return -1;
	while ((r = next(j, '}', first)) > 0) {
		first = 0;
		if (string(j, key, sizeof(key)) < 0 || expect(j, ':') < 0 ||
		    service_value(j, key, &npairs) < 0)
			return -1;
	}
	if (r < 0)
		return -1;

	/* the name may come anywhere in the object, so look for it first */
	for (i = 0; i < npairs && strcmp(pairs[i].key, "name") != 0; i++)
		;
	if (i == npairs) {
		config_error(j->path, start, "service has no \"name\"");
		j->errors++;
		return 0;
	}
	svc = config_add_service(j->path, pairs[i].line, cfg, pairs[i].val);
	if (!svc) {
		j->errors++;
		return 0;
	}
	for (i = 0; i < npairs; i++) {
		if (strcmp(pairs[i].key, "name") != 0)
			j->errors += config_service_key(j->path, pairs[i].line, svc,
							pairs[i].key, pairs[i].val);
	}
	return 0;
}

static int system_section(struct json *j, config_t *cfg)
{
	char key[32], val[CONFIG_LINE_LEN];
	int r, first = 1;

	if (expect(j, '{') < 0)
		return -1;
	while ((r = next(j, '}', first)) > 0) {
		first = 0;
		if (string(j, key, sizeof(key)) < 0 || expect(j, ':') < 0 ||
		    scalar(j, val, sizeof(val)) < 0)
			return -1;
		j->errors += config_system_key(j->path, j->line, cfg, key, val);
	}
	return r;
}

static int document(struct json *j, config_t *cfg)
{
	char key[32];
	int r, first = 1, inner;

	if (expect(j, '{') < 0)
		return -1;
	while ((r = next(j, '}', first)) > 0) {
		first = 0;
		if (string(j, key, sizeof(key)) < 0 || expect(j, ':') < 0)
			return -1;
		if (strcmp(key, "system") == 0) {
			if (system_section(j, cfg) < 0)
				return -1;
		} else if (strcmp(key, "services") == 0) {
			if (expect(j, '[') < 0)
				return -1;
			for (inner = 1; (r = next(j, ']', inner)) > 0; inner = 0) {
				if (service(j, cfg) < 0)
					return -1;
			}
			if (r < 0)
				return -1;
		} else {
			config_error(j->path, j->line, "unknown top-level key '%s'", key);
			return -1;
		}
	}
	if (r < 0)
		return -1;
	skip_ws(j);
	return *j->p ? syntax(j, "trailing data after the top-level object") : 0;
}

int config_parse_json(const char *path, config_t *cfg)
{
	struct json j;
	size_t n;
	FILE *f;

	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fread(file_buf, 1, sizeof(file_buf), f);
	fclose(f);
	if (n > JSON_MAX_FILE) {
		fprintf(stderr, "minit: %s: larger than %d bytes\n", path, JSON_MAX_FILE);
		return -1;
	}
	file_buf[n] = '\0';
	if (strlen(file_buf) != n) {
		fprintf(stderr, "minit: %s: contains a NUL byte\n", path);
		return -1;
	}

	j.p = file_buf;
	j.path = path;
	j.line = 1;
	j.errors = 0;
	if (document(&j, cfg) < 0)
		return -1;
	j.errors += config_check_services(path, cfg);
	return j.errors ? -1 : 0;
}
#endif
