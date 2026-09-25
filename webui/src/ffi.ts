/**
 * bun:ffi bindings for libshyake (see client/include/shyake.h).
 *
 * Structs are mapped by hand to their LP64 layouts; only pointers
 * cross the boundary. Calls on a Session are serialized through a
 * promise chain because the library is not thread-safe.
 */

import { dlopen, FFIType, ptr, read, CString, suffix } from "bun:ffi";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = fileURLToPath(new URL(".", import.meta.url));
const LIB_PATH =
    process.env.SHYAKE_LIB ??
    resolve(HERE, "../../client/lib", `libshyake.${suffix}`);

export const SHYAKE_OK = 0;
export const SHYAKE_ERR = -1;
export const SHYAKE_ERR_NETWORK = -2;
export const SHYAKE_ERR_KEY_MISMATCH = -4;
export const SHYAKE_ERR_GONE = -5;
export const SHYAKE_ERR_NOT_FOUND = -6;
export const SHYAKE_ERR_FORBIDDEN = -7;
export const SHYAKE_ERR_CRYPTO = -8;
export const SHYAKE_ERR_NO_INSTANCE = -9;

const P = FFIType.ptr;
const I32 = FFIType.i32;

const lib = dlopen(LIB_PATH, {
    shyake_init_ctx: { args: [P], returns: P },
    shyake_free_ctx: { args: [P], returns: FFIType.void },
    shyake_set_passphrase: { args: [P, FFIType.cstring], returns: FFIType.void },
    shyake_set_new_passphrase: { args: [P, FFIType.cstring], returns: FFIType.void },
    shyake_last_error: { args: [P], returns: P },
    shyake_generate_keys: { args: [P], returns: I32 },
    shyake_register: { args: [P, FFIType.cstring], returns: I32 },
    shyake_send: { args: [P, FFIType.cstring, FFIType.cstring, P, FFIType.u64], returns: I32 },
    shyake_check: { args: [P, FFIType.cstring], returns: P },
    shyake_free_mail_list: { args: [P], returns: FFIType.void },
    shyake_fetch: { args: [P, FFIType.cstring], returns: P },
    shyake_free_mail_detail: { args: [P], returns: FFIType.void },
    shyake_burn: { args: [P, FFIType.cstring], returns: I32 },
    shyake_block: { args: [P, FFIType.cstring, I32], returns: I32 },
    shyake_list_blocks: { args: [P], returns: P },
    shyake_free_block_list: { args: [P], returns: FFIType.void },
    shyake_rotate: { args: [P], returns: I32 },
    shyake_destroy: { args: [P], returns: I32 },
    shyake_fingerprint: { args: [P, FFIType.cstring, I32], returns: P },
    shyake_free_fp_result: { args: [P], returns: FFIType.void },
    shyake_save_mail: { args: [P, FFIType.cstring], returns: I32 },
    shyake_read_saved: { args: [P, FFIType.cstring], returns: P },
    shyake_list_saved: { args: [P], returns: P },
    shyake_free_saved_list: { args: [P], returns: FFIType.void },
    shyake_selfdec_new: { args: [P], returns: P },
    shyake_selfdec_free: { args: [P], returns: FFIType.void },
});

function cstr(addr: number | null | undefined): string | null {
    if (!addr) return null;
    return new CString(addr).toString();
}

function i64(addr: number, off: number): number {
    return Number(read.i64(addr, off));
}

/* struct offsets (LP64) */
const MAIL_ENTRY_SIZE = 56;   /* 3 ptrs, i32, pad, 2 i64, i32, pad */
const SAVED_ENTRY_SIZE = 56;  /* 4 ptrs, 2 i64, i32, pad */
const BLOCK_ENTRY_SIZE = 16;  /* ptr, i64 */

export class ShyakeError extends Error {
    code: number;
    constructor(code: number, detail: string | null) {
        super(detail ?? `operation failed (code ${code})`);
        this.code = code;
    }
}

export interface MailEntryOut {
    mail_id: string | null;
    party: string | null;
    subject: string | null;
    size: number;
    timestamp: number;
    is_sent: boolean;
}

export interface MailDetailOut {
    mail_id: string | null;
    sender: string | null;
    recipient: string | null;
    subject: string | null;
    body: string | null;
    timestamp: number;
    size: number;
}

export class Session {
    private ctx: number;
    private queue: Promise<unknown> = Promise.resolve();

    constructor(
        public configDir: string,
        instanceUrl = "",
        username = "",
    ) {
        /* shyake_config: 3 cstring ptrs + 3 i32 = 40 bytes */
        const bufs = [
            Buffer.from(instanceUrl + "\0"),
            Buffer.from(configDir + "\0"),
            Buffer.from(username + "\0"),
        ];
        const cfg = new ArrayBuffer(40);
        const dv = new DataView(cfg);
        dv.setBigUint64(0, BigInt(ptr(bufs[0])), true);
        dv.setBigUint64(8, BigInt(ptr(bufs[1])), true);
        dv.setBigUint64(16, BigInt(ptr(bufs[2])), true);
        dv.setInt32(24, 1, true); /* plain */
        dv.setInt32(28, 0, true); /* debug */
        dv.setInt32(32, 1, true); /* no_color */

        const ctx = lib.symbols.shyake_init_ctx(ptr(new Uint8Array(cfg)));
        /* keep bufs alive until init returns */
        if (!ctx || ctx === 0) {
            throw new ShyakeError(SHYAKE_ERR, "failed to initialize shyake context");
        }
        this.ctx = ctx;
        void bufs;
    }

    close() {
        if (this.ctx) {
            lib.symbols.shyake_free_ctx(this.ctx);
            this.ctx = 0;
        }
    }

    /** Serialize all library calls through one chain. */
    private run<T>(fn: () => T): Promise<T> {
        const p = this.queue.then(fn);
        this.queue = p.catch(() => {});
        return p;
    }

    private fail(code: number): never {
        throw new ShyakeError(code, cstr(lib.symbols.shyake_last_error(this.ctx)));
    }

    setPassphrase(passphrase: string | null) {
        lib.symbols.shyake_set_passphrase(this.ctx, passphrase);
    }

    setNewPassphrase(passphrase: string | null) {
        lib.symbols.shyake_set_new_passphrase(this.ctx, passphrase);
    }

    /** Load the own KEM secret key; fails on a wrong passphrase. */
    verifyKey(): Promise<void> {
        return this.run(() => {
            const sd = lib.symbols.shyake_selfdec_new(this.ctx);
            if (!sd) this.fail(SHYAKE_ERR_CRYPTO);
            lib.symbols.shyake_selfdec_free(sd);
        });
    }

    generateKeys(): Promise<void> {
        return this.run(() => {
            if (lib.symbols.shyake_generate_keys(this.ctx) !== 0)
                this.fail(SHYAKE_ERR_CRYPTO);
        });
    }

    register(username: string): Promise<void> {
        return this.run(() => {
            const ret = lib.symbols.shyake_register(this.ctx, username);
            if (ret !== SHYAKE_OK) this.fail(ret);
        });
    }

    send(recipient: string, subject: string, body: string): Promise<void> {
        return this.run(() => {
            const data = Buffer.from(body, "utf8");
            const ret = lib.symbols.shyake_send(
                this.ctx, recipient, subject, ptr(data), data.length);
            if (ret !== SHYAKE_OK) this.fail(ret);
        });
    }

    check(box: "inbox" | "sent"): Promise<MailEntryOut[]> {
        return this.run(() => {
            const lst = lib.symbols.shyake_check(this.ctx, box);
            if (!lst) this.fail(SHYAKE_ERR);
            try {
                const entries = read.ptr(lst, 0);
                const count = read.i32(lst, 8);
                const out: MailEntryOut[] = [];
                for (let i = 0; i < count; i++) {
                    const e = entries + i * MAIL_ENTRY_SIZE;
                    out.push({
                        mail_id: cstr(read.ptr(e, 0)),
                        party: cstr(read.ptr(e, 8)),
                        subject: cstr(read.ptr(e, 16)),
                        size: read.i32(e, 24),
                        timestamp: i64(e, 32),
                        is_sent: read.i32(e, 48) !== 0,
                    });
                }
                return out;
            } finally {
                lib.symbols.shyake_free_mail_list(lst);
            }
        });
    }

    private readDetail(d: number): MailDetailOut {
        return {
            mail_id: cstr(read.ptr(d, 0)),
            sender: cstr(read.ptr(d, 8)),
            recipient: cstr(read.ptr(d, 16)),
            subject: cstr(read.ptr(d, 24)),
            body: cstr(read.ptr(d, 32)),
            timestamp: i64(d, 40),
            size: read.i32(d, 56),
        };
    }

    fetch(mailId: string): Promise<MailDetailOut> {
        return this.run(() => {
            const d = lib.symbols.shyake_fetch(this.ctx, mailId);
            if (!d) this.fail(SHYAKE_ERR);
            try {
                return this.readDetail(d);
            } finally {
                lib.symbols.shyake_free_mail_detail(d);
            }
        });
    }

    readSaved(mailId: string): Promise<MailDetailOut> {
        return this.run(() => {
            const d = lib.symbols.shyake_read_saved(this.ctx, mailId);
            if (!d) this.fail(SHYAKE_ERR);
            try {
                return this.readDetail(d);
            } finally {
                lib.symbols.shyake_free_mail_detail(d);
            }
        });
    }

    listSaved(): Promise<MailEntryOut[]> {
        return this.run(() => {
            const lst = lib.symbols.shyake_list_saved(this.ctx);
            if (!lst) this.fail(SHYAKE_ERR);
            try {
                const entries = read.ptr(lst, 0);
                const count = read.i32(lst, 8);
                const out: MailEntryOut[] = [];
                for (let i = 0; i < count; i++) {
                    const e = entries + i * SAVED_ENTRY_SIZE;
                    out.push({
                        mail_id: cstr(read.ptr(e, 0)),
                        party: cstr(read.ptr(e, 8)), /* sender */
                        subject: cstr(read.ptr(e, 24)),
                        size: read.i32(e, 48),
                        timestamp: i64(e, 32),
                        is_sent: false,
                    });
                }
                return out;
            } finally {
                lib.symbols.shyake_free_saved_list(lst);
            }
        });
    }

    burn(mailId: string): Promise<void> {
        return this.run(() => {
            const ret = lib.symbols.shyake_burn(this.ctx, mailId);
            if (ret !== SHYAKE_OK) this.fail(ret);
        });
    }

    saveMail(mailId: string): Promise<void> {
        return this.run(() => {
            const ret = lib.symbols.shyake_save_mail(this.ctx, mailId);
            if (ret !== SHYAKE_OK) this.fail(ret);
        });
    }

    block(target: string, unblock: boolean): Promise<void> {
        return this.run(() => {
            const ret = lib.symbols.shyake_block(this.ctx, target, unblock ? 1 : 0);
            if (ret !== SHYAKE_OK) this.fail(ret);
        });
    }

    listBlocks(): Promise<{ target: string | null; created: number }[]> {
        return this.run(() => {
            const lst = lib.symbols.shyake_list_blocks(this.ctx);
            if (!lst) this.fail(SHYAKE_ERR);
            try {
                const entries = read.ptr(lst, 0);
                const count = read.i32(lst, 8);
                const out = [];
                for (let i = 0; i < count; i++) {
                    const e = entries + i * BLOCK_ENTRY_SIZE;
                    out.push({ target: cstr(read.ptr(e, 0)), created: i64(e, 8) });
                }
                return out;
            } finally {
                lib.symbols.shyake_free_block_list(lst);
            }
        });
    }

    rotate(): Promise<void> {
        return this.run(() => {
            const ret = lib.symbols.shyake_rotate(this.ctx);
            if (ret !== SHYAKE_OK) this.fail(ret);
        });
    }

    destroy(): Promise<void> {
        return this.run(() => {
            const ret = lib.symbols.shyake_destroy(this.ctx);
            if (ret !== SHYAKE_OK) this.fail(ret);
        });
    }

    fingerprint(targetUser: string | null, doUpdate: boolean): Promise<{
        local_fp: string;
        remote_fp: string;
        has_local: boolean;
        match: boolean;
    }> {
        return this.run(() => {
            const fp = lib.symbols.shyake_fingerprint(
                this.ctx, targetUser, doUpdate ? 1 : 0);
            if (!fp) this.fail(SHYAKE_ERR);
            try {
                const hex = (off: number) => {
                    let s = "";
                    for (let i = 0; i < 32; i++)
                        s += read.u8(fp, off + i).toString(16).padStart(2, "0");
                    return s;
                };
                return {
                    local_fp: hex(0),
                    remote_fp: hex(32),
                    has_local: read.i32(fp, 64) !== 0,
                    match: read.i32(fp, 68) !== 0,
                };
            } finally {
                lib.symbols.shyake_free_fp_result(fp);
            }
        });
    }
}
