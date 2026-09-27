// Every byte the film shows is real. This module runs the actual
// Shyake constructions (SPEC.md §3) with fixed seeds: ML-KEM-768 key
// encapsulation, ML-DSA-65 signatures, ChaCha20-Poly1305 mail bodies,
// header signing with the body digest, and a minted Hashcash token.

import { chacha20, chacha20poly1305 } from '@noble/ciphers/chacha.js';
import { sha256 } from '@noble/hashes/sha2.js';
import { bytesToHex } from '@noble/hashes/utils.js';
import { ml_dsa65 } from '@noble/post-quantum/ml-dsa.js';
import { ml_kem768 } from '@noble/post-quantum/ml-kem.js';
import { chachaTrace, type ChachaTrace } from './chacha.ts';

const utf8 = new TextEncoder();

function seed(label: string, len = 32): Uint8Array {
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i += 32) {
        const block = sha256(utf8.encode(`shyake/funny/${label}/${i}`));
        out.set(block.subarray(0, Math.min(32, len - i)), i);
    }
    return out;
}

export function b64(bytes: Uint8Array): string {
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return btoa(s);
}

export const hex = bytesToHex;

function concat(...parts: Uint8Array[]): Uint8Array {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) {
        out.set(p, o);
        o += p.length;
    }
    return out;
}

function xor(a: Uint8Array, b: Uint8Array): Uint8Array {
    const out = new Uint8Array(a.length);
    for (let i = 0; i < a.length; i++) out[i] = a[i] ^ b[i];
    return out;
}

// SPEC.md §3.6: lowercase hex SHA-256 of the raw ML-KEM public key
export function fingerprint(kemPk: Uint8Array): Uint8Array {
    return sha256(kemPk);
}

// Same walk as cli_print_randomart() in client/src/cli/display.c
export function randomart(fp: Uint8Array): string[] {
    let x = 8;
    let y = 4;
    const grid = Array.from({ length: 17 }, () => Array.from({ length: 9 }, () => 0));
    for (let i = 0; i < 32; i++) {
        const b = fp[i];
        for (let j = 0; j < 4; j++) {
            const dir = (b >> (j * 2)) & 3;
            x = Math.min(16, Math.max(0, x + (dir & 1 ? 1 : -1)));
            y = Math.min(8, Math.max(0, y + (dir & 2 ? 1 : -1)));
            grid[x][y]++;
        }
    }
    const chars = ' .o+=*BOX@%&#/^';
    const lines = ['+-----------------+'];
    for (let j = 0; j < 9; j++) {
        let row = '|';
        for (let i = 0; i < 17; i++) {
            if (i === 8 && j === 4) row += 'S';
            else if (i === x && j === y) row += 'E';
            else row += chars[Math.min(14, grid[i][j])];
        }
        lines.push(row + '|');
    }
    lines.push('+-----------------+');
    return lines;
}

// Same grouping as cli_print_fingerprint_hex()
export function fingerprintLines(fp: Uint8Array): string[] {
    const w = (i: number) => hex(fp.subarray(i * 2, i * 2 + 2)).toUpperCase();
    const row = (o: number) =>
        [0, 1, 2, 3].map((i) => w(o + i)).join(' ') +
        '  ' +
        [4, 5, 6, 7].map((i) => w(o + i)).join(' ');
    return [row(0), row(8)];
}

const BASE58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';

function mailId(label: string): string {
    const b = seed(label, 10);
    let s = '';
    for (let i = 0; i < 10; i++) s += BASE58[b[i] % 58];
    return s;
}

/* ------------------------------------------------------------------ */
/* The cast                                                           */
/* ------------------------------------------------------------------ */

export const SENDER = 'salmon';
export const RECIPIENT = 'flat_white';
export const TIMESTAMP = 1790502720; // 2026-09-27 09:52 UTC
export const BLOCK_TARGET = 'mallory@evil.example';

// Minted offline; tests check that it really has 20 zero bits
export const POW = '1:20:260927:salmon::cx52pinwheel:b82c';

export const SUBJECT = 'Where the wheels turn';
export const BODY =
    'The lid is open. Six pin wheels, one lug cage,\n' +
    'and a key you could set by hand.\n' +
    'Nobody else can read this line.';

export interface Artifacts {
    kem: { pk: Uint8Array; sk: Uint8Array; ct: Uint8Array; ss: Uint8Array };
    dsa: { pk: Uint8Array; sk: Uint8Array };
    fp: Uint8Array;
    fpHex: string;
    symKey: Uint8Array;
    nonce: Uint8Array;
    encSubject: string;
    encBody: string;
    encKeyRecipient: string;
    encKeySender: string;
    mailSigned: string;
    mailSig: Uint8Array;
    mailIds: string[];
    blockBody: string;
    blockDigest: string;
    blockSigned: string;
    blockSig: Uint8Array;
    trace: ChachaTrace;
    plainLine: string;
    cipherLine: Uint8Array;
    tagLine: Uint8Array;
}

let cached: Artifacts | null = null;

export function artifacts(): Artifacts {
    if (cached) return cached;

    // Keys
    const rk = ml_kem768.keygen(seed('kem/flat_white', 64));
    const sk = ml_kem768.keygen(seed('kem/salmon', 64));
    const dsa = ml_dsa65.keygen(seed('dsa/salmon'));
    const fp = fingerprint(rk.publicKey);

    // Mail body, SPEC.md §3.2
    const symKey = seed('sym');
    const nSub = seed('nonce/subject', 12);
    const nBody = seed('nonce/body', 12);
    const seal = (n: Uint8Array, text: string) =>
        b64(concat(n, chacha20poly1305(symKey, n).encrypt(utf8.encode(text))));
    const encSubject = seal(nSub, SUBJECT);
    const encBody = seal(nBody, BODY);

    const toR = ml_kem768.encapsulate(rk.publicKey, seed('encaps/recipient'));
    const toS = ml_kem768.encapsulate(sk.publicKey, seed('encaps/sender'));
    const encKeyRecipient = b64(concat(toR.cipherText, xor(symKey, toR.sharedSecret)));
    const encKeySender = b64(concat(toS.cipherText, xor(symKey, toS.sharedSecret)));

    // Body-based signature over the compact JSON subset, §3.3
    const mailSigned = JSON.stringify({
        sender: SENDER,
        recipient: RECIPIENT,
        recipient_kem_fingerprint: hex(fp),
        enc_subject: encSubject,
        enc_body: encBody,
        timestamp: String(TIMESTAMP),
        size: utf8.encode(BODY).length,
    });
    const mailSig = ml_dsa65.sign(utf8.encode(mailSigned), dsa.secretKey, {
        extraEntropy: false,
    });

    // Header-based signature with the body digest, protocol level 2
    const blockBody = JSON.stringify({ target: BLOCK_TARGET });
    const blockDigest = hex(sha256(utf8.encode(blockBody)));
    const blockSigned = `POST:/api/block:${SENDER}:${TIMESTAMP}:${blockDigest}`;
    const blockSig = ml_dsa65.sign(utf8.encode(blockSigned), dsa.secretKey, {
        extraEntropy: false,
    });

    // One ChaCha20 block, traced round by round
    const trace = chachaTrace(symKey, 1, nBody);
    const plainLine = 'Nobody else can read this line.';
    const pBytes = utf8.encode(plainLine);
    const cipherLine = chacha20(symKey, nBody, pBytes, undefined, 1);
    const sealed = chacha20poly1305(symKey, nBody).encrypt(pBytes);
    const tagLine = sealed.subarray(sealed.length - 16);

    cached = {
        kem: { pk: rk.publicKey, sk: rk.secretKey, ct: toR.cipherText, ss: toR.sharedSecret },
        dsa: { pk: dsa.publicKey, sk: dsa.secretKey },
        fp,
        fpHex: hex(fp),
        symKey,
        nonce: nBody,
        encSubject,
        encBody,
        encKeyRecipient,
        encKeySender,
        mailSigned,
        mailSig,
        mailIds: ['a', 'b', 'c', 'd', 'e', 'f'].map((k) => mailId(`mail/${k}`)),
        blockBody,
        blockDigest,
        blockSigned,
        blockSig,
        trace,
        plainLine,
        cipherLine,
        tagLine,
    };
    return cached;
}
