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
not already present — no separate install step is needed.

3. **Create the D1 database**:

```sh
npx wrangler d1 create shyake-db
```

Copy the `database_id` from the output.

4. **Edit `server/wrangler.toml`** in your fork:

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
```

The `[[d1_databases]]` block must be present and contain the
correct `database_id`. Without it the Worker has no database
binding and every request will fail.

You can use the default `*.workers.dev` URL as `INSTANCE_DOMAIN`
if you do not have a custom domain.

5. **Apply database migrations** (creates all tables):

```sh
cd server
npx wrangler d1 migrations apply shyake-db --remote
```

Cloudflare's CI pipeline does not apply database migrations
automatically. You must run `wrangler d1 migrations apply` once
manually. Skipping this leaves the database empty and the Worker
will error on every API call.

6. **Deploy**

Choose one of the following:

**Option A — Dashboard**: Go to
`Compute → Workers & Pages → Create application → Continue with GitHub`
in the Cloudflare Dashboard (you may need to `Add GitHub account` at the first
time), select your fork, and set:

| Field | Value |
|-------|-------|
| Framework preset | None |
| Build command | `` |
| Deploy command | `npx wrangler deploy` |
| Root directory | `/server` |

Future pushes to your fork will redeploy automatically.

**Option B — CLI only**:

```sh
cd server
npm install
npx wrangler deploy
```

7. **Verify**

Wait for deployment to complete and then open
`https://<worker>.workers.dev/health` (or your custom domain).
A `200 OK` confirms the Worker and database are working correctly.

### Self-hosting

WIP
