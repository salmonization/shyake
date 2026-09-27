// Power-on: a workstation PROM prints the Shyake parameters, is told
// to boot, and hands over to the first line of the story.

import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { drawLogo } from '../engine/logo.ts';
import { easeOut, hold, span } from '../engine/math.ts';
import { C } from '../engine/palette.ts';
import { at, BEAT, sectionStart } from '../engine/score.ts';
import { BOOT_CLEAR, BOOT_LINES } from '../engine/script.ts';
import type { Frame, SceneDef } from '../engine/types.ts';

const X = 360;
const Y = 170;
const LH = 38;
const SIZE = 25;

function powerOn(hud: Hud, t: number) {
    // a bright line that opens vertically into the raster
    const open = easeOut(span(t, 0.05, 0.35), 4);
    const h = 3 + open * H;
    hud.rect(0, 0, W, H, C.ink);
    hud.rect(0, H / 2 - h / 2, W, h, '#0a1522');
    const glow = Math.max(0, 1 - span(t, 0.05, 0.6));
    hud.rect(
        W * 0.5 - (W / 2) * easeOut(span(t, 0, 0.08)),
        H / 2 - 1.5 - open * 20,
        W * easeOut(span(t, 0, 0.08)),
        3 + open * 40,
        C.white,
        glow,
    );
}

function prom(hud: Hud, t: number) {
    const rows: { text: string; alpha: number }[] = [];
    for (const l of BOOT_LINES) {
        if (t < l.t) break;
        let text = l.text;
        if (l.typed) for (const k of l.typed) if (t >= k.t) text += k.ch;
        rows.push({ text, alpha: l.dim ? 0.6 : 1 });
    }

    // scroll so the last lines stay on screen
    const maxRows = 20;
    const scroll = Math.max(0, rows.length - maxRows);
    rows.slice(scroll).forEach((r, i) => {
        hud.text(r.text, X, Y + i * LH, {
            face: 'mono',
            size: SIZE,
            color: C.ice,
            alpha: r.alpha,
            glow: 8,
        });
    });

    // block cursor after the last row
    if (rows.length) {
        const last = rows[rows.length - 1];
        const cx = X + hud.measure(last.text, 'mono', SIZE);
        const on = Math.floor(t / (BEAT / 2)) % 2 === 0;
        if (on)
            hud.rect(
                cx + 2,
                Y + (rows.length - 1 - scroll) * LH - SIZE + 4,
                SIZE * 0.6,
                SIZE,
                C.ice,
                0.9,
            );
    }

    // vendor mark beside the banner, as consoles used to do
    const a = span(t, BOOT_LINES[0].t - 0.2, BOOT_LINES[0].t + 0.3);
    drawLogo(hud.ctx, 120, Y + 18, 104, C.ice, a * 0.95);
}

const LINE = ['In 1952,', 'a cipher', 'was a machine', 'you could hold.'];

function epigraph(hud: Hud, t: number) {
    const t0 = BOOT_CLEAR + BEAT;
    const end = sectionStart('cx52');
    const size = 76;
    const words = LINE.join(' ');
    const total = hud.measure(words, 'italic', size);
    let x = W / 2 - total / 2;
    LINE.forEach((w, i) => {
        const a = hold(t, t0 + i * BEAT * 1.5, end - 0.25, 0.5, 0.35);
        hud.text(w, x, H / 2 + 20 + (1 - a) * 12, {
            face: 'italic',
            size,
            color: C.ice,
            alpha: a,
            glow: 10,
        });
        x += hud.measure(w + ' ', 'italic', size);
    });
}

export const boot: SceneDef = {
    id: 'boot',
    sections: ['boot'],
    create() {
        return {
            update(f: Frame, hud: Hud) {
                const t = f.t;
                powerOn(hud, t);

                // the console clears to black before the epigraph
                const clear = span(t, BOOT_CLEAR - 0.15, BOOT_CLEAR + 0.1);
                if (clear < 1) {
                    hud.ctx.save();
                    hud.ctx.globalAlpha = 1 - clear;
                    prom(hud, t);
                    hud.ctx.restore();
                }
                if (t >= BOOT_CLEAR)
                    hud.rect(0, 0, W, H, C.ink, easeOut(span(t, BOOT_CLEAR, BOOT_CLEAR + 0.4)));
                epigraph(hud, t);

                const flicker = t < 0.6 ? 0.2 * Math.sin(t * 90) * (1 - t / 0.6) : 0;
                return {
                    post: {
                        curve: 1,
                        scan: 0.7,
                        aberr: 0.5,
                        grain: 0.45,
                        dither: 0.3,
                        bloom: 0.7,
                        threshold: 0.35,
                        vignette: 0.9,
                        flash:
                            Math.max(0, flicker) +
                            0.9 * Math.max(0, 1 - span(t, at(0) + 0.05, 0.5)) * (t > 0.05 ? 1 : 0),
                    },
                };
            },
        };
    },
};
