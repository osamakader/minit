#define _GNU_SOURCE  /* struct ucred */
#include "minit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIENT_TIMEOUT_MS 200  /* a stuck client must not stall PID 1 */

int control_open(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path))
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path);
	unlink(path);  /* left over from a previous run */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    chmod(path, 0600) < 0 || listen(fd, 4) < 0) {
		fprintf(stderr, "minit: cannot listen on %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int control_accept(int listen_fd, char *req, size_t len)
{
	struct timeval tv = { 0, CLIENT_TIMEOUT_MS * 1000 };
	struct ucred cred;
	socklen_t clen = sizeof(cred);
	ssize_t n;
	int fd;

	fd = accept(listen_fd, NULL, NULL);
	if (fd < 0)
		return -1;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	/* the socket is 0600, but check the peer too in case its mode was changed */
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 ||
	    (cred.uid != 0 && cred.uid != getuid())) {
		close(fd);
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	n = read(fd, req, len - 1);
	if (n <= 0) {
		close(fd);
		return -1;
	}
	req[n] = '\0';
	req[strcspn(req, "\r\n")] = '\0';
	return fd;
}

void control_reply(int fd, const char *text)
{
	(void)!write(fd, text, strlen(text));
	close(fd);
}
