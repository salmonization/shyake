## Shyake Deployment Guide

The server runs as a Cloudflare Worker with a D1 database.
However, you can also self-host it on your own hardware.

There are 2 ways to deploy the server:

* Using Cloudflare
* Self-hosting

**Federation**

Two instances federate automatically when both have
`FEDERATION_ENABLED = true`. No additional configuration is required.
Cross-instance mail is routed server-to-server; clients only ever talk
to their own instance.

To disable inbound and outbound federation:

```toml
FEDERATION_ENABLED = false
```

### Using Cloudflare

Prerequisites:

- Node.js 18+
- A Cloudflare account

Steps:

1. **Fork and clone** this repo on GitHub.

2. **Authenticate** Cloudflare **Wrangler CLI** in your terminal:

```sh
npx wrangler login
```

`npx` will prompt you to install Wrangler on first run if it is
not already present. No separate install step is needed.

3. **Create the D1 database**:

```sh
npx wrangler d1 create shyake-db
```

Copy the `database_id` from the output.

4. **Create the KV namespace** (version relay cache):

```sh
npx wrangler kv namespace create VERSION_CACHE
```

Copy the `id` from the output. Every instance relays the GitHub
Releases API for `shyake update` on its own clients; this KV
namespace caches the lookup for one hour. The binding is optional —
without it the endpoint still works but hits GitHub on every
request.

5. **Edit `server/cf/wrangler.toml`** in your fork:

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example" # edit this
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB; do not exceed 786432 (768 KiB)

[[d1_databases]]
binding        = "DB"
database_name  = "shyake-db"
database_id    = "<your database_id>" # paste your database_id here
migrations_dir = "migrations"

[[kv_namespaces]]
binding = "VERSION_CACHE"
id      = "<your kv namespace id>" # paste your KV namespace id here
```

The `[[d1_databases]]` block must be present and contain the
correct `database_id`. Without it the Worker has no database
binding and every request will fail.

You can use the default `*.workers.dev` URL as `INSTANCE_DOMAIN`
if you do not have a custom domain.

6. **Apply database migrations** (creates all tables):

```sh
cd server/cf
npx wrangler d1 migrations apply shyake-db --remote
```

Cloudflare's CI pipeline does not apply database migrations
automatically. You must run `wrangler d1 migrations apply` once
manually. Skipping this leaves the database empty and the Worker
will error on every API call.

7. **Deploy**

Choose one of the following:

**Option A: Dashboard**: Go to
`Compute → Workers & Pages → Create application → Continue with GitHub`
in the Cloudflare Dashboard (you may need to `Add GitHub account` at the first
time), select your fork, and set:

| Field | Value |
|-------|-------|
| Framework preset | None |
| Build command | None |
| Deploy command | `npx wrangler deploy` |
| Root directory | `/server/cf` |

Future pushes to your fork will redeploy automatically.

**Option B: CLI only**:

```sh
cd server/cf
npm install
npx wrangler deploy
```

8. **Verify**

Wait for deployment to complete and then open
`https://<worker>.workers.dev/health` (or your custom domain).
A `200 OK` confirms the Worker and database are working correctly.

### Self-hosting

Self-hosting runs the exact same Worker code on your own machine,
inside the local `workerd` runtime that ships with Wrangler. D1
(SQLite) and KV are emulated locally by Wrangler itself, so **no
Cloudflare account is needed**. No `wrangler login`, no resource
creation on the dashboard.

Prerequisites:

- Node.js 18+
- A machine that stays online (any OS Node.js supports; the examples
  below assume Linux with systemd)
- For federation: a public domain name pointing at the machine, and a
  reverse proxy with a valid TLS certificate (see below)

Steps:

1. **Clone** this repo (a fork is not required):

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/cf
npm install
```

2. **Edit `server/cf/wrangler.toml`**: only the `[vars]` section
matters. The `database_id` and KV `id` are ignored in local mode, so
the placeholder values can stay as they are:

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example" # edit this
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB; do not exceed 786432 (768 KiB)
```

`INSTANCE_DOMAIN` must be the domain your instance is reachable at
from the outside. It is embedded in every address on your instance
(`user@your.domain.example`) and other instances use it to route
federated mail back to you.

3. **Apply database migrations** locally (creates all tables):

```sh
npx wrangler d1 migrations apply shyake-db --local
```

Note the `--local` flag. This writes to a SQLite file on disk
instead of a Cloudflare-hosted database.

4. **Run the server**:

```sh
npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

Verify with `curl http://127.0.0.1:8787/health`. A `200 OK` means
the Worker and database are working.

Keep the server bound to `127.0.0.1` and let a reverse proxy handle
outside traffic (next step). Binding to `0.0.0.0` directly is only
reasonable on a trusted LAN without federation.

5. **Set up a reverse proxy with TLS**

This step is **required for federation**. Instances always contact
each other over `https://<domain>/...`, so your instance must be
reachable at `https://your.domain.example` with a certificate that
other instances will accept. A self-signed certificate will not
work. If your instance is private (its users only mail each other),
you can skip this step and let clients connect over plain HTTP.

With [Caddy](https://caddyserver.com/), certificates are obtained
and renewed automatically; the entire `Caddyfile` is:

```
your.domain.example {
    reverse_proxy 127.0.0.1:8787
}
```

nginx with a certbot-managed certificate works just as well. Proxy
`https://your.domain.example` to `http://127.0.0.1:8787`.

6. **Keep it running**

`wrangler dev` is a foreground process; use a supervisor to start it
on boot and restart it on failure. A minimal systemd unit
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
under `server/cf/.wrangler/state/`. Backing up your instance means
backing up that directory (stop the server first, or use SQLite-safe
tooling, to avoid copying a database mid-write). Deleting it resets
the instance to an empty database. Pass `--persist-to <dir>` to
`wrangler dev` to store state somewhere else.

**Caveats: Know what you are running**

`wrangler dev` is Wrangler's development server, not a hardened
production server. It runs the same `workerd` runtime that powers
Cloudflare Workers, and for a personal or small-community instance
it holds up fine, but be aware of its development-oriented behavior:

- **File watching / hot reload.** It watches the source tree and
  reloads the Worker when files change. Convenient in development,
  but on a server it means an edit or a `git pull` in `server/cf/`
  restarts your instance immediately. Update deliberately: pull,
  review, then let it reload (or restart the service yourself).
- **Single process, no supervision of its own.** There is no
  clustering and no built-in crash recovery. That is what the
  systemd unit above is for.
- **No rate limiting or DDoS protection.** On Cloudflare those come
  with the platform. Self-hosted, your reverse proxy is the place to
  add rate limits if your instance is publicly reachable.
- **Interactive keybindings.** `wrangler dev` reads hotkeys from
  stdin when attached to a terminal. Under systemd there is no TTY,
  so this is a non-issue, but if you run it in `tmux` instead, avoid
  stray keypresses (`x` clears the console, `Ctrl+C` exits).

If your instance outgrows this setup, the Cloudflare deployment path
above is the scalable option. The database can be migrated by
exporting the local SQLite file and importing it with
`wrangler d1 execute --remote`.
