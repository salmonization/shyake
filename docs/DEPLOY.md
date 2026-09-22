## Shyake Deployment Guide

Shyake has two server implementations with the same HTTP API. A
client works with either one, and the two kinds federate with each
other.

* **Using Cloudflare**: the Worker in `server/cf/`, on Cloudflare
  Workers with a D1 database. You need no machine of your own.
* **Self-hosting**: the Go server in `server/go/`, one binary with
  one SQLite file, on your own machine.

**Federation**

Two instances federate automatically when both have federation
enabled, which is the default. You need no additional configuration.
The server routes cross-instance mail directly, server-to-server.
Clients only ever talk to their own instance.

To disable inbound and outbound federation, set
`FEDERATION_ENABLED = false` in `wrangler.toml` (Worker) or
`SHYAKE_FEDERATION_ENABLED=false` (Go server).

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
| `--local` | set up a local development server ([DEV.md](DEV.md)) |

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

The Go server runs on your own machine. It is one static binary,
`shyake-server`, and keeps all data in one SQLite file. It needs no
Node.js and no Cloudflare account.

Prerequisites:

- A machine that stays online. The examples below assume Linux with
  systemd.
- Go 1.26 or newer to build the binary, or Docker.
- For federation: a public domain name that points at the machine,
  and a reverse proxy with a valid TLS certificate (step 4).

Steps:

1. **Build the binary**:

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/go
CGO_ENABLED=0 go build -trimpath -o shyake-server ./cmd/shyake-server
```

The result is a static binary. You can copy it to any Linux machine
with the same CPU architecture.

2. **Install it with systemd**. Create a system user for the
service, then install the files from `server/go/deploy/`:

```sh
sudo useradd --system --home-dir /var/lib/shyake --shell /usr/sbin/nologin shyake
sudo install -m 755 shyake-server /usr/local/bin/
sudo install -D -m 640 -g shyake deploy/shyake.env.example /etc/shyake/shyake.env
sudo install -m 644 deploy/shyake-server.service /etc/systemd/system/
```

3. **Configure it**. Edit `/etc/shyake/shyake.env` and set at least
the instance domain:

```sh
SHYAKE_INSTANCE_DOMAIN=your.domain.example
SHYAKE_LISTEN=127.0.0.1:8787
```

Your instance domain appears in every address on your instance
(`user@your.domain.example`). Other instances use it to route
federated mail back to you. The file lists every other setting with
its default. [SPEC.md §11.2](SPEC.md) describes them all.

Then start the service:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now shyake-server
curl http://127.0.0.1:8787/health
```

A `200 OK` means the server and its database work. The service runs
as the `shyake` user. It can write only to `/var/lib/shyake`, where
the database lives.

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

The server limits requests per client address. Behind a proxy, it
reads the client address from `X-Forwarded-For`, but only when the
proxy is in `SHYAKE_TRUSTED_PROXIES`. The default trusts proxies on
the same machine (`127.0.0.1`, `::1`). If your proxy runs elsewhere,
add its address. Otherwise all clients share the proxy's address and
one rate limit.

#### Running it with Docker

```sh
docker build -t shyake-server server/go
docker run -d --name shyake --restart unless-stopped \
    -p 127.0.0.1:8787:8787 -v shyake:/data \
    -e SHYAKE_INSTANCE_DOMAIN=your.domain.example \
    -e SHYAKE_TRUSTED_PROXIES=172.16.0.0/12 \
    shyake-server
```

The database is `/data/shyake.db` on the `shyake` volume. The
container sees the reverse proxy at the Docker bridge address, so
set `SHYAKE_TRUSTED_PROXIES` to the bridge network, as above.

#### Upgrading

```sh
cd shyake
git pull
cd server/go
CGO_ENABLED=0 go build -trimpath -o shyake-server ./cmd/shyake-server
sudo install -m 755 shyake-server /usr/local/bin/
sudo systemctl restart shyake-server
```

The server applies new database migrations when it starts. On
restart it finishes the relays already in progress, and it resumes
the queued ones when it starts again.

#### Data location and backups

All data is in one SQLite file: `/var/lib/shyake/shyake.db` under
systemd, `/data/shyake.db` in Docker. The database runs in WAL mode,
so two more files (`-wal`, `-shm`) sit next to it while the server
runs.

To back up a running server, use SQLite's online backup, which is
safe during writes:

```sh
sudo sqlite3 /var/lib/shyake/shyake.db ".backup /root/shyake-backup.db"
```

Or stop the service and copy the three files.

#### Moving from the Worker

The Go server can take over the data of a Worker instance: users,
mail, and blocks. Keep the same instance domain, because stored
addresses depend on it.

From a Worker on Cloudflare, export the database first:

```sh
cd shyake/server/cf
npx wrangler d1 export shyake-db --remote --output=d1-export.sql
```

From a local `wrangler dev` instance, stop it and use its database
file. It is the `.sqlite` file under
`server/cf/.wrangler/state/v3/d1/miniflare-D1DatabaseObject/` that is
not named `metadata.sqlite`. The file runs in WAL mode: most of its
data is in the `-wal` file next to it. If you copy the database, copy
its `-wal` file too.

Then import into a new database as the `shyake` user, before the
service first starts. The user must be able to read the export file:

```sh
sudo install -d -o shyake -g shyake -m 700 /var/lib/shyake
sudo install -o shyake -m 600 d1-export.sql /var/lib/shyake/
# for a D1 file instead: install both <file>.sqlite and <file>.sqlite-wal
sudo -u shyake env \
    SHYAKE_INSTANCE_DOMAIN=your.domain.example \
    SHYAKE_DATABASE=/var/lib/shyake/shyake.db \
    shyake-server -import-d1 /var/lib/shyake/d1-export.sql
sudo rm /var/lib/shyake/d1-export.sql
```

The import refuses a database that already has users. It reports
what it could not copy unchanged:

- Two names that differ only by case: the older account keeps the
  name. The Go server does not allow such pairs.
- A mail stored twice under the same signature: it keeps one copy.

It also rewrites block entries to the normalized form ([SPEC.md
§4](SPEC.md)).

Then point your domain at the new machine. Clients need no change:
their keys and addresses stay the same.

#### Database

The Go server supports SQLite only. The storage layer sits behind an
interface, with a test suite that every backend must pass.
PostgreSQL support is planned on that basis. Until then, the server
refuses a `postgres://` value in `SHYAKE_DATABASE`.
