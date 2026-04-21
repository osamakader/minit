#include "minit.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_DIR   "/var/log/minit"
#define LOG_DIR_ALT "/tmp/minit"
#define CRASH_LOG "/var/log/minit/crashes.log"
#define CRASH_LOG_ALT "/tmp/minit/crashes.log"
#define BOOT_FILE "/var/run/minit.boot"
#define BOOT_FILE_ALT "/tmp/minit.boot"

static int crash_fd = -1;
static char boot_path[128] = BOOT_FILE;

static void mkdir_p(const char *path)
{
	char buf[256];
	char *p;
	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(buf, 0755);
			*p = '/';
		}
	}
	mkdir(buf, 0755);
}

int log_init(const char *dir)
{
	(void)dir;
	mkdir_p(LOG_DIR);
	crash_fd = open(CRASH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (crash_fd < 0) {
		mkdir_p(LOG_DIR_ALT);
		crash_fd = open(CRASH_LOG_ALT, O_WRONLY | O_CREAT | O_APPEND, 0644);
		strncpy(boot_path, BOOT_FILE_ALT, sizeof(boot_path) - 1);
	}
	return (crash_fd >= 0) ? 0 : -1;
}

static void timestamp(char *buf, size_t len)
{
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void log_crash(const char *name, pid_t pid, int exit_code, int signal, int restarts)
{
	char buf[256];
	char ts[64];
	if (crash_fd < 0) return;
	timestamp(ts, sizeof(ts));
	snprintf(buf, sizeof(buf), "%s | %-12s | pid=%d | exit=%d | signal=%d | restarts=%d\n",
		ts, name, (int)pid, exit_code, signal, restarts);
	(void)write(crash_fd, buf, strlen(buf));
}

void log_boot(int boot_ms, int total, int started)
{
	int fd;
	char buf[128];
	snprintf(buf, sizeof(buf), "boot_ms=%d\nservices_total=%d\nservices_started=%d\n",
		boot_ms, total, started);
	fd = open(boot_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		(void)write(fd, buf, strlen(buf));
		close(fd);
	}
}
