// A monitor in the mist, the only white thing in the world. It runs
// its self test, the way the old consoles did, and boots Shyake. We
// move in until the white is all there is.

import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { drawLogo } from '../engine/logo.ts';
import { easeInOut, smooth, span } from '../engine/math.ts';
import { BEAT, BAR } from '../engine/score.ts';
import { BOOT_LINES } from '../engine/script.ts';
import type { Frame, SceneDef } from '../engine/types.ts';

// The screen's own coordinates: a 1152x900 raster
const SW = 1152;
const SH = 900;
const INK = '#1d2326';
const SCREEN = '#eef0ec';

function screen(hud: Hud, t: number) {
    const c = hud.ctx;
    c.fillStyle = SCREEN;
    c.fillRect(0, 0, SW, SH);
    const x0 = 64;
    const bx = 250;
    let y = 90;
    const lh = 30;
    let bannerTop = -1;
    for (const l of BOOT_LINES) {
        if (t < l.t) break;
        let text = l.text;
        if (l.progress) {
            const [a, b] = l.progress;
            const head = text.slice(0, text.indexOf('...'));
            const dots = Math.floor(span(t, a, b) * 3);
            text = t < b ? head + '.'.repeat(dots) : text;
        }
        if (l.banner && bannerTop < 0) {
            bannerTop = y;
            y += 12;
        }
        if (!l.banner && bannerTop >= 0 && y < bannerTop + 150) y = bannerTop + 150;
        hud.text(text, l.banner ? bx : x0, y, { face: 'mono', size: 21, color: INK });
        y += lh;
        if (l.text.startsWith('Selftest') || l.progress || l.text.startsWith('Auto')) y += lh * 0.6;
    }
    if (bannerTop >= 0) drawLogo(c, x0, bannerTop + 36, 84, INK, 1);
    // cursor
    if (Math.floor(t / (BEAT / 2)) % 2 === 0) hud.rect(x0, y - 20, 12, 22, INK, 0.85);
}

export const boot: SceneDef = {
    id: 'boot',
    sections: ['boot'],
    create() {
        return {
            update(f: Frame, hud: Hud) {
                const c = hud.ctx;

                // mist behind the monitor
                const g = c.createLinearGradient(0, 0, 0, H);
                g.addColorStop(0, '#aab4b6');
                g.addColorStop(0.62, '#8e9a9e');
                g.addColorStop(1, '#6f7c82');
                c.fillStyle = g;
                c.fillRect(0, 0, W, H);

                // a slow push in, then through the glass into the white
                const near = easeInOut(span(f.bar, 0, 4.4)) * 0.45;
                const through = Math.pow(span(f.bar, 4.4, f.dur / BAR), 2.2) * 3;
                const k = 0.42 + near + through;
                const sw = SW * k;
                const sh = SH * k;
                const sx = W / 2 - sw / 2;
                const sy = H / 2 - sh / 2 - 20 * (1 - near * 2);

                // bezel and a soft shadow on the mist
                const bz = 46 * k;
                c.save();
                c.fillStyle = 'rgba(40,50,56,0.18)';
                c.filter = 'blur(30px)';
                c.fillRect(sx - bz, sy + sh + bz * 0.6, sw + bz * 2, 40 * k);
                c.restore();
                hud.rect(sx - bz, sy - bz, sw + bz * 2, sh + bz * 2.3, '#b9c0bf');
                hud.rect(sx - bz * 0.35, sy - bz * 0.35, sw + bz * 0.7, sh + bz * 0.7, '#8d9697');
                hud.text('SHYAKE', sx + sw / 2, sy + sh + bz * 1.45, {
                    face: 'sansBold',
                    size: 14 * k * 1.6,
                    spacing: 6 * k,
                    align: 'center',
                    color: '#6c7678',
                });

                c.save();
                c.translate(sx, sy);
                c.scale(k, k);
                c.beginPath();
                c.rect(0, 0, SW, SH);
                c.clip();
                screen(hud, f.t);
                c.restore();

                return {
                    post: {
                        grain: 0.35,
                        vignette: 0.45,
                        white: 1 - smooth(f.local / 1.2) + smooth(span(f.bar, 5.3, 6)),
                    },
                };
            },
        };
    },
};
