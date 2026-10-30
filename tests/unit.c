/* Unit tests for the parts of minit that need no processes: quoting, config parsing, deps. */
#include "minit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* Write text to a temporary config file and parse it; suffix picks the format by name. */
static int parse_as(const char *text, config_t *cfg, const char *suffix)
{
	char path[64];
	int fd, r;
	snprintf(path, sizeof(path), "/tmp/minit-unit-XXXXXX%s", suffix);
	fd = mkstemps(path, (int)strlen(suffix));
	FILE *f = fdopen(fd, "w");
	fputs(text, f);
	fclose(f);
	r = config_parse(path, cfg);
	unlink(path);
	return r;
}

static int parse(const char *text, config_t *cfg)
{
	return parse_as(text, cfg, "");
}

static int split(const char *exec, char *argv[MINIT_MAX_ARGS + 1])
{
	static char buf[MINIT_EXEC_LEN];
	return exec_split(exec, buf, sizeof(buf), argv);
}

static void test_exec_split(void)
{
	char *argv[MINIT_MAX_ARGS + 1];
	char many[MINIT_EXEC_LEN];
	int i;

	CHECK(split("  /bin/echo   a  b ", argv) == 3);
	CHECK(strcmp(argv[0], "/bin/echo") == 0 && strcmp(argv[2], "b") == 0 && argv[3] == NULL);
	CHECK(split("echo \"a b\" 'c \"d\"' e\\ f", argv) == 4);
	CHECK(strcmp(argv[1], "a b") == 0);
	CHECK(strcmp(argv[2], "c \"d\"") == 0);
	CHECK(strcmp(argv[3], "e f") == 0);
	CHECK(split("echo \"a\\\"b\\\\c\" 'x\\y'", argv) == 3);
	CHECK(strcmp(argv[1], "a\"b\\c") == 0);
	CHECK(strcmp(argv[2], "x\\y") == 0);  /* no escapes inside single quotes */
	CHECK(split("echo \"\" x", argv) == 3 && argv[1][0] == '\0');
	CHECK(split("echo mixed\"part\"'s'", argv) == 2 && strcmp(argv[1], "mixedparts") == 0);
	CHECK(split("echo \"unterminated", argv) == -1);
	CHECK(split("   ", argv) == 0);

	/* more words than MINIT_MAX_ARGS: truncated, still NULL-terminated */
	many[0] = '\0';
	for (i = 0; i < MINIT_MAX_ARGS + 5; i++)
		strcat(many, "a ");
	CHECK(split(many, argv) == MINIT_MAX_ARGS && argv[MINIT_MAX_ARGS] == NULL);
}

static void test_config_defaults(void)
{
	config_t cfg;
	const service_t *s;

	CHECK(parse("# comment\n\n[service a]\nexec = /bin/true\n", &cfg) == 0);
	CHECK(cfg.nservices == 1);
	s = &cfg.services[0];
	CHECK(strcmp(s->name, "a") == 0 && strcmp(s->exec, "/bin/true") == 0);
	CHECK(s->restart == RESTART_ON_FAILURE);
	CHECK(s->respawn_delay == 1 && s->respawn_max == 0 && s->respawn_window == 60);
	CHECK(s->log == LOG_CONSOLE && s->log_size_kb == 256);
	CHECK(!s->oneshot && !s->ready_fd && !s->ready_file[0] && !s->critical);
	CHECK(!cfg.watchdog[0]);
}

static void test_config_values(void)
{
	config_t cfg;
	const service_t *s;

	CHECK(parse("[system]\nwatchdog = /dev/watchdog\nwatchdog_timeout = 15\n"
		    "[service a]\nexec = x\nrestart = always\nprovides = p1, p2\n"
		    "depends = b\nrespawn_delay = 5\nrespawn_max = 3\nrespawn_window = 30\n"
		    "log = file\nlog_size = 64\nready_fd = 4\nready_timeout = 9\ncritical = yes\n"
		    "user = www\ngroup = 33\nworkdir = /srv\numask = 027\nnice = -3\n"
		    "env = A=1\nenv = B=two words\nenv_file = /etc/a.env\n"
		    "[service b]\ntype = oneshot\nexec = y\n", &cfg) == 0);
	CHECK(strcmp(cfg.watchdog, "/dev/watchdog") == 0 && cfg.watchdog_timeout == 15);
	s = &cfg.services[0];
	CHECK(s->restart == RESTART_ALWAYS);
	CHECK(s->nprovides == 2 && strcmp(s->provides[1], "p2") == 0);
	CHECK(s->ndeps == 1 && strcmp(s->depends[0], "b") == 0);
	CHECK(s->respawn_delay == 5 && s->respawn_max == 3 && s->respawn_window == 30);
	CHECK(s->log == LOG_FILE && s->log_size_kb == 64);
	CHECK(s->ready_fd == 4 && s->ready_timeout == 9 && s->critical);
	CHECK(strcmp(s->user, "www") == 0 && strcmp(s->group, "33") == 0);
	CHECK(strcmp(s->workdir, "/srv") == 0 && s->umask == 027 && s->set_nice && s->nice == -3);
	CHECK(cfg.services[1].umask == -1 && !cfg.services[1].set_nice);
	CHECK(s->nenv == 2 && strcmp(s->env[1], "B=two words") == 0);
	CHECK(strcmp(s->env_file, "/etc/a.env") == 0);
	CHECK(config_valid_env("_X1=") && !config_valid_env("1X=a") && !config_valid_env("X"));
	CHECK(cfg.services[1].oneshot);
}

static void test_config_errors(void)
{
	config_t cfg;
	char big[700];

	CHECK(parse("[service a]\nexec = x\ncolour = red\n", &cfg) < 0);
	CHECK(parse("exec = x\n", &cfg) < 0);                       /* key outside a section */
	CHECK(parse("[service a]\n", &cfg) < 0);                    /* no exec */
	CHECK(parse("[service a]\nexec = \"x\n", &cfg) < 0);        /* unterminated quote */
	CHECK(parse("[service a]\nexec=x\n[service a]\nexec=y\n", &cfg) < 0);
	CHECK(parse("[service ../x]\nexec = x\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nrestart = sometimes\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nrespawn_delay = 5s\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nrespawn_delay = 61\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\ntype = oneshot\nrestart = always\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nready_fd = 3\nready_file = /f\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nready_timeout = 5\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nready_file = relative\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nprovides = 1,2,3,4,5\n", &cfg) < 0);
	CHECK(parse("[system]\nwatchdog = /w\n[system]\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nuser = a b\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\numask = 8\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nnice = -21\n", &cfg) < 0);
	CHECK(parse("[service a]\nexec = x\nenv = NOVALUE\n", &cfg) < 0);
	snprintf(big, sizeof(big), "[service a]\nexec = %0600d\n", 0);
	CHECK(parse(big, &cfg) < 0);                                /* line too long */
}

static int position(const int *order, int n, int idx)
{
	int i;
	for (i = 0; i < n; i++) {
		if (order[i] == idx)
			return i;
	}
	return -1;
}

static void test_deps(void)
{
	config_t cfg;
	int order[MINIT_MAX_SERVICES];

	/* c -> b (by provides) -> a (by name); listed in reverse */
	CHECK(parse("[service c]\nexec=x\ndepends=b-api\n"
		    "[service b]\nexec=x\nprovides=b-api\ndepends=a\n"
		    "[service a]\nexec=x\n", &cfg) == 0);
	CHECK(deps_resolve(&cfg, order) == 0);
	CHECK(position(order, 3, 2) < position(order, 3, 1));
	CHECK(position(order, 3, 1) < position(order, 3, 0));
	CHECK(deps_find_provider(&cfg, "b-api") == 1 && deps_find_provider(&cfg, "a") == 2);
	CHECK(deps_find_provider(&cfg, "nope") == -1);

	CHECK(parse("[service a]\nexec=x\nprovides=p\n[service b]\nexec=x\ndepends=p, p\n", &cfg) == 0);
	CHECK(deps_resolve(&cfg, order) == 0);  /* a repeated dependency is not a cycle */

	CHECK(parse("[service a]\nexec=x\ndepends=b\n[service b]\nexec=x\ndepends=a\n", &cfg) == 0);
	CHECK(deps_resolve(&cfg, order) < 0);
	CHECK(parse("[service a]\nexec=x\ndepends=a\n", &cfg) == 0);
	CHECK(deps_resolve(&cfg, order) < 0);
	CHECK(parse("[service a]\nexec=x\ndepends=nope\n", &cfg) == 0);
	CHECK(deps_resolve(&cfg, order) < 0);
	CHECK(parse("[service a]\nexec=x\nprovides=p\n[service b]\nexec=x\nprovides=p\n", &cfg) == 0);
	CHECK(deps_resolve(&cfg, order) < 0);
	CHECK(parse("[service a]\nexec=x\nprovides=b\n[service b]\nexec=x\n", &cfg) == 0);
	CHECK(deps_resolve(&cfg, order) < 0);   /* provides clashes with a service name */
}

#ifdef MINIT_JSON
static int parse_json(const char *text, config_t *cfg)
{
	return parse_as(text, cfg, ".json");
}

static void test_json_matches_conf(void)
{
	static config_t a, b;

	CHECK(parse("[system]\nwatchdog = /dev/watchdog\nwatchdog_timeout = 20\n"
		    "[service db]\nexec = /usr/bin/db --port 5432\nrestart = always\n"
		    "provides = sql, storage\nready_fd = 3\nready_timeout = 30\ncritical = yes\n"
		    "log = file\nlog_size = 128\nuser = db\ngroup = db\nworkdir = /var/db\n"
		    "umask = 027\nnice = 5\nenv = A=1\nenv = B=two words\nenv_file = /etc/db.env\n"
		    "respawn_delay = 2\nrespawn_max = 5\nrespawn_window = 120\n"
		    "[service web]\ntype = oneshot\nexec = /bin/setup\ndepends = sql, db\n", &a) == 0);
	CHECK(parse_json("{\n"
		    "  \"system\": { \"watchdog\": \"/dev/watchdog\", \"watchdog_timeout\": 20 },\n"
		    "  \"services\": [\n"
		    "    { \"exec\": \"/usr/bin/db --port 5432\", \"name\": \"db\", \"restart\": \"always\",\n"
		    "      \"provides\": [\"sql\", \"storage\"], \"ready_fd\": 3, \"ready_timeout\": 30,\n"
		    "      \"critical\": true, \"log\": \"file\", \"log_size\": 128, \"user\": \"db\",\n"
		    "      \"group\": \"db\", \"workdir\": \"/var/db\", \"umask\": \"027\", \"nice\": 5,\n"
		    "      \"env\": { \"A\": \"1\", \"B\": \"two words\" }, \"env_file\": \"/etc/db.env\",\n"
		    "      \"respawn_delay\": 2, \"respawn_max\": 5, \"respawn_window\": 120 },\n"
		    "    { \"name\": \"web\", \"type\": \"oneshot\", \"exec\": \"/bin/setup\",\n"
		    "      \"depends\": \"sql, db\" }\n"
		    "  ]\n}\n", &b) == 0);
	/* config_parse() zeroes everything first, so equal settings mean equal bytes */
	CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}

static void test_json_values(void)
{
	static config_t cfg;
	char *argv[MINIT_MAX_ARGS + 1];

	/* exec as an array keeps every argument intact through quoting */
	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":[\"/bin/echo\",\"it's\","
			 "\"two words\",\"q\\\"uote\",\"$HOME\",\"back\\\\slash\",\"\"],"
			 "\"env\":[\"X=1\",\"Y=2\"]}]}", &cfg) == 0);
	CHECK(split(cfg.services[0].exec, argv) == 7);
	CHECK(strcmp(argv[1], "it's") == 0);
	CHECK(strcmp(argv[2], "two words") == 0);
	CHECK(strcmp(argv[3], "q\"uote") == 0);
	CHECK(strcmp(argv[4], "$HOME") == 0);
	CHECK(strcmp(argv[5], "back\\slash") == 0);
	CHECK(argv[6][0] == '\0');
	CHECK(cfg.services[0].nenv == 2 && strcmp(cfg.services[0].env[1], "Y=2") == 0);
	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":\"\\u0041\\t\"}]}", &cfg) == 0);
	CHECK(strcmp(cfg.services[0].exec, "A\t") == 0);
	/* detected by content as well as by name */
	CHECK(parse("  {\"services\":[{\"name\":\"a\",\"exec\":\"x\"}]}", &cfg) == 0 &&
	      cfg.nservices == 1);
}

static void test_json_errors(void)
{
	static config_t cfg;

	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":\"x\",\"colour\":\"red\"}]}", &cfg) < 0);
	CHECK(parse_json("{\"services\":[{\"exec\":\"x\"}]}", &cfg) < 0);          /* no name */
	CHECK(parse_json("{\"services\":[{\"name\":\"a\"}]}", &cfg) < 0);          /* no exec */
	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":\"x\"}]", &cfg) < 0);   /* unclosed */
	CHECK(parse_json("{\"services\":[]} x", &cfg) < 0);                       /* trailing data */
	CHECK(parse_json("{\"servics\":[]}", &cfg) < 0);
	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":\"x\",\"depends\":[[\"b\"]]}]}", &cfg) < 0);
	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":\"\\u00e9\"}]}", &cfg) < 0);
	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":\"x\",\"restart\":null}]}", &cfg) < 0);
	CHECK(parse_json("{\"services\":[{\"name\":\"a\",\"exec\":\"x\"},{\"name\":\"a\",\"exec\":\"y\"}]}", &cfg) < 0);
	CHECK(parse_json("", &cfg) < 0);
}
#endif

int main(void)
{
	/* the error cases print parse errors; keep them out of the test output */
	if (!getenv("UNIT_VERBOSE"))
		(void)!freopen("/dev/null", "w", stderr);

	test_exec_split();
	test_config_defaults();
	test_config_values();
	test_config_errors();
	test_deps();
#ifdef MINIT_JSON
	test_json_matches_conf();
	test_json_values();
	test_json_errors();
#endif

	printf("%d/%d unit checks passed\n", checks - failures, checks);
	return failures != 0;
}
