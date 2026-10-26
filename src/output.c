#include "minit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KMSG_LINE 256

/*
 * One pipe per service for minit's whole lifetime: minit keeps both ends, so
 * the pipe survives restarts and never reports EOF, and every instance of the
 * service writes its stdout/stderr into it.
 */
static struct {
	int used;
	int rd, wr;
	int log_fd;   /* log = file */
	long size;    /* bytes in the current log file */
	char line[KMSG_LINE];  /* log = kmsg: partial line */
	size_t len;
} out[MINIT_MAX_SERVICES];

static int kmsg_fd = -1;

static void open_log_file(int idx, const service_t *svc)
{
	char path[256];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s.log", log_dir(), svc->name);
	out[idx].log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	out[idx].size = (out[idx].log_fd >= 0 && fstat(out[idx].log_fd, &st) == 0) ? st.st_size : 0;
	if (out[idx].log_fd < 0)
		fprintf(stderr, "minit: %s: cannot open %s: %s\n", svc->name, path, strerror(errno));
}

/* name.log -> name.log.1 (replacing the old one), then start a new name.log */
static void rotate(int idx, const service_t *svc)
{
	char path[256], old[260];

	snprintf(path, sizeof(path), "%s/%s.log", log_dir(), svc->name);
	snprintf(old, sizeof(old), "%s.1", path);
	close(out[idx].log_fd);
	rename(path, old);
	open_log_file(idx, svc);
}

int output_prepare(int idx, const service_t *svc)
{
	int fds[2];

	if (svc->log == LOG_CONSOLE || svc->log == LOG_NULL)
		return -1;
	if (out[idx].used)
		return out[idx].wr;
	if (pipe(fds) < 0)
		return -1;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	out[idx].used = 1;
	out[idx].rd = fds[0];
	out[idx].wr = fds[1];
	out[idx].log_fd = -1;
	out[idx].len = 0;
	if (svc->log == LOG_FILE)
		open_log_file(idx, svc);
	else if (kmsg_fd < 0 && (kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC)) < 0)
		fprintf(stderr, "minit: %s: cannot open /dev/kmsg: %s\n", svc->name, strerror(errno));
	return out[idx].wr;
}

int output_poll_fd(int idx)
{
	return out[idx].used ? out[idx].rd : -1;
}

/* One /dev/kmsg write is one record: "<6>name: text", level 6 = info. */
static void kmsg_line(const service_t *svc, const char *text, size_t len)
{
	char rec[KMSG_LINE + MINIT_NAME_LEN + 8];
	int n;

	if (kmsg_fd < 0)
		return;
	n = snprintf(rec, sizeof(rec), "<6>%s: %.*s", svc->name, (int)len, text);
	if (n > 0)
		(void)!write(kmsg_fd, rec, (size_t)n < sizeof(rec) ? (size_t)n : sizeof(rec) - 1);
}

static void to_kmsg(int idx, const service_t *svc, const char *buf, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (buf[i] != '\n')
			out[idx].line[out[idx].len++] = buf[i];
		if (buf[i] == '\n' || out[idx].len == sizeof(out[idx].line)) {
			kmsg_line(svc, out[idx].line, out[idx].len);
			out[idx].len = 0;
		}
	}
}

static void to_file(int idx, const service_t *svc, const char *buf, size_t n)
{
	ssize_t w;

	if (out[idx].log_fd < 0)
		return;
	w = write(out[idx].log_fd, buf, n);
	if (w > 0)
		out[idx].size += w;
	if (out[idx].size >= (long)svc->log_size_kb * 1024)
		rotate(idx, svc);
}

void output_handle(int idx, const service_t *svc)
{
	char buf[4096];
	ssize_t n;
	int i;

	/* bounded, so a chatty service cannot starve the main loop */
	for (i = 0; i < 16; i++) {
		n = read(out[idx].rd, buf, sizeof(buf));
		if (n <= 0)
			return;
		if (svc->log == LOG_FILE)
			to_file(idx, svc, buf, (size_t)n);
		else
			to_kmsg(idx, svc, buf, (size_t)n);
	}
}
