#include "minit.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_DIR   "/var/log/minit"
#define LOG_DIR_ALT "/tmp/minit"
#define BOOT_FILE "/var/run/minit.boot"
#define BOOT_FILE_RUN "/run/minit.boot"   /* /var/run is often missing on minimal rootfs */
#define BOOT_FILE_ALT "/tmp/minit.boot"

static int crash_fd = -1;
static char dir_used[200] = LOG_DIR;

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

/* Use dir for the crash log and service logs; returns 0 if the crash log opened. */
static int try_dir(const char *dir)
{
	char path[256];

	mkdir_p(dir);
	snprintf(path, sizeof(path), "%s/crashes.log", dir);
	crash_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (crash_fd < 0)
		return -1;
	snprintf(dir_used, sizeof(dir_used), "%s", dir);
	return 0;
}

int log_init(const char *dir)
{
	if (dir && dir[0])
		return try_dir(dir);
	if (try_dir(LOG_DIR) == 0)
		return 0;
	return try_dir(LOG_DIR_ALT);
}

const char *log_dir(void)
{
	return dir_used;
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
	(void)!write(crash_fd, buf, strlen(buf));
}

void log_giveup(const char *name, int restarts)
{
	char buf[256];
	char ts[64];
	timestamp(ts, sizeof(ts));
	snprintf(buf, sizeof(buf), "%s | %-12s | gave up after %d restarts\n",
		ts, name, restarts);
	(void)!write(STDERR_FILENO, "minit: ", 7);
	(void)!write(STDERR_FILENO, buf, strlen(buf));
	if (crash_fd >= 0)
		(void)!write(crash_fd, buf, strlen(buf));
}

void log_boot(int boot_ms, int total, int ready)
{
	static const char *const paths[] = { BOOT_FILE, BOOT_FILE_RUN, BOOT_FILE_ALT };
	char buf[128];
	size_t i;
	int fd;

	snprintf(buf, sizeof(buf), "boot_ms=%d\nservices_total=%d\nservices_started=%d\n",
		boot_ms, total, ready);
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) {
			(void)!write(fd, buf, strlen(buf));
			close(fd);
			return;
		}
	}
}
