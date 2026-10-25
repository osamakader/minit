#include "minit.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_MOUNTS      64
#define KILL_GRACE_MS   2000
#define EMERGENCY_SHELL "/bin/sh"

struct early_mount {
	const char *source;
	const char *target;
	const char *fstype;
	unsigned long flags;
	const char *data;
};

/* in order: /dev must exist before /dev/pts and /dev/shm */
static const struct early_mount early_mounts[] = {
	{ "proc",     "/proc",    "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL },
	{ "sysfs",    "/sys",     "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL },
	{ "devtmpfs", "/dev",     "devtmpfs", MS_NOSUID,                        "mode=0755" },
	{ "devpts",   "/dev/pts", "devpts",   MS_NOSUID | MS_NOEXEC,            "mode=0620,ptmxmode=0666" },
	{ "tmpfs",    "/dev/shm", "tmpfs",    MS_NOSUID | MS_NODEV,             "mode=1777" },
	{ "tmpfs",    "/run",     "tmpfs",    MS_NOSUID | MS_NODEV,             "mode=0755" },
	{ "tmpfs",    "/tmp",     "tmpfs",    MS_NOSUID | MS_NODEV,             "mode=1777" },
};

/* A path is a mountpoint if it is on a different device than its parent, or is "/". */
static int is_mountpoint(const char *path)
{
	struct stat st, parent;
	char buf[256];

	if (stat(path, &st) < 0)
		return 0;
	snprintf(buf, sizeof(buf), "%s/..", path);
	if (stat(buf, &parent) < 0)
		return 0;
	return st.st_dev != parent.st_dev || st.st_ino == parent.st_ino;
}

void system_mount_early(void)
{
	size_t i;

	for (i = 0; i < sizeof(early_mounts) / sizeof(early_mounts[0]); i++) {
		const struct early_mount *m = &early_mounts[i];
		if (is_mountpoint(m->target))
			continue;  /* already set up, e.g. by an initramfs */
		mkdir(m->target, 0755);
		if (mount(m->source, m->target, m->fstype, m->flags, m->data) < 0)
			fprintf(stderr, "minit: cannot mount %s on %s: %s\n",
				m->fstype, m->target, strerror(errno));
	}
}

void system_setup_stdio(int pid1)
{
	int fd;

	/* the kernel may start init without a console if /dev was empty at boot */
	if (pid1) {
		fd = open("/dev/console", O_RDWR | O_NOCTTY);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
	}

	/* never leave 0-2 closed, or the next open() (e.g. the crash log) lands there */
	for (fd = 0; fd <= 2; fd++) {
		if (fcntl(fd, F_GETFD) < 0 && open("/dev/null", O_RDWR) != fd)
			break;
	}
}

/* PID 1 must never exit or the kernel panics: stay alive and keep reaping orphans. */
void system_hang(void)
{
	for (;;) {
		if (waitpid(-1, NULL, 0) < 0)
			sleep(1);
	}
}

/* SIGTERM, then SIGKILL, every process except us; reap them as they go. */
static void kill_all(void)
{
	struct timespec tick = { 0, 100 * 1000000L };
	int i;

	kill(-1, SIGTERM);
	for (i = 0; i < KILL_GRACE_MS / 100; i++) {
		errno = 0;
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		if (errno == ECHILD)
			break;  /* nothing left */
		nanosleep(&tick, NULL);
	}
	kill(-1, SIGKILL);
	while (waitpid(-1, NULL, 0) > 0)
		;
}

/* Unmount in reverse mount order; whatever is busy is at least made read-only. */
static void unmount_all(void)
{
	static char targets[MAX_MOUNTS][256];
	char line[512];
	FILE *f;
	int n = 0;

	f = fopen("/proc/mounts", "r");
	if (f) {
		while (n < MAX_MOUNTS && fgets(line, sizeof(line), f)) {
			if (sscanf(line, "%*s %255s", targets[n]) == 1 && strcmp(targets[n], "/") != 0)
				n++;
		}
		fclose(f);
	}
	while (n-- > 0) {
		if (umount(targets[n]) < 0)
			mount(NULL, targets[n], NULL, MS_REMOUNT | MS_RDONLY, NULL);
	}
	mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
}

void system_shutdown(shutdown_action_t action)
{
	static const char *const what[] = { "powering off", "rebooting", "halting" };
	int cmd = action == SHUTDOWN_REBOOT ? RB_AUTOBOOT :
		  action == SHUTDOWN_HALT ? RB_HALT_SYSTEM : RB_POWER_OFF;

	/* kill(-1) outside PID 1 would hit every process of this user */
	if (getpid() != 1)
		return;

	fprintf(stderr, "minit: %s\n", what[action]);
	kill_all();
	sync();
	unmount_all();
	sync();
	reboot(cmd);
	system_hang();  /* reboot() failed */
}

int system_emergency_shell(void)
{
	pid_t pid, w;

	if (access(EMERGENCY_SHELL, X_OK) < 0)
		return -1;
	fprintf(stderr, "minit: starting emergency shell, exit it to retry boot\n");
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		setsid();
		ioctl(0, TIOCSCTTY, 1);  /* console as controlling tty, for Ctrl-C and job control */
		execl(EMERGENCY_SHELL, EMERGENCY_SHELL, (char *)NULL);
		_exit(127);
	}
	/* reap anything the shell leaves behind while waiting for it */
	while ((w = waitpid(-1, NULL, 0)) != pid) {
		if (w < 0 && errno != EINTR)
			break;
	}
	return 0;
}
