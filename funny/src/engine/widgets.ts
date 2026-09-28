// Pieces of the workstation desktop shared by more than one scene

import type { Hud } from './hud.ts';
import { H, W } from './hud.ts';
import { MOTIF } from './palette.ts';

export interface Rect {
    x: number;
    y: number;
    w: number;
    h: number;
}

// The root window: grey blue, with a faint weave
export function desktop(hud: Hud) {
    const c = hud.ctx;
    const g = c.createLinearGradient(0, 0, 0, H);
    g.addColorStop(0, '#4d5c66');
    g.addColorStop(1, '#2c373e');
    c.fillStyle = g;
    c.fillRect(0, 0, W, H);
    for (let x = -H; x < W; x += 24)
        hud.line(
            [
                [x, H],
                [x + H, 0],
            ],
            '#6d7d86',
            1,
            0.08,
        );
}

// An analogue clock, running from a given time of day
export function clock(hud: Hud, r: Rect, secs: number, alpha: number) {
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
    const hand = (frac: number, len: number, w: number) => {
        const a = frac * Math.PI * 2;
        c.lineWidth = w;
        c.beginPath();
        c.moveTo(cx, cy);
        c.lineTo(cx + Math.sin(a) * len, cy - Math.cos(a) * len);
        c.stroke();
    };
    hand((secs / 43200) % 1, rad * 0.5, 5);
    hand((secs / 3600) % 1, rad * 0.8, 3);
    c.strokeStyle = '#aebcc2';
    hand((Math.floor(secs) % 60) / 60, rad * 0.85, 1.5);
    c.restore();
}
