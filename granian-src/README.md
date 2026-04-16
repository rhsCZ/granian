# granian

`granian` is a Debian/Ubuntu package that provides a native C++ supervisor for
running one or more Python web applications with upstream `python3-granian`.
It is designed for system service use:

- one systemd unit: `granian.service`
- optional per-application unit: `granian@app.service`
- multiple application configs under `/etc/granian/apps-available/`
- enabled applications selected by symlinks in `/etc/granian/apps-enabled/`
- per-application process supervision
- per-application log files under `/var/log/granian/`
- helper commands similar to Apache's enable/disable workflow

The package does not replace upstream Granian. It wraps it so multiple apps
can be managed in a consistent Debian-friendly way.

## What The Package Does

The installed supervisor:

- reads global defaults from `/etc/granian/granian.conf`
- optionally reads extra defaults from `/etc/granian/conf.d/*.conf`
- scans `/etc/granian/apps-enabled/*.conf`
- loads each enabled application config
- validates wrapper-specific settings like `working-directory` and `venv`
- starts one Granian process per enabled application
- drops each child process to the configured Unix user/group
- restarts failed applications with restart throttling to avoid endless loops
- writes supervisor logs to the systemd journal
- writes per-app stdout/stderr logs to `/var/log/granian/<app>/<app>.log` by default

This gives you a workflow similar in spirit to `a2ensite`/`a2dissite`, but for
Granian-backed Python applications.

The package can be used in two execution modes:

- global supervisor mode via `granian.service`, which manages all enabled apps
- per-app mode via `granian@app.service`, which manages exactly one enabled app

## Installed Files

Important files and directories:

```text
/usr/sbin/granian-wrapper
/usr/sbin/granianenconf
/usr/sbin/graniandisconf
/usr/lib/systemd/system/granian.service
/usr/lib/systemd/system/granian@.service
/etc/default/granian
/etc/granian/granian.conf
/etc/granian/conf.d/
/etc/granian/apps-available/
/etc/granian/apps-enabled/
/etc/logrotate.d/granian
/var/log/granian/
```

Purpose of the main components:

- `granian-wrapper`: supervisor process started by systemd
- `granianenconf`: creates a symlink from `apps-available` to `apps-enabled`
- `graniandisconf`: removes that symlink
- `granian.service`: single systemd unit managing all enabled apps
- `granian@.service`: instance unit managing one enabled app
- `granian.conf`: global defaults shared by all apps
- `apps-available/*.conf`: per-app config files
- `apps-enabled/*.conf`: enabled apps via symlinks

## Installation

Runtime dependencies:

- `python3-granian`
- `systemd`
- standard Debian/Ubuntu base tools such as `adduser`

Install the package normally:

```sh
sudo dpkg -i granian_1.0.x_amd64.deb
```

If dependencies are missing, install them in the usual Debian/Ubuntu way.

After install:

- the package creates the `granian` system user and group
- it installs the service unit, configs and helper binaries
- it does not ship enabled applications by default
- the service is not automatically enabled or started
- service restart during upgrades is intentionally not forced

If needed after an upgrade:

```sh
sudo systemctl daemon-reload
sudo systemctl restart granian.service
```

## Basic Usage

Typical workflow:

1. Create an application config in `/etc/granian/apps-available/`
2. Enable it with `granianenconf`
3. Start or restart `granian.service`
4. Check logs in `/var/log/granian/`

Example:

```sh
sudo cp /etc/granian/apps-available/example.conf /etc/granian/apps-available/myapp.conf
sudo editor /etc/granian/apps-available/myapp.conf
sudo granianenconf myapp
sudo systemctl restart granian.service
sudo systemctl status granian.service
```

Disable an app:

```sh
sudo graniandisconf myapp
sudo systemctl restart granian.service
```

Single-app systemd workflow:

```sh
sudo granianenconf myapp
sudo systemctl start granian@myapp.service
sudo systemctl status granian@myapp.service
```

The instance unit `granian@myapp.service` loads:

- `/etc/granian/granian.conf`
- `/etc/granian/conf.d/*.conf`
- `/etc/granian/apps-enabled/myapp.conf`
- `/etc/granian/apps-enabled/myapp.d/*.conf`

## Configuration Model

There are two layers of configuration:

1. Global defaults in `/etc/granian/granian.conf` and `/etc/granian/conf.d/`
2. Per-app configuration in `/etc/granian/apps-available/*.conf`

The wrapper loads global config first, then app config. Per-app settings
override global defaults.

Format rules:

- `key = value`
- comments start with `#`
- most keys map to Granian CLI as `--key value`
- booleans map to `--key` / `--no-key`
- normal keys are last-wins, so per-app values override global defaults
- repeated keys are supported only for documented repeatable options

Wrapper-specific keys:

- `app`: required target in `module:object` form
- `working-directory`: directory used before spawning Granian
- `venv`: virtualenv root; requires executable `venv/bin/granian`
- `granian-bin`: explicit absolute path to a Granian executable
- `log-dir`: default log directory, usually `/var/log/granian`
- `log-file`: stdout log file override; must be under `/var/log/granian`
- `error-log-file`: stderr log file override; must be under `/var/log/granian`
- `user`: Unix user for the Granian child process
- `group`: Unix group for the Granian child process; requires `user`
- `restart-limit`: max failures allowed in `restart-window`
- `restart-window`: rolling failure window in seconds
- `restart-delay`: sleep before attempting restart
- `env`: repeatable `NAME=VALUE` env vars exported for the app
- `arg`: repeatable raw CLI args appended verbatim

Granian-specific keys:

- most options from `granian --help` work directly as config keys
- both `snake_case` and `snake-case` are accepted
- `websockets` is translated to Granian's `--ws` / `--no-ws`
- repeatable Granian keys append values: `env-files`, `static-path-route`,
  `static-path-mount`, `reload-paths`, `reload-ignore-dirs`,
  `reload-ignore-patterns`, `reload-ignore-paths`, `ssl-crl`

## Process Identity

The systemd supervisor starts as root so it can prepare per-app runtime paths
and then drop privileges independently for each app. Upstream Granian child
processes should not remain root.

Default behavior:

- no `user` or `group` configured: child runs as `granian:granian`
- `user` configured without `group`: child uses the user's primary group
- `group` configured without `user`: configuration check fails

Example:

```ini
user = myapp
group = myapp
```

The same keys can be placed in `/etc/granian/granian.conf` as global defaults
or in a per-app config to override the defaults.

## Example Application Config

A minimal application:

```ini
app = myapp.asgi:app
working-directory = /srv/myapp
venv = /srv/myapp/.venv
host = 127.0.0.1
port = 8000
interface = asgi
workers = 2
user = myapp
group = myapp
```

A more complete example is shipped in:

- `/etc/granian/apps-available/example.conf`

In the source tree, see:

- [packaging/example-app.conf](/root/granian/granian-src/packaging/example-app.conf)

That file is intentionally verbose and serves as a reference for supported
wrapper keys and common Granian options.

For Unix sockets, prefer an app-specific path under the runtime directory:

```ini
uds = /run/granian/myapp/myapp.sock
uds-permissions = 660
```

The wrapper prepares `/run/granian/myapp/` with ownership matching the app's
target `user`/`group` before dropping privileges.

## Logging

Supervisor logging:

- `journalctl -u granian.service`
- `journalctl -u granian@myapp.service`

Per-app logging:

- default stdout/stderr: `/var/log/granian/<app>/<app>.log`
- optional stderr override: `/var/log/granian/<app>/<app>.err.log`

The `<app>` name is taken from the enabled config file name without `.conf`.
For `/etc/granian/apps-enabled/myapp.conf`, the default log file is:

```text
/var/log/granian/myapp/myapp.log
```

The supervisor creates the app-specific log directory and sets ownership to the
target `user`/`group` before starting the child process. Explicit `log-file`
and `error-log-file` settings are respected only when they stay under
`/var/log/granian`.

The package also installs:

- `/etc/logrotate.d/granian`

Current logrotate behavior:

- covers the current `/var/log/granian/<app>/*.log` layout
- also covers legacy `/var/log/granian/*.log` files
- daily rotation
- rotation before a log grows beyond `20M`
- 14 retained files
- compression enabled
- `copytruncate`

## Restart Behavior

The wrapper contains crash-loop protection.

Relevant keys:

- `restart-limit`
- `restart-window`
- `restart-delay`

If an application fails too many times inside the configured time window, the
wrapper stops restarting that application and leaves it disabled until the
whole service is restarted.

This avoids infinite restart loops caused by broken code, bad config, missing
directories, locked databases, and similar failures.

## systemd Integration

The package ships one service:

- `granian.service`
- `granian@.service`

`granian.service` starts the global supervisor, which then starts one Granian
process per enabled app.

`granian@app.service` starts the same wrapper in instance mode and manages only
the app selected by `%i`.

Useful commands:

```sh
sudo systemctl status granian.service
sudo systemctl start granian.service
sudo systemctl stop granian.service
sudo systemctl restart granian.service
sudo journalctl -u granian.service
sudo journalctl -u granian@myapp.service
```

Conflict handling:

- `granian.service` conflicts with `granian@.service`
- `granian@.service` conflicts with `granian.service`

The intention is to avoid running the global supervisor and a per-app unit at
the same time for the same installation.

The package does not automatically create enabled applications. You decide what
to enable and whether to run them through the global unit or individual units.

The supervisor unit intentionally does not set `User=`/`Group=`. The wrapper
must start as root so it can prepare `/var/log/granian/<app>/` and
`/run/granian/<app>/`, then drop each child process to the configured identity.

## Virtualenv Support

Per-app virtualenv usage is supported through:

```ini
venv = /srv/myapp/.venv
```

Behavior:

- the wrapper sets `VIRTUAL_ENV`
- prepends `venv/bin` to `PATH`
- requires `venv/bin/granian` when `venv` is set
- uses `granian-bin = /absolute/path` if an explicit executable is needed

This is useful when the application and its Python dependencies are managed in
their own venv, while the Debian package still provides the service wrapper and
system integration.

Diagnose the resolved command without starting the app:

```sh
sudo granian-wrapper --dry-run --instance myapp
```

## Helper Commands

Enable an application:

```sh
sudo granianenconf myapp
```

Disable an application:

```sh
sudo graniandisconf myapp
```

The helper commands operate on:

- `/etc/granian/apps-available/<name>.conf`
- `/etc/granian/apps-enabled/<name>.conf`

They are analogous to the Apache enable/disable pattern, but they manage app
configuration symlinks rather than vhosts.

## Building The Package

The Debian source tree lives in:

- `/root/granian/granian-src`

Build a source package:

```sh
cd granian-src
debuild -us -uc -S
```

Build a binary package:

```sh
cd granian-src
dpkg-buildpackage -us -uc -b
```

Build in pbuilder:

```sh
cd granian-src
debuild -us -uc -S
pbuilder build ../granian_1.0.x.dsc
```

The project is a native Debian package:

- source format: `3.0 (native)`

Build dependencies are intentionally minimal so the package can be built in
restricted environments.

## Repository Layout

Top-level repository:

```text
README.md
granian-src/
```

Main project layout:

```text
granian-src/
  debian/
  packaging/
  src/
  Makefile
  README.md
```

Directory roles:

- `src/`: C++ source code for the wrapper and helper tools
- `packaging/`: installed config templates and service files
- `debian/`: Debian packaging metadata

## Notes

- the package expects upstream `python3-granian` to be installed
- the wrapper is responsible for process supervision, config parsing and log
  redirection
- upstream Granian is still responsible for serving the Python application
- if a specific Granian option is not conveniently expressed as `key = value`,
  use repeatable `arg = ...` lines
- `--dry-run` and `--print-command` print the resolved command, log paths and
  target identity without starting child processes

## Summary

Use this package when you want:

- a Debian-style Granian service package
- multiple apps under one supervisor service
- enable/disable helpers for app configs
- log files under `/var/log/granian`
- per-app Unix users and groups
- systemd integration and package-managed defaults
- upstream Granian left intact as the actual Python app server
