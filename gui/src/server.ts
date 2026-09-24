/**
 * Local HTTP API for the shyake GUI. Binds 127.0.0.1 only and guards
 * every /api route with a per-launch token injected into the page, so
 * other local processes and websites cannot drive the account.
 */

import { Session } from "./ffi";
import { existsSync, readFileSync, writeFileSync, mkdirSync, rmSync, readdirSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = fileURLToPath(new URL(".", import.meta.url));
const PUBLIC_DIR = resolve(HERE, "../public");
const PORT = Number(process.env.SHYAKE_GUI_PORT ?? 8788);
const TOKEN = crypto.randomUUID();

const CONFIG_DIR = process.env.SHYAKE_CONFIG ??
    join(process.env.HOME ?? ".", ".config", "shyake");

const DEFAULT_CONFIG = `# shyake global configuration file

INSTANCE=
USERNAME=

# Date & Time format (strftime format)
TIME_FORMAT="%Y-%m-%d %H:%M"

# Time zone (default: auto)
TIME_ZONE=auto

# Display columns for \`check\` command
CHECK_COLUMNS=id,sender,subject,size,date

# Default action when running without arguments
# 0 = man, 1 = check inbox, 2 = check inbox --count
DEFAULT_ACTION=0
`;

let session: Session | null = null;

function readConfig(): Record<string, string> {
    const out: Record<string, string> = {};
    try {
        for (const line of readFileSync(join(CONFIG_DIR, "config"), "utf8").split("\n")) {
            const t = line.trim();
            if (!t || t.startsWith("#")) continue;
            const eq = t.indexOf("=");
            if (eq < 0) continue;
            let v = t.slice(eq + 1).trim();
            if (v.length >= 2 && (v[0] === '"' || v[0] === "'") && v[0] === v[v.length - 1])
                v = v.slice(1, -1);
            out[t.slice(0, eq).trim()] = v;
        }
    } catch { /* no config yet */ }
    return out;
}

function writeConfigUserInstance(username: string, instance: string) {
    const p = join(CONFIG_DIR, "config");
    let text = existsSync(p) ? readFileSync(p, "utf8") : DEFAULT_CONFIG;
    text = text.split("\n").filter((l) => {
        const k = l.split("=")[0].trim();
        return k !== "INSTANCE" && k !== "USERNAME";
    }).join("\n");
    if (!text.endsWith("\n")) text += "\n";
    text += `INSTANCE=${instance}\nUSERNAME=${username}\n`;
    writeFileSync(p, text, { mode: 0o600 });
}

function keysExist(): boolean {
    return existsSync(join(CONFIG_DIR, "kem_pk.bin")) &&
        existsSync(join(CONFIG_DIR, "sig_pk.bin"));
}

function keyEncrypted(): boolean {
    try {
        const b = readFileSync(join(CONFIG_DIR, "kem_sk.bin"));
        return b.length >= 4 && b.subarray(0, 4).toString("latin1") === "SHYK";
    } catch {
        return false;
    }
}

function json(data: unknown, status = 200): Response {
    return Response.json(data, { status });
}

function err(e: unknown, status = 500): Response {
    if (e instanceof Error && "code" in e)
        return json({ error: e.message, code: (e as { code: number }).code }, status);
    if (e instanceof Error)
        return json({ error: e.message }, status);
    return json({ error: String(e) }, status);
}

function needSession(): Session {
    if (!session) throw Object.assign(new Error("locked"), { status: 401 });
    return session;
}

function newSession(): Session {
    const cfg = readConfig();
    return new Session(CONFIG_DIR, cfg.INSTANCE ?? "", cfg.USERNAME ?? "");
}

type Ctx = { req: Request; params: Record<string, string> };

const routes: [string, RegExp, (c: Ctx) => Promise<Response> | Response][] = [];

function route(method: string, pattern: string,
               handler: (c: Ctx) => Promise<Response> | Response) {
    const regex = new RegExp("^" + pattern.replace(/:(\w+)/g, "(?<$1>[^/]+)") + "$");
    routes.push([method, regex, handler]);
}

route("GET", "/api/status", () => {
    const cfg = readConfig();
    return json({
        initialized: keysExist(),
        registered: Boolean(cfg.USERNAME),
        unlocked: session !== null,
        keyEncrypted: keyEncrypted(),
        username: cfg.USERNAME ?? null,
        instance: cfg.INSTANCE ?? null,
        configDir: CONFIG_DIR,
    });
});

/* mirrors server-side validation (server/src/utils.ts) */
const USERNAME_RE = /^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$/;

route("POST", "/api/setup", async ({ req }) => {
    const { instance, username, passphrase } = await req.json();
    if (!instance || !username)
        return json({ error: "instance and username are required" }, 400);
    if (!USERNAME_RE.test(username))
        return json({
            error: "username must be 4-16 chars (letters, digits, underscore; at least one letter)",
        }, 400);
    try {
        mkdirSync(CONFIG_DIR, { recursive: true, mode: 0o700 });
        /* persist USERNAME only after the server accepts registration */
        writeConfigUserInstance("", instance);
        const s = new Session(CONFIG_DIR, instance, username);
        try {
            s.setPassphrase(passphrase || null);
            if (!keysExist())
                await s.generateKeys();
            await s.register(username);
        } catch (e) {
            s.close();
            throw e;
        }
        writeConfigUserInstance(username, instance);
        session?.close();
        session = s;
        return json({ ok: true });
    } catch (e) {
        /* SHYAKE_ERR_HTTP (-3): the instance rejected registration */
        const code = (e as { code?: number }).code;
        return err(e, code === -3 ? 400 : 500);
    }
});

route("POST", "/api/unlock", async ({ req }) => {
    const { passphrase } = await req.json().catch(() => ({}));
    try {
        const s = newSession();
        try {
            s.setPassphrase(passphrase || null);
            await s.verifyKey();
        } catch (e) {
            s.close();
            return json({ error: "unlock failed (wrong passphrase?)" }, 401);
        }
        session?.close();
        session = s;
        return json({ ok: true });
    } catch (e) {
        return err(e);
    }
});

route("POST", "/api/lock", () => {
    session?.close();
    session = null;
    return json({ ok: true });
});

route("GET", "/api/mail/:box", async ({ params }) => {
    try {
        const box = params.box;
        if (box === "inbox" || box === "sent")
            return json(await needSession().check(box));
        if (box === "saved")
            return json(await needSession().listSaved());
        return json({ error: "unknown box" }, 400);
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("GET", "/api/mail/detail/:id", async ({ req, params }) => {
    try {
        const saved = new URL(req.url).searchParams.get("src") === "saved";
        const s = needSession();
        return json(saved ? await s.readSaved(params.id) : await s.fetch(params.id));
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("POST", "/api/send", async ({ req }) => {
    try {
        const { to, subject, body } = await req.json();
        if (!to || !subject) return json({ error: "to and subject are required" }, 400);
        if (subject.length > 128) return json({ error: "subject exceeds 128 bytes" }, 400);
        await needSession().send(to, subject, body ?? "");
        return json({ ok: true });
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("POST", "/api/burn/:id", async ({ params }) => {
    try {
        await needSession().burn(params.id);
        return json({ ok: true });
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("POST", "/api/save/:id", async ({ params }) => {
    try {
        await needSession().saveMail(params.id);
        return json({ ok: true });
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("GET", "/api/blocklist", async () => {
    try {
        return json(await needSession().listBlocks());
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("POST", "/api/block", async ({ req }) => {
    try {
        const { target, unblock } = await req.json();
        if (!target) return json({ error: "target is required" }, 400);
        await needSession().block(target, Boolean(unblock));
        return json({ ok: true });
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("GET", "/api/fingerprint", async ({ req }) => {
    try {
        const user = new URL(req.url).searchParams.get("user");
        return json(await needSession().fingerprint(user || null, false));
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("POST", "/api/fingerprint/update", async ({ req }) => {
    try {
        const { user } = await req.json();
        if (!user) return json({ error: "user is required" }, 400);
        return json(await needSession().fingerprint(user, true));
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("POST", "/api/rotate", async ({ req }) => {
    try {
        const { newPassphrase } = await req.json().catch(() => ({}));
        const s = needSession();
        s.setNewPassphrase(newPassphrase || null);
        await s.rotate();
        return json({ ok: true });
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

route("POST", "/api/destroy", async ({ req }) => {
    try {
        const { confirm } = await req.json();
        const cfg = readConfig();
        if (confirm !== cfg.USERNAME)
            return json({ error: "confirmation does not match username" }, 400);
        await needSession().destroy();
        session?.close();
        session = null;
        for (const f of readdirSync(CONFIG_DIR))
            rmSync(join(CONFIG_DIR, f), { recursive: true, force: true });
        return json({ ok: true });
    } catch (e) {
        return err(e, (e as { status?: number }).status ?? 500);
    }
});

const MIME: Record<string, string> = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
};

async function serveStatic(pathname: string): Promise<Response> {
    const rel = pathname === "/" ? "index.html" : pathname.slice(1);
    if (rel.includes("..")) return new Response("bad path", { status: 400 });
    const file = Bun.file(join(PUBLIC_DIR, rel));
    if (!(await file.exists())) return new Response("not found", { status: 404 });
    let body: BodyInit = await file.arrayBuffer();
    if (rel === "index.html")
        body = new TextDecoder().decode(body as ArrayBuffer)
            .replace('"__SHYAKE_TOKEN__"', JSON.stringify(TOKEN));
    return new Response(body, {
        headers: { "content-type": MIME[rel.slice(rel.lastIndexOf("."))] ?? "application/octet-stream" },
    });
}

const server = Bun.serve({
    port: PORT,
    hostname: "127.0.0.1",
    async fetch(req) {
        const url = new URL(req.url);
        if (url.pathname.startsWith("/api/")) {
            if (req.headers.get("x-shyake-token") !== TOKEN)
                return json({ error: "forbidden" }, 403);
            for (const [method, regex, handler] of routes) {
                const m = regex.exec(url.pathname);
                if (method === req.method && m)
                    return handler({ req, params: m.groups ?? {} });
            }
            return json({ error: "not found" }, 404);
        }
        return serveStatic(url.pathname);
    },
});

const url = `http://127.0.0.1:${server.port}`;
console.log(`shyake GUI listening at ${url}`);
console.log(`config dir: ${CONFIG_DIR}`);
if (process.env.SHYAKE_GUI_NO_OPEN !== "1" && process.platform === "darwin")
    Bun.spawn(["open", url]);
