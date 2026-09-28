// Captions: one sentence at a time, centred, in a quiet serif at
// reading size. They arrive slowly and leave slowly.

import type { Hud } from './hud.ts';
import { H, W } from './hud.ts';
import { hold } from './math.ts';
import { C } from './palette.ts';

export const CAPTION_SIZE = 34;
const MAX_W = 1240;

export interface Line {
    // absolute song seconds
    from: number;
    to: number;
    text: string;
    // a smaller second line, such as an attribution
    small?: string;
}

function wrap(hud: Hud, text: string, size: number): string[] {
    const words = text.split(' ');
    const lines: string[] = [];
    let cur = '';
    for (const w of words) {
        const next = cur ? `${cur} ${w}` : w;
        if (cur && hud.measure(next, 'serif', size) > MAX_W) {
            lines.push(cur);
            cur = w;
        } else cur = next;
    }
    if (cur) lines.push(cur);
    return lines;
}

export interface CaptionOpts {
    y?: number;
    color?: string;
    size?: number;
}

export function caption(hud: Hud, text: string, alpha: number, o: CaptionOpts = {}) {
    if (alpha <= 0) return;
    const size = o.size ?? CAPTION_SIZE;
    const lh = size * 1.55;
    const lines = wrap(hud, text, size);
    const y0 = (o.y ?? H - 150) - ((lines.length - 1) * lh) / 2;
    lines.forEach((l, i) =>
        hud.text(l, W / 2, y0 + i * lh, {
            face: 'serif',
            size,
            align: 'center',
            color: o.color ?? C.paper,
            alpha,
        }),
    );
}

export function captions(hud: Hud, t: number, lines: Line[], o: CaptionOpts = {}) {
    for (const l of lines) {
        const a = hold(t, l.from, l.to, 1.4, 1.2);
        if (a <= 0) continue;
        caption(hud, l.text, a, o);
        if (l.small)
            hud.text(l.small, W / 2, (o.y ?? H - 150) + 64, {
                face: 'serif',
                size: 24,
                align: 'center',
                color: C.mist,
                alpha: a,
            });
    }
}
