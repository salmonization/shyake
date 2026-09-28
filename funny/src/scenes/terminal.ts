// A workstation desktop: salmon sends the letter from one terminal,
// flat_white reads it in another, then the key is checked by hand.
// Output formats follow client/src/cli/display.c.

import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { drawLogo } from '../engine/logo.ts';
import { smooth } from '../engine/math.ts';
import { MOTIF } from '../engine/palette.ts';
import { terminalScript, type Command, type Pane } from '../engine/script.ts';
import type { Frame, SceneDef } from '../engine/types.ts';
import { clock, desktop, type Rect } from '../engine/widgets.ts';
import { artifacts, BODY, fingerprintLines, randomart, SUBJECT } from '../data/artifacts.ts';

const SIZE = 20;
const LH = 26;
const CW = SIZE * 0.6;
const PROMPT: Record<Pane, string> = { left: 'salmon@lattice% ', right: 'flat_white@wheel% ' };

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

// A quiet load meter: the pad's slow breathing, not a beat
function meter(hud: Hud, r: Rect, t: number, alpha: number) {
    const n = 48;
    const bw = (r.w - 24) / n;
    for (let i = 0; i < n; i++) {
        const tt = t - 6 + (i / n) * 6;
        const v = 0.25 + 0.2 * Math.sin(tt * 0.9) + 0.12 * Math.sin(tt * 2.3 + 1);
        const h = v * (r.h - 30);
        hud.rect(r.x + 12 + i * bw, r.y + r.h - 12 - h, bw - 2, h, '#8fa0a8', alpha * 0.8);
    }
}

export const terminal: SceneDef = {
    id: 'desk',
    sections: ['desk'],
    create() {
        const a = artifacts();
        const script = terminalScript(a.mailIds[0]);
        const out = outputs();

        return {
            update(f: Frame, hud: Hud) {
                const t = f.t;
                desktop(hud);
                drawLogo(hud.ctx, W - 230, H - 110, 70, '#aebcc2', 0.3);

                (['left', 'right'] as Pane[]).forEach((pane) => {
                    const def = WINDOWS[pane];
                    const r = def.rect;
                    const active =
                        pane === script.find((c) => t >= c.typed[0].t && t < c.out + 2)?.pane;
                    const inner = hud.window(r.x, r.y, r.w, r.h, def.title, { active });
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
                                color: '#aebcc2',
                            });
                            hud.text(rest.join(':'), inner.x + 16 + (lab.length + 1) * CW, y, {
                                face: 'mono',
                                size: SIZE,
                                color: MOTIF.text,
                            });
                        } else
                            hud.text(row.text, inner.x + 16, y, {
                                face: 'mono',
                                size: SIZE,
                                color:
                                    /^[|+]/.test(row.text) || row.text === 'Status: MATCH'
                                        ? '#ffffff'
                                        : MOTIF.text,
                            });
                    });
                    const last = visible[visible.length - 1];
                    if (Math.floor(t / 0.4) % 2 === 0 && last)
                        hud.rect(
                            inner.x + 16 + last.text.length * CW,
                            inner.y + 30 + (visible.length - 1) * LH - SIZE + 3,
                            CW,
                            SIZE,
                            MOTIF.text,
                            0.85,
                        );
                    hud.ctx.restore();
                });

                const ci = hud.window(1020, 660, 250, 236, 'xclock', { active: false });
                clock(hud, ci, 9 * 3600 + 52 * 60 + f.local, 1);
                const li = hud.window(1300, 660, 550, 236, 'loadmeter', { active: false });
                meter(hud, li, t, 1);

                return {
                    post: {
                        grain: 0.3,
                        vignette: 0.45,
                        fade: 1 - smooth(f.local / 0.9),
                    },
                };
            },
        };
    },
};
