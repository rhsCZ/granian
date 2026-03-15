# granian

`granian` is a small C++ supervisor around the upstream
`python3-granian` package. It reads global defaults from
`/etc/granian/granian.conf` plus optional `/etc/granian/conf.d/*.conf`, then
loads every enabled app from `/etc/granian/apps-enabled/*.conf` and starts one
Granian process per app.

Layout:

```text
/etc/granian/granian.conf
/etc/granian/conf.d/*.conf
/etc/granian/apps-available/*.conf
/etc/granian/apps-enabled/*.conf
```

Typical enable flow:

```sh
granianenconf myapp
systemctl restart granian.service
```

Per-app config format:

```ini
app = myapp.asgi:app
working-directory = /srv/myapp
venv = /srv/myapp/.venv
host = 0.0.0.0
port = 8080
interface = asgi
workers = 4
websockets = true
env = PYTHONPATH=/srv/myapp
arg = --log-level
arg = info
```

Rules:

- Global config is loaded first, app config overrides it.
- `app=` is required in each enabled app file.
- `working-directory=` changes cwd only for that app process.
- `venv=` activates the given Python virtualenv for that app process. If
  `venv/bin/granian` exists, it is used instead of `/usr/bin/granian`.
- `websockets=` is translated to Granian's `--ws` / `--no-ws`.
- `restart-limit=`, `restart-window=` and `restart-delay=` prevent infinite
  crash loops; after too many failures in the configured window, the app is
  left disabled until the service is restarted.
- app stdout/stderr are written to `/var/log/granian/<app>.log` by default,
  with optional `log-file=` and `error-log-file=` overrides.
- Other keys are converted to Granian CLI options (`workers=4` ->
  `--workers 4`).
- Boolean values become `--key` or `--no-key`.
- `arg=` appends a raw argument.
- `env=` exports `NAME=VALUE` only for the spawned app process.

The package ships one `granian.service` systemd unit using the dedicated
`granian` system user. The supervisor restarts failed child apps and stops all
children when the unit is stopped, logs supervisor output to
`/var/log/granian/supervisor.log`, and installs `logrotate` config for the log
files under `/var/log/granian`.

Admin helpers:

- `granianenconf myapp` enables `/etc/granian/apps-available/myapp.conf`
- `graniandisconf myapp` disables `/etc/granian/apps-enabled/myapp.conf`
