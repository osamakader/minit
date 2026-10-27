#include "minit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* from <linux/watchdog.h> (stable ABI), which toolchains without kernel headers lack */
#define WDIOC_KEEPALIVE  _IOR('W', 5, int)
#define WDIOC_SETTIMEOUT _IOWR('W', 6, int)
#define WDIOC_GETTIMEOUT _IOR('W', 7, int)

#define DEFAULT_INTERVAL_MS 1000  /* when the driver does not report its timeout */

static int wd_fd = -1;
static long long interval_ms, next_ping_ms;

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int watchdog_open(const char *path, int timeout)
{
	int t = timeout;

	wd_fd = open(path, O_WRONLY | O_CLOEXEC);
	if (wd_fd < 0) {
		fprintf(stderr, "minit: cannot open watchdog %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (timeout > 0 && ioctl(wd_fd, WDIOC_SETTIMEOUT, &t) < 0)
		fprintf(stderr, "minit: cannot set watchdog timeout: %s\n", strerror(errno));
	/* the driver may round the timeout, so ask what it actually is */
	if (ioctl(wd_fd, WDIOC_GETTIMEOUT, &t) < 0 || t <= 0)
		interval_ms = DEFAULT_INTERVAL_MS;
	else
		interval_ms = (long long)t * 1000 / 2;
	watchdog_ping();
	return 0;
}

void watchdog_ping(void)
{
	if (wd_fd < 0)
		return;
	if (ioctl(wd_fd, WDIOC_KEEPALIVE, 0) < 0)
		(void)!write(wd_fd, "\0", 1);  /* any write also pings */
	next_ping_ms = now_ms() + interval_ms;
}

int watchdog_due(void)
{
	long long now;

	if (wd_fd < 0)
		return -1;
	now = now_ms();
	if (now >= next_ping_ms)
		watchdog_ping();
	return (int)(next_ping_ms - now_ms());
}

void watchdog_disarm(void)
{
	if (wd_fd < 0)
		return;
	/* "magic close": drivers without nowayout stop the timer when 'V' precedes close */
	(void)!write(wd_fd, "V", 1);
	close(wd_fd);
	wd_fd = -1;
}
