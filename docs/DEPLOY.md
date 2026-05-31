## Shyake Deployment Guide

The server runs as a Cloudflare Worker with a D1 database.
However, you can also self-host it on your own hardware.

So there are 3 ways to deploy the server:

* Using Cloudflare Dashboard (RECOMMEND)
* Using Cloudflare Wrangler CLI
* Self-hosting

**Federation**

Two instances federate automatically when both have `FEDERATION_ENABLED = true`.
No additional configuration is required. Cross-instance mail is routed
server-to-server; clients only ever talk to their own instance.

To disable inbound and outbound federation:

```toml
FEDERATION_ENABLED = false
```

### Using Cloudflare Dashboard

We recommend you do in this way. This will ensure that your instance is
open, transparent, and observable to users of your instance.

Steps:

1. Fork this repo on GitHub

2. Create a Cloudflare account

3. Log in to the Cloudflare Dashboard

4. Navigate to `Build / Compute / Workers & Pages`

5. Click `Create application` and choose `Continue with GitHub`

6. Choose the repo you just forked

7. Wait for the deployment to complete

8. Check if everything goes well

### Using Cloudflare Wrangler CLI

Prerequisites:

- Node.js 18+
- A Cloudflare account

Steps:

1. Install and authenticate Wrangler CLI

```sh
npm install -g wrangler
npx wrangler login
```

2. Install dependencies

```sh
cd server
npm install
```

3. Create the D1 database

```sh
npx wrangler d1 create shyake-db
```

Copy the `database_id` from the output and paste it into `wrangler.toml`:

```toml
[[d1_databases]]
binding = "DB"
database_name = "shyake-db"
database_id = "<your-database-id>"
```

4. Apply migrations

```sh
npx wrangler d1 migrations apply shyake-db --remote
```

5. Configure `wrangler.toml`

```toml
[vars]
INSTANCE_DOMAIN    = "your.domain.example"    # public hostname of this instance
REGISTRATION_ENABLED = true                   # set false to close registrations
RESERVED_USERNAMES = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED = true                     # set false to isolate this instance
MAX_MAIL_SIZE      = 196608                   # max payload bytes (default 192 KiB)
```

You can also use the default Worker URL as instance domain.

Recommend that you do not set `MAX_MAIL_SIZE` to a value greater than 786432
(768 KiB), as this is limited by Cloudflare D1's per-row size.

6. Deploy

```sh
npx wrangler deploy
```

Wait for deployment to complete and check if everything goes well.

### Self-hosting
