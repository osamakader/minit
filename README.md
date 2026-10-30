# minit

Minimal PID1 replacement for embedded systems. A middle ground between BusyBox init and systemd.

## Features

- **Two config formats** — INI-style `[service name]` sections or JSON (optional at build time); `minit --check` validates either
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
- **Control** — `minitctl status | start | stop | restart | reload | reboot | poweroff | halt`
- **Config reload** — SIGHUP or `minitctl reload` applies only what changed
- **Hardware watchdog** — kept alive by minit; `critical` services reboot the board when they fail for good
- **Process settings** — per-service user/group, working directory, umask, nice level and environment
- **Minimal footprint** — about 130KB for minit and 35KB for minitctl as static musl binaries built with `-Os` (budget: 500KB)

## Build

```sh
make                       # dynamic ./minit and ./minitctl
make static CC=musl-gcc    # stripped static ./minit-static and ./minitctl-static for the target rootfs
make size CC=musl-gcc      # report the static binary sizes
make test                  # unit tests and behaviour tests (no root needed)
make JSON=0                # leave out the JSON config parser
```

## Usage

```sh
./minit [config]           # default: /etc/minit.conf, or MINIT_CONFIG
./minit --check [config]   # validate it and print the start order; exit 1 on errors
```

`MINIT_LOG_DIR` overrides the log directory and `MINIT_SOCKET` the control socket path.

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
| `ready_timeout` | — | Seconds (1–3600) a oneshot, `ready_fd` or `ready_file` service may take to become ready; then its process group is killed and the restart policy applies. |
| `critical` | `no` | `yes`: reboot when the service fails for good (gives up after `respawn_max`, or fails with no restart left). |
| `respawn_delay` | `1` | Base restart delay in seconds (1–60), doubled per quick restart, capped at 60s. |
| `respawn_max` | `0` | Give up after this many quick restarts in a row (0 = never give up). |
| `respawn_window` | `60` | A run longer than this many seconds resets the restart count. |
| `log` | `console` | `console`, `null`, `file` (`<log dir>/<name>.log`) or `kmsg` (one kernel log record per line). |
| `log_size` | `256` | `log = file`: rotate to `<name>.log.1` at this many KB. |
| `user` | minit's | Name or uid to run as. A named user also gets its supplementary groups. Resolved at each start; unknown users mark the service failed. |
| `group` | user's primary | Name or gid. |
| `workdir` | minit's | Absolute directory to start in (entered after switching user). |
| `umask` | minit's | Octal file mode mask, e.g. `027`. |
| `nice` | `0` | Niceness, -20..19. |
| `env` | — | `KEY=VALUE`; repeat for more (up to 8). Overrides `env_file`. |
| `env_file` | — | Absolute path of a file with `KEY=VALUE` lines (`#` comments allowed), read at each start. |

Without `type = oneshot`, `ready_fd` or `ready_file`, a service is ready as soon as it is started.
Use at most one of the three. The boot report is written once every service is ready (or can never
start because a dependency stopped or failed for good). The log directory is `/var/log/minit`, or
`/tmp/minit` if that is not writable. The kernel rate-limits `/dev/kmsg` writes unless booted with
`printk.devkmsg=on`.

An optional `[system]` section holds settings for minit itself:

| Key | Default | Description |
|-----|---------|-------------|
| `watchdog` | — | Watchdog device, e.g. `/dev/watchdog`. minit pings it at half its timeout. It is disarmed (magic close) on poweroff, halt or when minit exits outside PID 1, and left armed on reboot. |
| `watchdog_timeout` | driver default | Seconds (1–600) to program into the watchdog. |

### JSON

A file ending in `.json`, or starting with `{`, is read as JSON with the same keys (see
`examples/minit.json.example`):

```json
{
  "system": { "watchdog": "/dev/watchdog" },
  "services": [
    { "name": "network", "type": "oneshot", "exec": "/etc/minit/network.sh" },
    { "name": "httpd", "exec": ["/usr/sbin/lighttpd", "-D", "-f", "/etc/lighttpd.conf"],
      "depends": ["network"], "env": { "LANG": "C" }, "critical": true }
  ]
}
```

`exec` may be a string (quoted like in the conf format) or an array of arguments; `depends`,
`provides` and `env` may be arrays; `env` may also be an object; `true`/`false` stand for
`yes`/`no`. Files are limited to 64KB and only ASCII `\u` escapes are accepted.

## Control

`minitctl` talks to minit over `/run/minit.sock` (mode 0600; set `MINIT_SOCKET` on both sides to use
another path):

```sh
minitctl status                   # state, pid and restart count of every service
minitctl stop httpd               # SIGTERM its process group (SIGKILL after 10s); stays stopped
minitctl start httpd              # start it again once its dependencies are ready
minitctl restart httpd
minitctl reload                   # same as SIGHUP
minitctl reboot | poweroff | halt
```

`reload` (or SIGHUP) parses the config again and, only if it is valid, applies the difference:
unchanged services keep running, changed ones restart with their new settings, new ones start and
removed ones are stopped. Changes to `[system]` need a restart. An invalid config is reported and
the running one is kept.

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
| SIGHUP | reload the config (not a shutdown) |

Services are stopped in reverse dependency order (each service's whole process group gets SIGTERM,
then SIGKILL after 30s), every remaining process gets SIGTERM and then
SIGKILL, a fresh random seed is saved, filesystems are unmounted (or remounted read-only if busy),
and the system reboots, halts or powers off.
