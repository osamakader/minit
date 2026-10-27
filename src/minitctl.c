#include "minit.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s status\n"
		"       %s start|stop|restart <service>\n"
		"       %s reboot|poweroff|halt\n"
		"Socket: %s, or MINIT_SOCKET\n",
		prog, prog, prog, MINIT_SOCKET);
	return 2;
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr;
	char req[256], buf[CONTROL_REPLY_LEN];
	const char *path = getenv("MINIT_SOCKET");
	ssize_t n;
	int fd, i, failed = 0;
	size_t used = 0;

	if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
		return usage(argv[0]);
	if (!path || !path[0])
		path = MINIT_SOCKET;

	/* the request is the arguments joined by spaces, one line */
	req[0] = '\0';
	for (i = 1; i < argc; i++) {
		int w = snprintf(req + used, sizeof(req) - used, "%s%s", i > 1 ? " " : "", argv[i]);
		if (w < 0 || (size_t)w >= sizeof(req) - used - 1) {
			fprintf(stderr, "minitctl: request too long\n");
			return 2;
		}
		used += (size_t)w;
	}
	strcat(req, "\n");

	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "minitctl: socket path too long\n");
		return 1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "minitctl: cannot connect to %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (write(fd, req, strlen(req)) < 0) {
		fprintf(stderr, "minitctl: %s\n", strerror(errno));
		return 1;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (!failed && n >= 6 && strncmp(buf, "error:", 6) == 0)
			failed = 1;
		fwrite(buf, 1, (size_t)n, stdout);
	}
	close(fd);
	return failed;
}
