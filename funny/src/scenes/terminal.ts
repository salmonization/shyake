// A workstation desktop: salmon sends the letter from one terminal,
// flat_white reads it in another, then the key is checked by hand.
// Output formats follow client/src/cli/display.c.

import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { drawLogo } from '../engine/logo.ts';
import { clamp, easeIn, easeOut, hold, span } from '../engine/math.ts';
import { C, MOTIF } from '../engine/palette.ts';
import { BAR, kickEnv } from '../engine/score.ts';
import { terminalScript, type Command, type Pane } from '../engine/script.ts';
import type { Frame, SceneDef } from '../engine/types.ts';
import { artifacts, BODY, fingerprintLines, randomart, SUBJECT } from '../data/artifacts.ts';

const SIZE = 20;
const LH = 26;
const CW = SIZE * 0.6;
const PROMPT: Record<Pane, string> = { left: 'salmon@lattice% ', right: 'flat_white@wheel% ' };

interface Rect {
    x: number;
    y: number;
    w: number;
    h: number;
}

const WINDOWS: Record<Pane, { rect: Rect; title: string }> = {
    left: { rect: { x: 70, y: 96, w: 920, h: 800 }, title: 'winterm · salmon@lattice' },
    right: { rect: { x: 1020, y: 96, w: 830, h: 540 }, title: 'winterm · flat_white@wheel' },
};

function outputs(): Record<Command['key'], string[]> {
    const a = artifacts();
    const fp = [...fingerprintLines(a.fp), ...randomart(a.fp)];
    const size = new TextEncoder().encode(BODY).length;
    return {
        whoami: [
            'USERNAME: salmon',
            'INSTANCE: https://a.example',
            'CONFIG:   /home/salmon/.config/shyake',
        ],
        send: ['Mail sent.'],
        inbox: [
            'ID          From      Subject                 Size  Date',
            `${a.mailIds[0]}  salmon    ${SUBJECT.padEnd(22)}  ${String(size).padEnd(4)}  09:52`,
        ],
        fetch: [
            'FROM:  salmon',
            'TO:    flat_white',
            'DATE:  Sep 27 09:52',
            `SUBJ:  ${SUBJECT}`,
            ...BODY.split('\n'),
        ],
        fingerprint: [
            'Fetching public key for flat_white...',
            '',
            '[Local known_hosts]',
            ...fp,
            '',
            '[Remote server]',
            ...fp,
            '',
            'Status: MATCH',
        ],
    };
}

// Split a line to the terminal width
function wrap(s: string, cols: number): string[] {
    if (s.length <= cols) return [s];
    const out: string[] = [];
    for (let i = 0; i < s.length; i += cols) out.push(s.slice(i, i + cols));
    return out;
}

interface Row {
    text: string;
    label?: boolean;
}

function paneRows(
    pane: Pane,
    t: number,
    script: Command[],
    out: Record<Command['key'], string[]>,
    cols: number,
) {
    const rows: Row[] = [];
    let cursorAfter = true;
    const mine = script.filter((x) => x.pane === pane);
    for (const [ci, c] of mine.entries()) {
        if (t < c.typed[0].t - 0.4) break;
        let typed = '';
        for (const k of c.typed) if (t >= k.t) typed += k.ch;
        for (const w of wrap(PROMPT[pane] + typed, cols)) rows.push({ text: w });
        if (t < c.out) {
            cursorAfter = true;
            return { rows, cursorAfter };
        }
        const lines = out[c.key];
        const shown = Math.min(lines.length, Math.floor((t - c.out) / 0.035) + 1);
        for (const l of lines.slice(0, shown))
            for (const w of wrap(l, cols))
                rows.push({ text: w, label: /^[A-Z]{2,8}:/.test(l) && c.key === 'fetch' });
        const next = mine[ci + 1];
        if (!next || t < next.typed[0].t - 0.4) rows.push({ text: PROMPT[pane] });
    }
    if (!rows.length) rows.push({ text: PROMPT[pane] });
    return { rows, cursorAfter };
}

function desktop(hud: Hud) {
    const c = hud.ctx;
    const g = c.createLinearGradient(0, 0, 0, H);
    g.addColorStop(0, '#22345a');
    g.addColorStop(1, '#0c1628');
    c.fillStyle = g;
    c.fillRect(0, 0, W, H);
    // faint weave
    for (let x = -H; x < W; x += 24)
        hud.line(
            [
                [x, H],
                [x + H, 0],
            ],
            '#3a5282',
            1,
            0.07,
        );
    drawLogo(c, W - 250, H - 120, 90, '#9fb4d8', 0.35);
    hud.text('Salmonization', W - 110, H - 60, {
        face: 'sans',
        size: 16,
        spacing: 3,
        align: 'right',
        color: '#9fb4d8',
        alpha: 0.5,
    });
}

// Map and unmap animation: grow from the center, shrink to an icon
function mapped(r: Rect, open: number, iconify: number, iconX: number): Rect {
    const s = 0.4 + 0.6 * easeOut(open);
    const cx = r.x + r.w / 2;
    const cy = r.y + r.h / 2;
    const w = r.w * s * (1 - 0.92 * iconify);
    const h = r.h * s * (1 - 0.94 * iconify);
    const x = cx + (iconX - cx) * easeIn(iconify, 2) - w / 2;
    const y = cy + (H - 60 - cy) * easeIn(iconify, 2) - h / 2;
    return { x, y, w, h };
}

export const terminal: SceneDef = {
    id: 'terminal',
    sections: ['terminal'],
    create() {
        const a = artifacts();
        const script = terminalScript(a.mailIds[0]);
        const out = outputs();

        return {
            update(f: Frame, hud: Hud) {
                const bar = f.bar;
                const t = f.t;
                desktop(hud);
                const iconify = span(bar, 6.9, 7.4);

                (['left', 'right'] as Pane[]).forEach((pane, i) => {
                    const def = WINDOWS[pane];
                    const open = span(bar, 0.05 + i * 0.1, 0.3 + i * 0.1);
                    if (open <= 0) return;
                    const r = mapped(def.rect, open, iconify, 120 + i * 140);
                    const alpha = clamp(open * 3) * (1 - iconify * 0.6);
                    const inner = hud.window(r.x, r.y, r.w, r.h, def.title, {
                        alpha,
                        active:
                            pane === script.find((c) => t >= c.typed[0].t && t < c.out + 2)?.pane,
                    });
                    if (iconify > 0.3) return;
                    const cols = Math.floor((inner.w - 32) / CW);
                    const maxRows = Math.floor((inner.h - 24) / LH);
                    const { rows } = paneRows(pane, t, script, out, cols);
                    const visible = rows.slice(Math.max(0, rows.length - maxRows));
                    hud.ctx.save();
                    hud.ctx.beginPath();
                    hud.ctx.rect(inner.x, inner.y, inner.w, inner.h);
                    hud.ctx.clip();
                    visible.forEach((row, k) => {
                        const y = inner.y + 30 + k * LH;
                        if (row.label) {
                            const [lab, ...rest] = row.text.split(':');
                            hud.text(lab + ':', inner.x + 16, y, {
                                face: 'monoBold',
                                size: SIZE,
                                color: C.cyan,
                                alpha,
                            });
                            hud.text(rest.join(':'), inner.x + 16 + (lab.length + 1) * CW, y, {
                                face: 'mono',
                                size: SIZE,
                                color: MOTIF.text,
                                alpha,
                            });
                        } else {
                            const isArt = /^[|+]/.test(row.text);
                            hud.text(row.text, inner.x + 16, y, {
                                face: 'mono',
                                size: SIZE,
                                color: isArt
                                    ? C.frost
                                    : row.text === 'Status: MATCH'
                                      ? C.phosphor
                                      : MOTIF.text,
                                alpha,
                            });
                        }
                    });
                    const last = visible[visible.length - 1];
                    if (Math.floor(t / 0.3) % 2 === 0 && last)
                        hud.rect(
                            inner.x + 16 + last.text.length * CW,
                            inner.y + 30 + (visible.length - 1) * LH - SIZE + 3,
                            CW,
                            SIZE,
                            MOTIF.text,
                            alpha * 0.85,
                        );
                    hud.ctx.restore();
                });

                // clock and load meter
                const small = span(bar, 0.4, 0.7);
                if (small > 0) {
                    const alpha = clamp(small * 3) * (1 - iconify);
                    const ck = mapped({ x: 1020, y: 660, w: 250, h: 236 }, small, iconify, 400);
                    const ci = hud.window(ck.x, ck.y, ck.w, ck.h, 'xclock', {
                        alpha,
                        active: false,
                    });
                    if (iconify < 0.3) clock(hud, ci, t, alpha);
                    const lm = mapped({ x: 1300, y: 660, w: 550, h: 236 }, small, iconify, 540);
                    const li = hud.window(lm.x, lm.y, lm.w, lm.h, 'loadmeter', {
                        alpha,
                        active: false,
                    });
                    if (iconify < 0.3) meter(hud, li, t, alpha);
                }

                hud.scrim(H - 180, H, 0.75 * (1 - iconify));
                hud.text(
                    'A plain command line. Any client can do the same through libshyake.',
                    W / 2,
                    H - 50,
                    {
                        face: 'italic',
                        size: 36,
                        align: 'center',
                        alpha: hold(bar, 0.6, 3.6, 0.4, 0.3),
                    },
                );
                hud.text(
                    'Keys are trusted on first use, and checked on every send after that.',
                    W / 2,
                    H - 50,
                    {
                        face: 'italic',
                        size: 36,
                        align: 'center',
                        alpha: hold(bar, 3.8, 6.8, 0.4, 0.3),
                    },
                );

                return {
                    post: {
                        curve: 0.5,
                        scan: 0.35,
                        aberr: 0.3,
                        grain: 0.35,
                        dither: 0.35,
                        bloom: 0.25,
                        threshold: 0.75,
                        vignette: 0.8,
                        fade: easeIn(span(bar, 7.4, 8), 2),
                        flash: 0.4 * Math.exp(-f.local / 0.2),
                    },
                };
            },
        };
    },
};

function clock(hud: Hud, r: Rect, t: number, alpha: number) {
    const cx = r.x + r.w / 2;
    const cy = r.y + r.h / 2;
    const rad = Math.min(r.w, r.h) / 2 - 16;
    const c = hud.ctx;
    c.save();
    c.globalAlpha = alpha;
    c.strokeStyle = MOTIF.text;
    for (let i = 0; i < 60; i++) {
        const a = (i / 60) * Math.PI * 2;
        const l = i % 5 === 0 ? 12 : 4;
        c.lineWidth = i % 5 === 0 ? 3 : 1;
        c.beginPath();
        c.moveTo(cx + Math.sin(a) * (rad - l), cy - Math.cos(a) * (rad - l));
        c.lineTo(cx + Math.sin(a) * rad, cy - Math.cos(a) * rad);
        c.stroke();
    }
    // 09:52, running with the song
    const secs = 9 * 3600 + 52 * 60 + (t % 3600);
    const hand = (frac: number, len: number, w: number, color: string) => {
        const a = frac * Math.PI * 2;
        c.strokeStyle = color;
        c.lineWidth = w;
        c.beginPath();
        c.moveTo(cx, cy);
        c.lineTo(cx + Math.sin(a) * len, cy - Math.cos(a) * len);
        c.stroke();
    };
    hand((secs / 43200) % 1, rad * 0.5, 5, MOTIF.text);
    hand((secs / 3600) % 1, rad * 0.8, 3, MOTIF.text);
    hand((Math.floor(secs) % 60) / 60, rad * 0.85, 1.5, C.cyan);
    c.restore();
}

function meter(hud: Hud, r: Rect, t: number, alpha: number) {
    const n = 64;
    const span4 = BAR * 2;
    const bw = (r.w - 24) / n;
    for (let i = 0; i < n; i++) {
        const tt = t - span4 + (i / n) * span4;
        const v = tt < 0 ? 0 : kickEnv(tt, 0.3) * 0.8 + 0.1 + 0.1 * Math.abs(Math.sin(tt * 7.3));
        const h = v * (r.h - 30);
        hud.rect(
            r.x + 12 + i * bw,
            r.y + r.h - 12 - h,
            bw - 2,
            h,
            i === n - 1 ? C.phosphor : C.cyan,
            alpha * 0.8,
        );
    }
    hud.line(
        [
            [r.x + 12, r.y + r.h / 2],
            [r.x + r.w - 12, r.y + r.h / 2],
        ],
        MOTIF.light,
        1,
        alpha * 0.4,
        [4, 4],
    );
}
