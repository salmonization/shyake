// The server's view, heard through a wall: a sqlite3 session over the
// mail table. Every column that matters is ciphertext.

import { sha256 } from '@noble/hashes/sha2.js';
import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { easeOut, hold, span } from '../engine/math.ts';
import { C, MOTIF } from '../engine/palette.ts';
import { BEAT, STEP } from '../engine/score.ts';
import type { Frame, SceneDef } from '../engine/types.ts';
import { artifacts, b64, RECIPIENT, SENDER } from '../data/artifacts.ts';

const QUERY = 'SELECT mail_id, sender, recipient, enc_subject, enc_body FROM mail;';

// Other rows: opaque bytes of plausible lengths, as the server sees them
function opaque(label: string, len: number): string {
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i += 32)
        out.set(
            sha256(new TextEncoder().encode(`${label}/${i}`)).subarray(0, Math.min(32, len - i)),
            i,
        );
    return b64(out);
}

const cut = (s: string, n: number) => (s.length > n ? s.slice(0, n - 1) + '…' : s.padEnd(n));

export const server: SceneDef = {
    id: 'server',
    sections: ['server'],
    create() {
        const a = artifacts();
        const people: [string, string][] = [
            [SENDER, RECIPIENT],
            ['flat_white', 'salmon'],
            ['mocha@b.example', 'salmon'],
            [SENDER, 'cortado@c.example.net'],
            ['flat_white', 'ristretto'],
            ['doppio@d.example.org', 'flat_white'],
        ];
        const rows = people.map(([from, to], i) => {
            const subj = i === 0 ? a.encSubject : opaque(`s${i}`, 40 + i * 3);
            const body = i === 0 ? a.encBody : opaque(`b${i}`, 90 + i * 37);
            return [a.mailIds[i], cut(from, 21), cut(to, 21), cut(subj, 26), cut(body, 40)].join(
                '|',
            );
        });

        // background rain of the recipient's encapsulated key
        const rain = a.encKeyRecipient;

        return {
            update(f: Frame, hud: Hud) {
                const bar = f.bar;
                hud.rect(0, 0, W, H, '#050c16');
                for (let c = 0; c < 40; c++) {
                    const x = 30 + c * 48;
                    const speed = 40 + ((c * 37) % 50);
                    const off = Math.floor((f.local * speed) / 22 + c * 13);
                    for (let r = 0; r < 42; r++) {
                        const ch = rain[(off + r * 7 + c * 31) % rain.length];
                        hud.text(ch, x, r * 26 + ((f.local * speed) % 26), {
                            face: 'mono',
                            size: 18,
                            color: C.slate,
                            alpha: 0.5,
                        });
                    }
                }

                const wA = hold(bar, 0, 3.85, 0.15, 0.2);
                const win = hud.window(150, 170, W - 300, 560, 'sqlite3 shyake.db', { alpha: wA });
                const typed = Math.floor(Math.max(0, f.local - 0.15) / (STEP / 4));
                hud.text('sqlite> ' + QUERY.slice(0, typed), win.x + 22, win.y + 44, {
                    face: 'mono',
                    size: 20,
                    color: MOTIF.text,
                    alpha: wA,
                });
                const r0 = 0.15 + (QUERY.length * STEP) / 4 + BEAT / 2;
                rows.forEach((row, i) => {
                    if (f.local < r0 + i * STEP) return;
                    hud.text(row, win.x + 22, win.y + 104 + i * 44, {
                        face: 'mono',
                        size: 20,
                        color: i === 0 ? C.phosphor : C.fog,
                        alpha: wA,
                    });
                });
                if (f.local > r0 + rows.length * STEP)
                    hud.text('sqlite> ', win.x + 22, win.y + 104 + rows.length * 44 + 20, {
                        face: 'mono',
                        size: 20,
                        color: MOTIF.text,
                        alpha: wA,
                    });

                hud.scrim(H - 330, H, 0.9);
                hud.text('This is everything the server holds.', W / 2, H - 170, {
                    face: 'italic',
                    size: 50,
                    align: 'center',
                    alpha: hold(bar, 1.6, 3.9, 0.35, 0.2),
                });
                hud.text('No key. No plaintext. Nothing to hand over.', W / 2, H - 100, {
                    face: 'sans',
                    size: 24,
                    spacing: 6,
                    align: 'center',
                    color: C.fog,
                    alpha: hold(bar, 2.5, 3.9, 0.3, 0.2),
                });

                return {
                    post: {
                        bloom: 0.3,
                        threshold: 0.7,
                        scan: 0.35,
                        aberr: 0.3,
                        grain: 0.5,
                        dither: 0.3,
                        vignette: 1,
                        fade: 1 - easeOut(span(f.local, 0, 0.3)),
                        flash: 0.6 * Math.max(0, span(bar, 3.7, 4)) ** 2,
                    },
                };
            },
        };
    },
};
