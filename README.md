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
- **Boot time measurement** — write `boot_ms` to `/var/run/minit.boot` (or `/run/minit.boot` if `/var/run` is missing)
- **Dependency ordering** — topological start order via `depends` / `provides`; dependents wait until a service is ready
- **Oneshot services and readiness** — `type = oneshot`, `ready_fd` (s6-style notification) or `ready_file`
- **Service output** — to the console, `/dev/null`, a rotated log file, or the kernel log
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

## Configuration

```ini
[service network]
type = oneshot
exec = /etc/minit/network.sh

[service httpd]
exec = /usr/sbin/lighttpd -D -f /etc/lighttpd.conf
depends = network
log = file
```

Service names may use `A-Z a-z 0-9 . _ -`. Keys per `[service name]` section:

| Key | Default | Description |
|-----|---------|-------------|
| `exec` | (required) | Command line, run directly without a shell. `'...'`, `"..."` and `\` quote like a shell, without expansion. Looked up in `PATH`. |
| `restart` | `on-failure` | `always`, `on-failure` or `never`. Being killed by a signal counts as a failure. |
| `type` | `simple` | `oneshot`: ready once it exits 0, for setup scripts (`restart = always` is not allowed). |
| `ready_fd` | — | 3–63: the service gets a pipe on this fd and is ready once it writes a newline to it (s6 notification-fd). |
| `ready_file` | — | Absolute path: ready once it exists. Removed before each start so a stale file does not count. |
| `provides` | — | Comma-separated names (up to 4) others can depend on. The service name is always provided too. |
| `depends` | — | Comma-separated names; this service starts once all their providers are ready. |
| `respawn_delay` | `1` | Base restart delay in seconds (1–60), doubled per quick restart, capped at 60s. |
| `respawn_max` | `0` | Give up after this many quick restarts in a row (0 = never give up). |
| `respawn_window` | `60` | A run longer than this many seconds resets the restart count. |
| `log` | `console` | `console`, `null`, `file` (`<log dir>/<name>.log`) or `kmsg` (one kernel log record per line). |
| `log_size` | `256` | `log = file`: rotate to `<name>.log.1` at this many KB. |

Without `type = oneshot`, `ready_fd` or `ready_file`, a service is ready as soon as it is started.
Use at most one of the three. The boot report is written once every service is ready (or can never
start because a dependency stopped or failed for good). The log directory is `/var/log/minit`, or
`/tmp/minit` if that is not writable. The kernel rate-limits `/dev/kmsg` writes unless booted with
`printk.devkmsg=on`.

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
