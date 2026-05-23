import { Hono } from 'hono';
import { isValidUsername, isReservedUsername, verifyPoW } from './utils';

// Import the Web build which allows manual instantiation
import initWasm, { verify as verifySignature } from
    'mldsa65-wasm/web/mldsa65.js';
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
    INSTANCE_DOMAIN: string;
    REGISTRATION_ENABLED: string;
    RESERVED_USERNAMES: string;
    FEDERATION_ENABLED: string;
    MAX_MAIL_SIZE: string;
}

const app = new Hono<{ Bindings: Env }>();

app.post('/api/register', async (c) => {
    const regEnabled = String(c.env.REGISTRATION_ENABLED) === 'true';
    if (!regEnabled) {
        return c.json({ error: 'Registration is disabled' }, 403);
    }

    const body = await c.req.json();
    const { username,
            kem_pubkey,
            sig_pubkey,
            timestamp,
            signature,
            pow } = body;

    if (!username || !kem_pubkey || !sig_pubkey || !timestamp ||
        !signature || !pow) {
        return c.json({ error: 'Missing required fields' }, 400);
    }

    if (!isValidUsername(username)) {
        return c.json({ error: 'Invalid username format' }, 400);
    }
    if (isReservedUsername(username, c.env.RESERVED_USERNAMES)) {
        return c.json({ error: 'Username is reserved' }, 403);
    }

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid) {
        return c.json({ error: 'Invalid Proof of Work' }, 403);
    }
// 3. Verify Timestamp (anti-replay)
const clientTs = parseInt(timestamp, 10);
const serverTs = Math.floor(Date.now() / 1000);
if (Math.abs(serverTs - clientTs) > 300) {
    return c.json({ error: 'Timestamp out of window' }, 403);
}

// Initialize the WASM module for Cloudflare Workers
await initWasm({ module_or_path: wasmModule });

// 4. Verify Signature
try {
    // Reconstruct the signed payload
    const signedData = { username, kem_pubkey, sig_pubkey, timestamp };
    const message = JSON.stringify(signedData);

    const msgBytes = new TextEncoder().encode(message);


    const sigUrl = toBase64Url(signature);
    const pkUrl = toBase64Url(sig_pubkey);

    // verify(vk, message, signature, context)
    const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
    if (!isSigValid) {
        return c.json({ error: 'Invalid signature' }, 401);
    }
} catch (e) {
    console.error('Signature verification error:', e);
    return c.json({ error: 'Signature verification failed' }, 401);
}

    try {
        await c.env.DB.prepare(
            'INSERT INTO users (username, kem_pubkey, sig_pubkey, ' +
            'created_at) VALUES (?, ?, ?, ?)'
        )
            .bind(username, kem_pubkey, sig_pubkey, serverTs)
            .run();

        return c.json({ message: 'Registered successfully' }, 201);
    } catch (e: any) {
        if (e.message && e.message.includes('UNIQUE constraint failed')) {
            return c.json({ error: 'Username already taken' }, 409);
        }
        return c.json({ error: 'Database error' }, 500);
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
            'SELECT kem_pubkey, sig_pubkey FROM users WHERE username = ?'
        ).bind(localPart).first();
    } else {
        if (c.env.FEDERATION_ENABLED !== 'true' &&
            c.env.FEDERATION_ENABLED !== true) {
            return null;
        }
        try {
            const resp = await fetch(
                `https://${domainPart}/api/pubkey/${localPart}`
            );
            if (!resp.ok) return null;
            return await resp.json();
        } catch (e) {
            return null;
        }
    }
}

app.post('/api/mail', async (c) => {
    const rawBody = await c.req.text();
    
    const maxSize = parseInt(c.env.MAX_MAIL_SIZE || '196608', 10);
    if (rawBody.length > maxSize) {
        return c.json({ error: 'Payload too large' }, 413);
    }

    let body;
    try {
        body = JSON.parse(rawBody);
    } catch (e) {
        return c.json({ error: 'Invalid JSON' }, 400);
    }

    const {
        sender, recipient, recipient_kem_fingerprint,
        enc_key_sender, enc_key_recipient, enc_subject, enc_body,
        size, timestamp, signature, pow
    } = body;

    if (!sender || !recipient || !recipient_kem_fingerprint ||
        !enc_key_sender || !enc_key_recipient || 
        enc_subject === undefined || !enc_body || 
        size === undefined || !timestamp || !signature || !pow) {
        return c.json({ error: 'Missing required fields' }, 400);
    }

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid) {
        return c.json({ error: 'Invalid Proof of Work' }, 403);
    }

    const clientTs = parseInt(timestamp, 10);
    const serverTs = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTs - clientTs) > 300) {
        return c.json({ error: 'Timestamp out of window' }, 403);
    }

    const senderUser = await getPubkeyForUser(sender, c);
    
    if (!senderUser) {
        return c.json({ error: 'Sender not registered' }, 401);
    }

    const recipientUser = await getPubkeyForUser(recipient, c);

    if (!recipientUser) {
        return c.json({ error: 'Recipient not found' }, 404);
    }

    if (!recipientUser.kem_pubkey) {
        return c.json({ error: 'USER_DESTROYED' }, 410);
    }

    // Convert standard base64 to Uint8Array safely
    const b64Str = (recipientUser.kem_pubkey as string)
        .replace(/-/g, '+').replace(/_/g, '/');
    const kemBytes = Uint8Array.from(atob(b64Str), c => c.charCodeAt(0));

    const hashBuffer = await crypto.subtle.digest('SHA-256', kemBytes);
    const hashArray = Array.from(new Uint8Array(hashBuffer));
    const expectedFp = hashArray.map(
        b => b.toString(16).padStart(2, '0')
    ).join('');

    if (recipient_kem_fingerprint !== expectedFp) {
        return c.json({ error: 'KEY_MISMATCH' }, 409);
    }

    await initWasm({ module_or_path: wasmModule });
    try {
        const signedData = {
            sender, recipient, recipient_kem_fingerprint,
            enc_subject, enc_body, timestamp, size
        };
        const message = JSON.stringify(signedData);
        const msgBytes = new TextEncoder().encode(message);


        const sigUrl = toBase64Url(signature);
        const pkUrl = toBase64Url(senderUser.sig_pubkey as string);

        const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
        if (!isSigValid) {
            return c.json({ error: 'Invalid signature' }, 401);
        }
    } catch (e) {
        return c.json({ error: 'Signature verification failed' }, 401);
    }

    /* check if sender is blocked by recipient */
    const senderLocal = sender.includes('@')
        ? sender.split('@')[0] : sender;
    const senderDomain = sender.includes('@')
        ? sender.split('@')[1] : c.env.INSTANCE_DOMAIN;
    const blockCheck = await c.env.DB.prepare(
        'SELECT 1 FROM blocks WHERE blocker = ? AND ' +
        '(blocked = ? OR blocked = ?)'
    ).bind(recipient, senderLocal, senderDomain).first();
    if (blockCheck) {
        return c.json({ error: 'Recipient has blocked this sender' }, 403);
    }

    const charset = '123456789ABCDEFGHJKLMNPQRSTUVWXYZ' +
                    'abcdefghijkmnopqrstuvwxyz';
    let mail_id = '';
    for (let i = 0; i < 10; i++) {
        mail_id += charset.charAt(
            Math.floor(Math.random() * charset.length)
        );
    }

    const suffix = `@${c.env.INSTANCE_DOMAIN}`;
    const dbSender = sender.endsWith(suffix)
        ? sender.slice(0, -suffix.length)
        : sender;
    const dbRecipient = recipient.endsWith(suffix)
        ? recipient.slice(0, -suffix.length)
        : recipient;

    try {
        await c.env.DB.prepare(
            'INSERT INTO mail (mail_id, sender, recipient, ' +
            'enc_key_sender, enc_key_recipient, enc_subject, ' +
            'enc_body, size, signature, timestamp) ' +
            'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)'
        ).bind(
            mail_id, dbSender, dbRecipient, enc_key_sender,
            enc_key_recipient, enc_subject, enc_body, size, 
            signature, serverTs
        ).run();

        const recipientDomain = recipient.includes('@')
            ? recipient.split('@')[1]
            : c.env.INSTANCE_DOMAIN;
        if (recipientDomain !== c.env.INSTANCE_DOMAIN) {
            const forwardReq = fetch(
                `https://${recipientDomain}/api/mail`,
                {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: rawBody
                }
            ).catch(e => console.error('Failed to forward mail:', e));
            c.executionCtx.waitUntil(forwardReq);
        }

        return c.json({ message: 'Mail sent', id: mail_id }, 201);
    } catch (e) {
        return c.json({ error: 'Database error' }, 500);
    }
});

app.get('/api/mail', async (c) => {
    const username = c.req.header('X-Shyake-Username');
    const timestamp = c.req.header('X-Shyake-Timestamp');
    const signature = c.req.header('X-Shyake-Signature');
    const pow = c.req.header('X-Shyake-Pow');
    const type = c.req.query('type') || 'inbox';

    if (!username || !timestamp || !signature || !pow) {
        return c.json({ error: 'Missing auth headers' }, 401);
    }

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid) {
        return c.json({ error: 'Invalid Proof of Work' }, 403);
    }

    const clientTs = parseInt(timestamp, 10);
    const serverTs = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTs - clientTs) > 300) {
        return c.json({ error: 'Timestamp out of window' }, 403);
    }

    const user = await c.env.DB.prepare(
        'SELECT sig_pubkey FROM users WHERE username = ?'
    ).bind(username).first();

    if (!user) {
        return c.json({ error: 'User not found' }, 401);
    }

    await initWasm({ module_or_path: wasmModule });
    try {
        const message =
            `GET:/api/mail?type=${type}:${username}:${timestamp}`;
        const msgBytes = new TextEncoder().encode(message);

        const sigUrl = toBase64Url(signature);
        const pkUrl = toBase64Url(user.sig_pubkey as string);

        const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
        if (!isSigValid) {
            return c.json({ error: 'Invalid signature' }, 401);
        }
    } catch (e) {
        console.error('Signature verification error:', e);
        return c.json({ error: 'Signature verification failed' }, 401);
    }

    try {
        const column = type === 'sent' ? 'sender' : 'recipient';
        const { results } = await c.env.DB.prepare(
            `SELECT mail_id, sender, recipient, enc_key_sender, ` +
            `enc_key_recipient, enc_subject, size, timestamp ` +
            `FROM mail WHERE ${column} = ? ORDER BY timestamp DESC`
        ).bind(username).all();

        return c.json({ mail: results }, 200);
    } catch (e) {
        return c.json({ error: 'Database error' }, 500);
    }
});

app.get('/api/mail/:id', async (c) => {
    const id = c.req.param('id');
    const username = c.req.header('X-Shyake-Username');
    const timestamp = c.req.header('X-Shyake-Timestamp');
    const signature = c.req.header('X-Shyake-Signature');
    const pow = c.req.header('X-Shyake-Pow');

    if (!username || !timestamp || !signature || !pow) {
        return c.json({ error: 'Missing auth headers' }, 401);
    }

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid) {
        return c.json({ error: 'Invalid Proof of Work' }, 403);
    }

    const clientTs = parseInt(timestamp, 10);
    const serverTs = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTs - clientTs) > 300) {
        return c.json({ error: 'Timestamp out of window' }, 403);
    }

    const user = await c.env.DB.prepare(
        'SELECT sig_pubkey FROM users WHERE username = ?'
    ).bind(username).first();

    if (!user) {
        return c.json({ error: 'User not found' }, 401);
    }

    await initWasm({ module_or_path: wasmModule });
    try {
        const message = `GET:/api/mail/${id}:${username}:${timestamp}`;
        const msgBytes = new TextEncoder().encode(message);


        const sigUrl = toBase64Url(signature);
        const pkUrl = toBase64Url(user.sig_pubkey as string);

        const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
        if (!isSigValid) {
            return c.json({ error: 'Invalid signature' }, 401);
        }
    } catch (e) {
        return c.json({ error: 'Signature verification failed' }, 401);
    }

    try {
        const mail = await c.env.DB.prepare(
            `SELECT * FROM mail WHERE mail_id = ? AND ` +
            `(sender = ? OR recipient = ?)`
        ).bind(id, username, username).first();

        if (!mail) {
            return c.json({ error: 'Mail not found' }, 404);
        }

        return c.json(mail, 200);
    } catch (e) {
        return c.json({ error: 'Database error' }, 500);
    }
});

app.get('/api/pubkey/:username', async (c) => {
    let username = c.req.param('username');

    if (username.includes('@')) {
        const [localUser, domain] = username.split('@');
        if (domain !== c.env.INSTANCE_DOMAIN) {
            if (c.env.FEDERATION_ENABLED !== 'true' &&
            c.env.FEDERATION_ENABLED !== true) {
                return c.json(
                    { error: 'Federation disabled' },
                    403
                );
            }
            try {
                const resp = await fetch(
                    `https://${domain}/api/pubkey/${localUser}`
                );
                if (!resp.ok) {
                    return c.json(
                        { error: 'External user not found' },
                        resp.status
                    );
                }
                const data = await resp.json();
                return c.json(data, 200);
            } catch (e) {
                return c.json(
                    { error: 'Failed to connect' },
                    502
                );
            }
        }
        username = localUser;
    }

    const user = await c.env.DB.prepare(
        'SELECT kem_pubkey, sig_pubkey FROM users WHERE username = ?'
    )
        .bind(username)
        .first();

    if (!user) {
        return c.json({ error: 'User not found' }, 404);
    }
    return c.json(user);
});

/* burn: DELETE /api/mail/:id */
app.delete('/api/mail/:id', async (c) => {
    const id = c.req.param('id');
    const username = c.req.header('X-Shyake-Username');
    const timestamp = c.req.header('X-Shyake-Timestamp');
    const signature = c.req.header('X-Shyake-Signature');
    const pow = c.req.header('X-Shyake-Pow');

    if (!username || !timestamp || !signature || !pow)
        return c.json({ error: 'Missing auth headers' }, 401);

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid)
        return c.json({ error: 'Invalid Proof of Work' }, 403);

    const clientTs = parseInt(timestamp, 10);
    const serverTs = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTs - clientTs) > 300)
        return c.json({ error: 'Timestamp out of window' }, 403);

    const user = await c.env.DB.prepare(
        'SELECT sig_pubkey FROM users WHERE username = ?'
    ).bind(username).first();
    if (!user) return c.json({ error: 'User not found' }, 401);

    await initWasm({ module_or_path: wasmModule });
    try {
        const message =
            `DELETE:/api/mail/${id}:${username}:${timestamp}`;
        const msgBytes = new TextEncoder().encode(message);
        const sigUrl = toBase64Url(signature);
        const pkUrl = toBase64Url(user.sig_pubkey as string);
        if (!verifySignature(pkUrl, msgBytes, sigUrl))
            return c.json({ error: 'Invalid signature' }, 401);
    } catch (e) {
        return c.json({ error: 'Signature verification failed' }, 401);
    }

    const mail = await c.env.DB.prepare(
        'SELECT mail_id FROM mail WHERE mail_id = ? AND ' +
        '(sender = ? OR recipient = ?)'
    ).bind(id, username, username).first();
    if (!mail) return c.json({ error: 'Mail not found' }, 404);

    await c.env.DB.prepare(
        'DELETE FROM mail WHERE mail_id = ?'
    ).bind(id).run();
    return c.json({ message: 'Mail burned' }, 200);
});

/* block/unblock: POST /api/block and DELETE /api/block */
async function handleBlock(
    c: any, unblock: boolean
): Promise<Response> {
    const username = c.req.header('X-Shyake-Username');
    const timestamp = c.req.header('X-Shyake-Timestamp');
    const signature = c.req.header('X-Shyake-Signature');
    const pow = c.req.header('X-Shyake-Pow');

    if (!username || !timestamp || !signature || !pow)
        return c.json({ error: 'Missing auth headers' }, 401);

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid)
        return c.json({ error: 'Invalid Proof of Work' }, 403);

    const clientTs = parseInt(timestamp, 10);
    const serverTs = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTs - clientTs) > 300)
        return c.json({ error: 'Timestamp out of window' }, 403);

    const user = await c.env.DB.prepare(
        'SELECT sig_pubkey FROM users WHERE username = ?'
    ).bind(username).first();
    if (!user) return c.json({ error: 'User not found' }, 401);

    await initWasm({ module_or_path: wasmModule });
    const method = unblock ? 'DELETE' : 'POST';
    try {
        const message =
            `${method}:/api/block:${username}:${timestamp}`;
        const msgBytes = new TextEncoder().encode(message);
        const sigUrl = toBase64Url(signature);
        const pkUrl = toBase64Url(user.sig_pubkey as string);
        if (!verifySignature(pkUrl, msgBytes, sigUrl))
            return c.json({ error: 'Invalid signature' }, 401);
    } catch (e) {
        return c.json({ error: 'Signature verification failed' }, 401);
    }

    const body = await c.req.json();
    const { target } = body;
    if (!target)
        return c.json({ error: 'Missing target' }, 400);

    if (unblock) {
        await c.env.DB.prepare(
            'DELETE FROM blocks WHERE blocker = ? AND blocked = ?'
        ).bind(username, target).run();
        return c.json({ message: 'Unblocked' }, 200);
    } else {
        const ts = Math.floor(Date.now() / 1000);
        await c.env.DB.prepare(
            'INSERT OR REPLACE INTO blocks (blocker, blocked, created_at)' +
            ' VALUES (?, ?, ?)'
        ).bind(username, target, ts).run();
        return c.json({ message: 'Blocked' }, 201);
    }
}

app.post('/api/block', (c) => handleBlock(c, false));
app.delete('/api/block', (c) => handleBlock(c, true));

app.post('/api/rotate', async (c) => {
    const username = c.req.header('X-Shyake-Username');
    const timestamp = c.req.header('X-Shyake-Timestamp');
    const signature = c.req.header('X-Shyake-Signature');
    const pow = c.req.header('X-Shyake-Pow');

    if (!username || !timestamp || !signature || !pow) {
        return c.json({ error: 'Missing auth headers' }, 401);
    }

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid) {
        return c.json({ error: 'Invalid Proof of Work' }, 403);
    }

    const clientTs = parseInt(timestamp, 10);
    const serverTs = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTs - clientTs) > 300) {
        return c.json({ error: 'Timestamp out of window' }, 403);
    }

    const user = await c.env.DB.prepare(
        'SELECT sig_pubkey FROM users WHERE username = ?'
    ).bind(username).first();

    if (!user || !user.sig_pubkey) {
        return c.json(
            { error: 'User not found or destroyed' }, 401
        );
    }

    await initWasm({ module_or_path: wasmModule });
    try {
        const message = `POST:/api/rotate:${username}:${timestamp}`;
        const msgBytes = new TextEncoder().encode(message);
        const toBase64Url = (b64: string) =>
            b64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '');
        const sigUrl = toBase64Url(signature);
        const pkUrl = toBase64Url(user.sig_pubkey as string);
        const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
        if (!isSigValid) {
            return c.json({ error: 'Invalid signature' }, 401);
        }
    } catch (e) {
        return c.json({ error: 'Signature verification failed' }, 401);
    }

    let body;
    try {
        body = await c.req.json();
    } catch (e) {
        return c.json({ error: 'Invalid JSON' }, 400);
    }

    const { new_kem_pubkey, new_sig_pubkey } = body;
    if (!new_kem_pubkey || !new_sig_pubkey) {
        return c.json({ error: 'Missing new keys' }, 400);
    }

    try {
        await c.env.DB.prepare(
            'UPDATE users SET kem_pubkey = ?, sig_pubkey = ? ' +
            'WHERE username = ?'
        ).bind(new_kem_pubkey, new_sig_pubkey, username).run();

        await c.env.DB.prepare(
            'DELETE FROM mail WHERE sender = ? OR recipient = ?'
        ).bind(username, username).run();

        return c.json(
            { message: 'Keys rotated and old mails deleted' },
            200
        );
    } catch (e) {
        return c.json({ error: 'Database error' }, 500);
    }
});

app.delete('/api/destroy', async (c) => {
    const username = c.req.header('X-Shyake-Username');
    const timestamp = c.req.header('X-Shyake-Timestamp');
    const signature = c.req.header('X-Shyake-Signature');
    const pow = c.req.header('X-Shyake-Pow');

    if (!username || !timestamp || !signature || !pow) {
        return c.json({ error: 'Missing auth headers' }, 401);
    }

    const isPowValid = await verifyPoW(pow, 20);
    if (!isPowValid) {
        return c.json({ error: 'Invalid Proof of Work' }, 403);
    }

    const clientTs = parseInt(timestamp, 10);
    const serverTs = Math.floor(Date.now() / 1000);
    if (Math.abs(serverTs - clientTs) > 300) {
        return c.json({ error: 'Timestamp out of window' }, 403);
    }

    const user = await c.env.DB.prepare(
        'SELECT sig_pubkey FROM users WHERE username = ?'
    ).bind(username).first();

    if (!user || !user.sig_pubkey) {
        return c.json(
            { error: 'User not found or already destroyed' }, 401
        );
    }

    await initWasm({ module_or_path: wasmModule });
    try {
        const message = `DELETE:/api/destroy:${username}:${timestamp}`;
        const msgBytes = new TextEncoder().encode(message);
        const toBase64Url = (b64: string) =>
            b64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '');
        const sigUrl = toBase64Url(signature);
        const pkUrl = toBase64Url(user.sig_pubkey as string);
        const isSigValid = verifySignature(pkUrl, msgBytes, sigUrl);
        if (!isSigValid) {
            return c.json({ error: 'Invalid signature' }, 401);
        }
    } catch (e) {
        return c.json({ error: 'Signature verification failed' }, 401);
    }

    try {
        await c.env.DB.prepare(
            "UPDATE users SET kem_pubkey = '', sig_pubkey = '' " +
            "WHERE username = ?"
        ).bind(username).run();

        await c.env.DB.prepare(
            'DELETE FROM mail WHERE sender = ? OR recipient = ?'
        ).bind(username, username).run();

        await c.env.DB.prepare(
            'DELETE FROM blocks WHERE blocker = ? OR blocked = ?'
        ).bind(username, username).run();

        return c.json({ message: 'Account destroyed' }, 200);
    } catch (e) {
        return c.json({ error: 'Database error' }, 500);
    }
});

export default app;
