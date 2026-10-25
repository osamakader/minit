# minit

Minimal PID1 replacement for embedded systems. A middle ground between BusyBox init and systemd.

## Features

- **Simple conf format** — INI-style `[service name]` sections (JSON support is planned; `examples/minit.json.example` shows the intended format)
- **Service supervision** — track and respawn processes
- **Restart policies** — `always`, `on-failure`, `never`
- **Auto-restart with backoff** — exponential backoff to avoid restart storms
- **Signal handling** — SIGTERM, SIGINT → graceful shutdown; as PID 1, reboot/halt/poweroff (see below)
- **PID 1 boot setup** — mounts `/proc`, `/sys`, `/dev`, `/dev/pts`, `/dev/shm`, `/run`, `/tmp`, console, hostname, random seed
- **Emergency shell** — `/bin/sh` on the console on fatal boot errors or with `single`
- **Boot time measurement** — write `boot_ms` to `/var/run/minit.boot`
- **Dependency ordering** — topological start order via `depends` / `provides`
- **Crash logging** — log exits to `/var/log/minit/crashes.log`
- **Minimal footprint** — about 100KB as a static musl binary (budget: 500KB)

## Build

```sh
make                       # dynamic ./minit
make static CC=musl-gcc    # stripped static ./minit-static for the target rootfs
make size CC=musl-gcc      # report the static binary size
```

## Usage

```sh
./minit [config]
# Default config: /etc/minit.conf or MINIT_CONFIG env
```

## Running as PID 1

Boot with `init=/sbin/minit` (or install it as `/sbin/init`). As PID 1 minit:

1. mounts `/proc`, `/sys`, `/dev` (devtmpfs), `/dev/pts`, `/dev/shm`, `/run` and `/tmp`,
   skipping anything that is already a mountpoint (e.g. set up by an initramfs);
2. puts stdin/stdout/stderr on `/dev/console`;
3. sets the hostname from `/etc/hostname` and mixes `/var/lib/minit/random-seed` into `/dev/urandom`;
4. reads the kernel command line:
   - `minit.config=PATH` — config file (takes precedence over `MINIT_CONFIG`, which the kernel also passes);
   - `single` or `minit.single` — start an emergency shell before any service; exit it to continue booting.

If the config cannot be loaded, minit starts `/bin/sh` on the console. Fix the problem and exit the
shell to retry the boot. Without a shell it stays alive instead of exiting, because PID 1 exiting
panics the kernel.

Shutdown follows the BusyBox init convention, so BusyBox `reboot`, `halt` and `poweroff` work:

| Signal | Action |
|--------|--------|
| SIGTERM | reboot |
| SIGINT (Ctrl-Alt-Del) | reboot |
| SIGUSR1 | halt |
| SIGUSR2 | power off |

Services are stopped in reverse dependency order, every remaining process gets SIGTERM and then
SIGKILL, a fresh random seed is saved, filesystems are unmounted (or remounted read-only if busy),
and the system reboots, halts or powers off.
