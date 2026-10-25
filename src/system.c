#include "minit.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

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
