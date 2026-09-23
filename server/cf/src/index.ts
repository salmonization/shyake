import {Hono} from 'hono';
import {
  isValidUsername,
  isReservedUsername,
  verifyPoW,
  normalizeAddress,
  addressDomain,
  relayMail,
  readSignedBody,
} from './utils';

// Import the Web build which allows manual instantiation
import initWasm, {verify as verifySignature} from 'mldsa65-wasm/web/mldsa65.js';
// Import the WebAssembly module directly (handled by Wrangler)
import wasmModule from 'mldsa65-wasm/mldsa65_bg.wasm';

// convert base64 to base64url format
const toBase64Url = (b64: string): string => {
  if (!b64) {
    return '';
  }
  return b64
    .replace(/\+/g, '-')
    .replace(/\//g, '_')
    .replace(/=/g, '')
    .replace(/\s+/g, '');
};

export interface Env {
  DB: D1Database;
  VERSION_CACHE?: KVNamespace;
  INSTANCE_DOMAIN: string;
  REGISTRATION_ENABLED: string;
  RESERVED_USERNAMES: string;
  FEDERATION_ENABLED: string;
  MAX_MAIL_SIZE: string;
  /* optional: authenticates the GitHub release lookup, which is
   * otherwise limited per IP address, and Worker IPs are shared */
  GITHUB_TOKEN?: string;
}

/* release tag from server/VERSION, set by deploy.sh through
 * `wrangler deploy --define`; absent under a plain `wrangler dev` */
declare const SERVER_VERSION: string;

/* protocol level: 2 signs the bodies of header-authenticated requests */
const PROTOCOL_LEVEL = 2;

const app = new Hono<{Bindings: Env}>();

app.get('/health', async c => {
  try {
    await c.env.DB.prepare('SELECT 1').first();
    return c.text('200 OK', 200);
  } catch (e: any) {
    return c.text(`500 Internal Server Error: ${e.message}`, 500);
  }
});

app.get('/api/version', c =>
  c.json(
    {
      version: typeof SERVER_VERSION === 'string' ? SERVER_VERSION : 'dev',
      implementation: 'cf',
      protocol: PROTOCOL_LEVEL,
    },
    200,
  ),
);

/* proxy GitHub Releases API: a fresh answer is cached for one hour in
 * KV; the last good answer is kept without expiry and served when
 * GitHub fails, as the Go server does */
app.get('/api/client/version', async c => {
  const CACHE_KEY = 'client_version_v3';
  const LAST_KEY = 'client_version_last';
  const CACHE_TTL = 3600;

  const kvGet = async (key: string): Promise<string | null> => {
    try {
      return c.env.VERSION_CACHE ? await c.env.VERSION_CACHE.get(key) : null;
    } catch (_) {
      return null;
    }
  };
  const lastOrFail = async (why: string) => {
    const last = await kvGet(LAST_KEY);
    console.error(
      `client version: GitHub ${why}; ${last ? 'serving stale' : 'no stale answer'}`,
    );
    return last
      ? c.json(JSON.parse(last), 200)
      : c.json({error: 'Failed to fetch releases'}, 502);
  };

  const cached = await kvGet(CACHE_KEY);
  if (cached) {
    return c.json(JSON.parse(cached), 200);
  }

  try {
    const headers: Record<string, string> = {
      'User-Agent': 'shyake-server/1.0',
      Accept: 'application/vnd.github+json',
    };
    if (c.env.GITHUB_TOKEN) {
      headers.Authorization = `Bearer ${c.env.GITHUB_TOKEN}`;
    }
    const resp = await fetch(
      'https://api.github.com/repos/salmonization/shyake' + '/releases',
      {headers},
    );
    if (!resp.ok) {
      const left = resp.headers.get('x-ratelimit-remaining') ?? '?';
      const text = (await resp.text()).slice(0, 160);
      return await lastOrFail(
        `answered HTTP ${resp.status}, ${left} calls left: ${text}`,
      );
    }
    const releases: any[] = await resp.json();

    let release: string | null = null;
    let pre_release: string | null = null;
    const release_digests: Record<string, string> = {};
    const pre_release_digests: Record<string, string> = {};

    // collect per-asset sha256 digests from GitHub
    const collectDigests = (r: any, out: Record<string, string>) => {
      for (const a of r.assets ?? []) {
        if (typeof a.digest === 'string' && a.digest.startsWith('sha256:')) {
          out[a.name] = a.digest.slice(7);
        }
      }
    };

    /* a release with only server builds (shyake-server-*) has nothing
     * for clients to install */
    const hasClientAsset = (r: any) =>
      (r.assets ?? []).some(
        (a: any) =>
          typeof a.name === 'string' &&
          a.name.startsWith('shyake-') &&
          !a.name.startsWith('shyake-server-') &&
          a.name.endsWith('.tar.gz'),
      );

    for (const r of releases) {
      if (r.draft || !hasClientAsset(r)) continue;
      if (!r.prerelease && !release) {
        release = r.tag_name;
        collectDigests(r, release_digests);
      }
      if (r.prerelease && !pre_release) {
        pre_release = r.tag_name;
        collectDigests(r, pre_release_digests);
      }
      if (release && pre_release) break;
    }

    const payload: Record<string, any> = {};
    if (release) {
      payload.release = release;
      payload.release_digests = release_digests;
    }
    if (pre_release) {
      payload.pre_release = pre_release;
      payload.pre_release_digests = pre_release_digests;
    }

    try {
      if (c.env.VERSION_CACHE) {
        const body = JSON.stringify(payload);
        await c.env.VERSION_CACHE.put(CACHE_KEY, body, {
          expirationTtl: CACHE_TTL,
        });
        await c.env.VERSION_CACHE.put(LAST_KEY, body);
      }
    } catch (_) {}

    return c.json(payload, 200);
  } catch (e: any) {
    return await lastOrFail(`failed: ${e.message}`);
  }
});

app.post('/api/register', async c => {
  const regEnabled = String(c.env.REGISTRATION_ENABLED) === 'true';
  if (!regEnabled) {
    return c.json({error: 'Registration is disabled'}, 403);
  }

  const body = await c.req.json();
  const {username, kem_pubkey, sig_pubkey, timestamp, signature, pow} = body;

  if (
    !username ||
    !kem_pubkey ||
    !sig_pubkey ||
    !timestamp ||
    !signature ||
    !pow
  ) {
    return c.json({error: 'Missing required fields'}, 400);
  }

  if (!isValidUsername(username)) {
    return c.json({error: 'Invalid username format'}, 400);
  }
  if (isReservedUsername(username, c.env.RESERVED_USERNAMES)) {
    return c.json({error: 'Username is reserved'}, 403);
  }

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) {
    return c.json({error: 'Invalid Proof of Work'}, 403);
  }
  // 3. Verify Timestamp (anti-replay)
  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300) {
    return c.json({error: 'Timestamp out of window'}, 403);
  }

  // Initialize the WASM module for Cloudflare Workers
  await initWasm({module_or_path: wasmModule});

  // 4. Verify Signature
  try {
    // Reconstruct the signed payload
    const signedData = {username, kem_pubkey, sig_pubkey, timestamp};
    const message = JSON.stringify(signedData);

    const msgBytes = new TextEncoder().encode(message);

    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(sig_pubkey);

    // verify(vk, message, signature, context)
    const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
    if (!isSigValid) {
      return c.json({error: 'Invalid signature'}, 401);
    }
  } catch (e) {
    console.error('Signature verification error:', e);
    return c.json({error: 'Signature verification failed'}, 401);
  }

  /* Reject a name that differs from an existing one only by case:
   * "Alice" alongside "alice" is an impersonation vector. The guard
   * rides on the INSERT so a racing registration cannot slip past a
   * separate SELECT. COLLATE NOCASE folds ASCII only, which is the
   * whole of the username charset. */
  try {
    const res = await c.env.DB.prepare(
      'INSERT INTO users (username, kem_pubkey, sig_pubkey, ' +
        'created_at) SELECT ?, ?, ?, ? WHERE NOT EXISTS ' +
        '(SELECT 1 FROM users WHERE username = ? COLLATE NOCASE)',
    )
      .bind(username, kem_pubkey, sig_pubkey, serverTs, username)
      .run();

    if (!res.meta || res.meta.changes === 0) {
      return c.json({error: 'Username already taken'}, 409);
    }

    return c.json({message: 'Registered successfully'}, 201);
  } catch (e: any) {
    if (e.message && e.message.includes('UNIQUE constraint failed')) {
      return c.json({error: 'Username already taken'}, 409);
    }
    return c.json({error: 'Database error'}, 500);
  }
});

async function getPubkeyForUser(userString: string, c: any) {
  const localPart = userString.includes('@')
    ? userString.split('@')[0]
    : userString;
  const domainPart = userString.includes('@')
    ? userString.split('@')[1]
    : c.env.INSTANCE_DOMAIN;

  if (domainPart === c.env.INSTANCE_DOMAIN) {
    return await c.env.DB.prepare(
      'SELECT kem_pubkey, sig_pubkey FROM users WHERE username = ?',
    )
      .bind(localPart)
      .first();
  } else {
    if (
      c.env.FEDERATION_ENABLED !== 'true' &&
      c.env.FEDERATION_ENABLED !== true
    ) {
      return null;
    }
    try {
      const resp = await fetch(`https://${domainPart}/api/pubkey/${localPart}`);
      if (!resp.ok) return null;
      return await resp.json();
    } catch (e) {
      return null;
    }
  }
}

app.post('/api/mail', async c => {
  const rawBody = await c.req.text();

  const maxSize = parseInt(c.env.MAX_MAIL_SIZE || '196608', 10);
  if (rawBody.length > maxSize) {
    return c.json({error: 'Payload too large'}, 413);
  }

  let body;
  try {
    body = JSON.parse(rawBody);
  } catch (e) {
    return c.json({error: 'Invalid JSON'}, 400);
  }

  const {
    sender,
    recipient,
    recipient_kem_fingerprint,
    enc_key_sender,
    enc_key_recipient,
    enc_subject,
    enc_body,
    size,
    timestamp,
    signature,
    pow,
  } = body;

  if (
    !sender ||
    !recipient ||
    !recipient_kem_fingerprint ||
    !enc_key_sender ||
    !enc_key_recipient ||
    enc_subject === undefined ||
    !enc_body ||
    size === undefined ||
    !timestamp ||
    !signature ||
    !pow
  ) {
    return c.json({error: 'Missing required fields'}, 400);
  }

  const isPowValid = await verifyPoW(pow, sender);
  if (!isPowValid) {
    return c.json({error: 'Invalid Proof of Work'}, 403);
  }

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300) {
    return c.json({error: 'Timestamp out of window'}, 403);
  }

  const senderUser = await getPubkeyForUser(sender, c);

  if (!senderUser) {
    return c.json({error: 'Sender not registered'}, 401);
  }

  const recipientUser = await getPubkeyForUser(recipient, c);

  if (!recipientUser) {
    return c.json({error: 'Recipient not found'}, 404);
  }

  if (!recipientUser.kem_pubkey) {
    return c.json({error: 'USER_DESTROYED'}, 410);
  }

  // Convert standard base64 to Uint8Array safely
  const b64Str = (recipientUser.kem_pubkey as string)
    .replace(/-/g, '+')
    .replace(/_/g, '/');
  const kemBytes = Uint8Array.from(atob(b64Str), c => c.charCodeAt(0));

  const hashBuffer = await crypto.subtle.digest('SHA-256', kemBytes);
  const hashArray = Array.from(new Uint8Array(hashBuffer));
  const expectedFp = hashArray
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');

  if (recipient_kem_fingerprint !== expectedFp) {
    return c.json({error: 'KEY_MISMATCH'}, 409);
  }

  await initWasm({module_or_path: wasmModule});
  try {
    const signedData = {
      sender,
      recipient,
      recipient_kem_fingerprint,
      enc_subject,
      enc_body,
      timestamp,
      size,
    };
    const message = JSON.stringify(signedData);
    const msgBytes = new TextEncoder().encode(message);

    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(senderUser.sig_pubkey as string);

    const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
    if (!isSigValid) {
      return c.json({error: 'Invalid signature'}, 401);
    }
  } catch (e) {
    return c.json({error: 'Signature verification failed'}, 401);
  }

  /* Normalize both parties to the stored form before any lookup.
   * Relayed mail arrives fully qualified ("bob@our.domain"), so an
   * un-normalized blocker never matched the bare name the blocklist
   * holds - precisely the federated case blocking is for. */
  const dbSender = normalizeAddress(sender, c.env.INSTANCE_DOMAIN);
  const dbRecipient = normalizeAddress(recipient, c.env.INSTANCE_DOMAIN);
  const senderDomain = addressDomain(sender, c.env.INSTANCE_DOMAIN);

  /* check if sender is blocked by recipient: the sender's full
   * address as stored, or their whole domain */
  const blockCheck = await c.env.DB.prepare(
    'SELECT 1 FROM blocks WHERE blocker = ? AND ' +
      '(blocked = ? OR blocked = ?)',
  )
    .bind(dbRecipient, dbSender, senderDomain)
    .first();
  if (blockCheck) {
    return c.json({error: 'Recipient has blocked this sender'}, 403);
  }

  const charset =
    '123456789ABCDEFGHJKLMNPQRSTUVWXYZ' + 'abcdefghijkmnopqrstuvwxyz';
  let mail_id = '';
  for (let i = 0; i < 10; i++) {
    mail_id += charset.charAt(Math.floor(Math.random() * charset.length));
  }

  /* relay before storing the sender's copy: a failed relay leaves
   * nothing behind, and the client keeps the mail as a draft */
  const recipientDomain = addressDomain(recipient, c.env.INSTANCE_DOMAIN);
  if (recipientDomain !== c.env.INSTANCE_DOMAIN.toLowerCase()) {
    const refused = await relayMail(recipientDomain, rawBody);
    if (refused) {
      return c.json({error: refused.error}, refused.status);
    }
  }

  try {
    await c.env.DB.prepare(
      'INSERT INTO mail (mail_id, sender, recipient, ' +
        'enc_key_sender, enc_key_recipient, enc_subject, ' +
        'enc_body, size, signature, timestamp) ' +
        'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
    )
      .bind(
        mail_id,
        dbSender,
        dbRecipient,
        enc_key_sender,
        enc_key_recipient,
        enc_subject,
        enc_body,
        size,
        signature,
        serverTs,
      )
      .run();

    return c.json({message: 'Mail sent', id: mail_id}, 201);
  } catch (e) {
    return c.json({error: 'Database error'}, 500);
  }
});

app.get('/api/mail', async c => {
  const username = c.req.header('X-Shyake-Username');
  const timestamp = c.req.header('X-Shyake-Timestamp');
  const signature = c.req.header('X-Shyake-Signature');
  const pow = c.req.header('X-Shyake-Pow');
  const type = c.req.query('type') || 'inbox';

  if (!username || !timestamp || !signature || !pow) {
    return c.json({error: 'Missing auth headers'}, 401);
  }

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) {
    return c.json({error: 'Invalid Proof of Work'}, 403);
  }

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300) {
    return c.json({error: 'Timestamp out of window'}, 403);
  }

  const user = await c.env.DB.prepare(
    'SELECT sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();

  if (!user) {
    return c.json({error: 'User not found'}, 401);
  }

  await initWasm({module_or_path: wasmModule});
  try {
    const message = `GET:/api/mail?type=${type}:${username}:${timestamp}`;
    const msgBytes = new TextEncoder().encode(message);

    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(user.sig_pubkey as string);

    const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
    if (!isSigValid) {
      return c.json({error: 'Invalid signature'}, 401);
    }
  } catch (e) {
    console.error('Signature verification error:', e);
    return c.json({error: 'Signature verification failed'}, 401);
  }

  try {
    const column = type === 'sent' ? 'sender' : 'recipient';
    const {results} = await c.env.DB.prepare(
      `SELECT mail_id, sender, recipient, enc_key_sender, ` +
        `enc_key_recipient, enc_subject, size, timestamp ` +
        `FROM mail WHERE ${column} = ? ORDER BY timestamp DESC`,
    )
      .bind(username)
      .all();

    return c.json({mail: results}, 200);
  } catch (e) {
    return c.json({error: 'Database error'}, 500);
  }
});

app.get('/api/mail/:id', async c => {
  const id = c.req.param('id');
  const username = c.req.header('X-Shyake-Username');
  const timestamp = c.req.header('X-Shyake-Timestamp');
  const signature = c.req.header('X-Shyake-Signature');
  const pow = c.req.header('X-Shyake-Pow');

  if (!username || !timestamp || !signature || !pow) {
    return c.json({error: 'Missing auth headers'}, 401);
  }

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) {
    return c.json({error: 'Invalid Proof of Work'}, 403);
  }

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300) {
    return c.json({error: 'Timestamp out of window'}, 403);
  }

  const user = await c.env.DB.prepare(
    'SELECT sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();

  if (!user) {
    return c.json({error: 'User not found'}, 401);
  }

  await initWasm({module_or_path: wasmModule});
  try {
    const message = `GET:/api/mail/${id}:${username}:${timestamp}`;
    const msgBytes = new TextEncoder().encode(message);

    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(user.sig_pubkey as string);

    const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
    if (!isSigValid) {
      return c.json({error: 'Invalid signature'}, 401);
    }
  } catch (e) {
    return c.json({error: 'Signature verification failed'}, 401);
  }

  try {
    const mail = await c.env.DB.prepare(
      `SELECT * FROM mail WHERE mail_id = ? AND ` +
        `(sender = ? OR recipient = ?)`,
    )
      .bind(id, username, username)
      .first();

    if (!mail) {
      return c.json({error: 'Mail not found'}, 404);
    }

    return c.json(mail, 200);
  } catch (e) {
    return c.json({error: 'Database error'}, 500);
  }
});

app.get('/api/pubkey/:username', async c => {
  let username = c.req.param('username');

  if (username.includes('@')) {
    const [localUser, domain] = username.split('@');
    if (domain !== c.env.INSTANCE_DOMAIN) {
      if (
        c.env.FEDERATION_ENABLED !== 'true' &&
        c.env.FEDERATION_ENABLED !== true
      ) {
        return c.json({error: 'Federation disabled'}, 403);
      }
      try {
        const resp = await fetch(`https://${domain}/api/pubkey/${localUser}`);
        if (!resp.ok) {
          return c.json({error: 'External user not found'}, resp.status);
        }
        const data = await resp.json();
        return c.json(data, 200);
      } catch (e) {
        return c.json({error: 'Failed to connect'}, 502);
      }
    }
    username = localUser;
  }

  const user = await c.env.DB.prepare(
    'SELECT kem_pubkey, sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();

  if (!user) {
    return c.json({error: 'User not found'}, 404);
  }
  return c.json(user);
});

/* burn: DELETE /api/mail/:id */
app.delete('/api/mail/:id', async c => {
  const id = c.req.param('id');
  const username = c.req.header('X-Shyake-Username');
  const timestamp = c.req.header('X-Shyake-Timestamp');
  const signature = c.req.header('X-Shyake-Signature');
  const pow = c.req.header('X-Shyake-Pow');

  if (!username || !timestamp || !signature || !pow)
    return c.json({error: 'Missing auth headers'}, 401);

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) return c.json({error: 'Invalid Proof of Work'}, 403);

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300)
    return c.json({error: 'Timestamp out of window'}, 403);

  const user = await c.env.DB.prepare(
    'SELECT sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();
  if (!user) return c.json({error: 'User not found'}, 401);

  await initWasm({module_or_path: wasmModule});
  try {
    const message = `DELETE:/api/mail/${id}:${username}:${timestamp}`;
    const msgBytes = new TextEncoder().encode(message);
    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(user.sig_pubkey as string);
    if (!verifySignature(pkUrl, msgBytes, sigUrl))
      return c.json({error: 'Invalid signature'}, 401);
  } catch (e) {
    return c.json({error: 'Signature verification failed'}, 401);
  }

  const mail = await c.env.DB.prepare(
    'SELECT mail_id FROM mail WHERE mail_id = ? AND ' +
      '(sender = ? OR recipient = ?)',
  )
    .bind(id, username, username)
    .first();
  if (!mail) return c.json({error: 'Mail not found'}, 404);

  await c.env.DB.prepare('DELETE FROM mail WHERE mail_id = ?').bind(id).run();
  return c.json({message: 'Mail burned'}, 200);
});

/* block/unblock: POST /api/block and DELETE /api/block */
async function handleBlock(c: any, unblock: boolean): Promise<Response> {
  const raw = await readSignedBody(c.req.raw);
  if (!raw) return c.json({error: 'Payload too large'}, 413);

  const username = c.req.header('X-Shyake-Username');
  const timestamp = c.req.header('X-Shyake-Timestamp');
  const signature = c.req.header('X-Shyake-Signature');
  const pow = c.req.header('X-Shyake-Pow');

  if (!username || !timestamp || !signature || !pow)
    return c.json({error: 'Missing auth headers'}, 401);

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) return c.json({error: 'Invalid Proof of Work'}, 403);

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300)
    return c.json({error: 'Timestamp out of window'}, 403);

  const user = await c.env.DB.prepare(
    'SELECT sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();
  if (!user) return c.json({error: 'User not found'}, 401);

  await initWasm({module_or_path: wasmModule});
  const method = unblock ? 'DELETE' : 'POST';
  try {
    const message = `${method}:/api/block:${username}:${timestamp}:${raw.digest}`;
    const msgBytes = new TextEncoder().encode(message);
    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(user.sig_pubkey as string);
    if (!verifySignature(pkUrl, msgBytes, sigUrl))
      return c.json({error: 'Invalid signature'}, 401);
  } catch (e) {
    return c.json({error: 'Signature verification failed'}, 401);
  }

  let body;
  try {
    body = JSON.parse(raw.text);
  } catch (e) {
    return c.json({error: 'Invalid JSON'}, 400);
  }
  const {target} = body;
  if (!target) return c.json({error: 'Missing target'}, 400);

  /* Store the target in the same form the mail path looks up, so
   * "bob@this.instance", "Bob@Remote.Example" and "Remote.Example"
   * all match what arrives on the wire. */
  const normTarget = normalizeAddress(target, c.env.INSTANCE_DOMAIN);

  if (unblock) {
    /* also accept the raw string, to clear rows stored before
     * normalization existed */
    await c.env.DB.prepare(
      'DELETE FROM blocks WHERE blocker = ? AND blocked IN (?, ?)',
    )
      .bind(username, normTarget, target)
      .run();
    return c.json({message: 'Unblocked'}, 200);
  } else {
    const ts = Math.floor(Date.now() / 1000);
    await c.env.DB.prepare(
      'INSERT OR REPLACE INTO blocks (blocker, blocked, created_at)' +
        ' VALUES (?, ?, ?)',
    )
      .bind(username, normTarget, ts)
      .run();
    return c.json({message: 'Blocked'}, 201);
  }
}

app.post('/api/block', c => handleBlock(c, false));
app.delete('/api/block', c => handleBlock(c, true));

/* blocklist: GET /api/block returns the caller's blocks */
app.get('/api/block', async c => {
  const username = c.req.header('X-Shyake-Username');
  const timestamp = c.req.header('X-Shyake-Timestamp');
  const signature = c.req.header('X-Shyake-Signature');
  const pow = c.req.header('X-Shyake-Pow');

  if (!username || !timestamp || !signature || !pow)
    return c.json({error: 'Missing auth headers'}, 401);

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) return c.json({error: 'Invalid Proof of Work'}, 403);

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300)
    return c.json({error: 'Timestamp out of window'}, 403);

  const user = await c.env.DB.prepare(
    'SELECT sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();
  if (!user) return c.json({error: 'User not found'}, 401);

  await initWasm({module_or_path: wasmModule});
  try {
    const message = `GET:/api/block:${username}:${timestamp}`;
    const msgBytes = new TextEncoder().encode(message);
    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(user.sig_pubkey as string);
    if (!verifySignature(pkUrl, msgBytes, sigUrl))
      return c.json({error: 'Invalid signature'}, 401);
  } catch (e) {
    return c.json({error: 'Signature verification failed'}, 401);
  }

  try {
    const {results} = await c.env.DB.prepare(
      'SELECT blocked, created_at FROM blocks WHERE blocker = ? ' +
        'ORDER BY created_at DESC',
    )
      .bind(username)
      .all();
    return c.json({blocks: results}, 200);
  } catch (e) {
    return c.json({error: 'Database error'}, 500);
  }
});

app.post('/api/rotate', async c => {
  const raw = await readSignedBody(c.req.raw);
  if (!raw) {
    return c.json({error: 'Payload too large'}, 413);
  }

  const username = c.req.header('X-Shyake-Username');
  const timestamp = c.req.header('X-Shyake-Timestamp');
  const signature = c.req.header('X-Shyake-Signature');
  const pow = c.req.header('X-Shyake-Pow');

  if (!username || !timestamp || !signature || !pow) {
    return c.json({error: 'Missing auth headers'}, 401);
  }

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) {
    return c.json({error: 'Invalid Proof of Work'}, 403);
  }

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300) {
    return c.json({error: 'Timestamp out of window'}, 403);
  }

  const user = await c.env.DB.prepare(
    'SELECT sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();

  if (!user || !user.sig_pubkey) {
    return c.json({error: 'User not found or destroyed'}, 401);
  }

  await initWasm({module_or_path: wasmModule});
  try {
    const message = `POST:/api/rotate:${username}:${timestamp}:${raw.digest}`;
    const msgBytes = new TextEncoder().encode(message);
    const toBase64Url = (b64: string) =>
      b64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '');
    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(user.sig_pubkey as string);
    const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
    if (!isSigValid) {
      return c.json({error: 'Invalid signature'}, 401);
    }
  } catch (e) {
    return c.json({error: 'Signature verification failed'}, 401);
  }

  let body;
  try {
    body = JSON.parse(raw.text);
  } catch (e) {
    return c.json({error: 'Invalid JSON'}, 400);
  }

  const {new_kem_pubkey, new_sig_pubkey} = body;
  if (!new_kem_pubkey || !new_sig_pubkey) {
    return c.json({error: 'Missing new keys'}, 400);
  }

  try {
    await c.env.DB.prepare(
      'UPDATE users SET kem_pubkey = ?, sig_pubkey = ? ' + 'WHERE username = ?',
    )
      .bind(new_kem_pubkey, new_sig_pubkey, username)
      .run();

    await c.env.DB.prepare('DELETE FROM mail WHERE sender = ? OR recipient = ?')
      .bind(username, username)
      .run();

    return c.json({message: 'Keys rotated and old mails deleted'}, 200);
  } catch (e) {
    return c.json({error: 'Database error'}, 500);
  }
});

app.delete('/api/destroy', async c => {
  const username = c.req.header('X-Shyake-Username');
  const timestamp = c.req.header('X-Shyake-Timestamp');
  const signature = c.req.header('X-Shyake-Signature');
  const pow = c.req.header('X-Shyake-Pow');

  if (!username || !timestamp || !signature || !pow) {
    return c.json({error: 'Missing auth headers'}, 401);
  }

  const isPowValid = await verifyPoW(pow, username);
  if (!isPowValid) {
    return c.json({error: 'Invalid Proof of Work'}, 403);
  }

  const clientTs = parseInt(timestamp, 10);
  const serverTs = Math.floor(Date.now() / 1000);
  if (Math.abs(serverTs - clientTs) > 300) {
    return c.json({error: 'Timestamp out of window'}, 403);
  }

  const user = await c.env.DB.prepare(
    'SELECT sig_pubkey FROM users WHERE username = ?',
  )
    .bind(username)
    .first();

  if (!user || !user.sig_pubkey) {
    return c.json({error: 'User not found or already destroyed'}, 401);
  }

  await initWasm({module_or_path: wasmModule});
  try {
    const message = `DELETE:/api/destroy:${username}:${timestamp}`;
    const msgBytes = new TextEncoder().encode(message);
    const toBase64Url = (b64: string) =>
      b64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '');
    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(user.sig_pubkey as string);
    const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
    if (!isSigValid) {
      return c.json({error: 'Invalid signature'}, 401);
    }
  } catch (e) {
    return c.json({error: 'Signature verification failed'}, 401);
  }

  try {
    await c.env.DB.prepare(
      "UPDATE users SET kem_pubkey = '', sig_pubkey = '' " +
        'WHERE username = ?',
    )
      .bind(username)
      .run();

    await c.env.DB.prepare('DELETE FROM mail WHERE sender = ? OR recipient = ?')
      .bind(username, username)
      .run();

    await c.env.DB.prepare(
      'DELETE FROM blocks WHERE blocker = ? OR blocked = ?',
    )
      .bind(username, username)
      .run();

    return c.json({message: 'Account destroyed'}, 200);
  } catch (e) {
    return c.json({error: 'Database error'}, 500);
  }
});

export default app;
