## Shyake Deployment Guide

The server runs as a Cloudflare Worker with a D1 database.
However, you can also self-host it on your own hardware.

There are 2 ways to deploy the server:

* Using Cloudflare
* Self-hosting

**Federation**

Two instances federate automatically when both have
`FEDERATION_ENABLED = true`. You need no additional configuration.
The server routes cross-instance mail directly, server-to-server.
Clients only ever talk to their own instance.

To disable inbound and outbound federation:

```toml
FEDERATION_ENABLED = false
```

### Using Cloudflare

Everything runs from your own machine with the Wrangler CLI. You do
not need to fork this repository, connect it to Cloudflare, or touch
the dashboard.

Prerequisites:

- Node.js 18+
- A Cloudflare account

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/cf
./deploy.sh
```

`deploy.sh` runs the whole deployment:

1. installs the Worker's dependencies
2. runs `npx wrangler login` if you are not authenticated yet
3. asks for your instance domain
4. creates the D1 database and the KV cache namespace, reusing them
   if they already exist
5. writes `server/cf/wrangler.toml` with the resulting ids
6. applies the database migrations
7. deploys the Worker and checks `/health`

Your instance domain appears in every address on your instance
(`user@your.domain.example`). Other instances use it to route
federated mail back to you. If you do not have a custom domain, the
default `*.workers.dev` URL works.

Options:

| Option | Effect |
|---|---|
| `--domain <d>` | set the instance domain non-interactively |
| `--update` | pull the latest code first, then redeploy |
| `--no-kv` | skip the KV version cache |
| `--config-only` | write `wrangler.toml` and stop |
| `--local` | set up for local self-hosting instead (see below) |

#### Upgrading

```sh
cd shyake/server/cf
./deploy.sh --update
```

This pulls the latest code, reapplies any new migrations, and
redeploys. The script reuses existing resources and keeps your
settings.

#### Changing settings

`deploy.sh` generates `server/cf/wrangler.toml` from
`wrangler.template.toml` on first run. Git does **not** track it, so
your instance settings survive `git pull` and never conflict. Edit
it, then re-run `./deploy.sh`:

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example"
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB; do not exceed 786432 (768 KiB)
```

If you re-run the script, it never overwrites these values. It only
fills in resource ids that are still unset.

If you deployed an instance before `wrangler.toml` became generated,
`./deploy.sh --update` moves your settings aside as
`wrangler.toml.bak` and restores them after pulling.

### Self-hosting

Self-hosting runs the exact same Worker code on your own machine,
inside the local `workerd` runtime that ships with Wrangler.
Wrangler itself emulates D1 (SQLite) and KV locally, so **you need
no Cloudflare account**. No `wrangler login`, no resource creation on
the dashboard.

Prerequisites:

- Node.js 18+
- A machine that stays online (any OS Node.js supports, though the
  examples below assume Linux with systemd)
- For federation: a public domain name pointing at the machine, and a
  reverse proxy with a valid TLS certificate (see below)

Steps:

1. **Set it up** (you do not need to fork):

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/cf
./deploy.sh --local --domain your.domain.example
```

`--local` skips everything that needs a Cloudflare account: no
`wrangler login`, no remote resources. It installs the dependencies,
writes `wrangler.toml`, and creates the local SQLite database.

`--domain` must be the public domain your instance is reachable at.
Your instance domain appears in every address on your instance
(`user@your.domain.example`). Other instances use it to route
federated mail back to you. If you omit the flag, the script asks
for it.

2. **Adjust settings**, if you want to, in the generated
`server/cf/wrangler.toml`. Only the `[vars]` section matters. Local
mode ignores the `database_id` and KV `id`:

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example"
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB; do not exceed 786432 (768 KiB)
```

Git does not track the file, so your edits survive `git pull`.
Upgrade later with `./deploy.sh --update --local`.

3. **Run the server**:

```sh
npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

Verify with `curl http://127.0.0.1:8787/health`. A `200 OK` means
the Worker and database are working.

Keep the server bound to `127.0.0.1` and let a reverse proxy handle
outside traffic (next step). If you bind directly to `0.0.0.0`, this
is only reasonable on a trusted LAN without federation.

4. **Set up a reverse proxy with TLS**

This step is **required for federation**. Instances always contact
each other over `https://<domain>/...`, so your instance must be
reachable at `https://your.domain.example` with a certificate other
instances accept. A self-signed certificate does not work. If your
instance is private (its users only mail each other), you can skip
this step and let clients connect over plain HTTP.

With [Caddy](https://caddyserver.com/), Caddy obtains and renews
certificates automatically. The entire `Caddyfile` is:

```
your.domain.example {
    reverse_proxy 127.0.0.1:8787
}
```

nginx with a certbot-managed certificate works just as well. Proxy
`https://your.domain.example` to `http://127.0.0.1:8787`.

5. **Keep it running**

`wrangler dev` is a foreground process. Use a supervisor to start it
on boot and restart it on failure. Here is a minimal systemd unit
(`/etc/systemd/system/shyake.service`):

```ini
[Unit]
Description=Shyake server (local workerd)
After=network-online.target
Wants=network-online.target

[Service]
User=shyake
WorkingDirectory=/home/shyake/shyake/server/cf
ExecStart=/usr/bin/npx wrangler dev --local --ip 127.0.0.1 --port 8787
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now shyake
```

**Data location and backups**

All local state (the D1 SQLite database and the KV cache) lives
under `server/cf/.wrangler/state/`. To back up your instance, back
up that directory. Stop the server first, or use SQLite-safe
tooling, to avoid copying a database mid-write. If you delete the
directory, the instance resets to an empty database. Pass
`--persist-to <dir>` to `wrangler dev` to store state somewhere
else.

**Caveats: Know what you are running**

`wrangler dev` is Wrangler's development server, not a hardened
production server. It runs the same `workerd` runtime that powers
Cloudflare Workers. For a personal or small-community instance, it
holds up fine. Be aware of its development-oriented behavior,
though:

- **File watching / hot reload.** It watches the source tree and
  reloads the Worker when files change. This is convenient in
  development. On a server, though, it means an edit or a
  `git pull` in `server/cf/` restarts your instance immediately.
  Update deliberately: pull, review, then let it reload (or restart
  the service yourself).
- **Single process, no supervision of its own.** There is no
  clustering and no built-in crash recovery. The systemd unit above
  provides that supervision.
- **No rate limiting or DDoS protection.** On Cloudflare those come
  with the platform. When you self-host, add rate limits at your
  reverse proxy if your instance is publicly reachable.
- **Interactive keybindings.** `wrangler dev` reads hotkeys from
  stdin if you run it attached to a terminal. Under systemd there is
  no TTY, so this is a non-issue. If you run it in `tmux` instead,
  avoid stray keypresses (`x` clears the console, `Ctrl+C` exits).

If your instance outgrows this setup, the Cloudflare deployment path
above is the scalable option. You can migrate the database: export
the local SQLite file, then import it with
`wrangler d1 execute --remote`.
