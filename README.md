# minit

Minimal PID1 replacement for embedded systems. A middle ground between BusyBox init and systemd.

## Features

- **JSON or simple conf** — service definitions in `.json` or `.conf`
- **Service supervision** — track and respawn processes
- **Restart policies** — `always`, `on-failure`, `never`
- **Auto-restart with backoff** — exponential backoff to avoid restart storms
- **Signal handling** — SIGTERM, SIGINT → graceful shutdown
- **Boot time measurement** — write `boot_ms` to `/var/run/minit.boot`
- **Dependency ordering** — topological start order via `depends` / `provides`
- **Crash logging** — log exits to `/var/log/minit/crashes.log`
- **Minimal footprint** — <500KB static memory

## Build

```sh
make
```

## Usage

```sh
./minit [config]
# Default config: /etc/minit.conf or MINIT_CONFIG env
```
