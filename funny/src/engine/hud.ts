// The 2D layer: text and workstation chrome drawn with Canvas2D on a
// fixed 1920x1080 grid, then composited over the 3D render.

import { C, MOTIF } from './palette.ts';

export const W = 1920;
export const H = 1080;

export type Face = 'serif' | 'italic' | 'sans' | 'sansBold' | 'mono' | 'monoBold';

const FAMILY: Record<Face, string> = {
    serif: '400 {s}px "EB Garamond"',
    italic: 'italic 400 {s}px "EB Garamond"',
    sans: '400 {s}px "FreeSans"',
    sansBold: '700 {s}px "FreeSans"',
    mono: '400 {s}px "FreeMono"',
    monoBold: '700 {s}px "FreeMono"',
};

export interface TextOpts {
    face?: Face;
    size?: number;
    color?: string;
    alpha?: number;
    align?: CanvasTextAlign;
    baseline?: CanvasTextBaseline;
    spacing?: number;
    glow?: number;
    weight?: number;
}

export class Hud {
    canvas: HTMLCanvasElement;
    ctx: CanvasRenderingContext2D;
    scale: number;
    dirty = true;

    constructor(scale: number) {
        this.scale = scale;
        this.canvas = document.createElement('canvas');
        this.canvas.width = W * scale;
        this.canvas.height = H * scale;
        const ctx = this.canvas.getContext('2d');
        if (!ctx) throw new Error('no 2d context');
        this.ctx = ctx;
    }

    clear() {
        const c = this.ctx;
        c.setTransform(1, 0, 0, 1, 0, 0);
        c.clearRect(0, 0, this.canvas.width, this.canvas.height);
        c.setTransform(this.scale, 0, 0, this.scale, 0, 0);
        c.globalAlpha = 1;
        c.shadowBlur = 0;
        this.dirty = true;
    }

    font(face: Face, size: number, weight?: number) {
        let f = FAMILY[face].replace('{s}', String(size));
        if (weight) f = f.replace(/\b[47]00\b/, String(weight));
        this.ctx.font = f;
    }

    text(s: string, x: number, y: number, o: TextOpts = {}) {
        const c = this.ctx;
        c.save();
        this.font(o.face ?? 'sans', o.size ?? 24, o.weight);
        c.fillStyle = o.color ?? C.ice;
        c.globalAlpha = o.alpha ?? 1;
        c.textAlign = o.align ?? 'left';
        c.textBaseline = o.baseline ?? 'alphabetic';
        c.letterSpacing = `${o.spacing ?? 0}px`;
        if (o.glow) {
            c.shadowColor = c.fillStyle;
            c.shadowBlur = o.glow;
        }
        c.fillText(s, x, y);
        c.restore();
    }

    measure(s: string, face: Face, size: number, spacing = 0): number {
        const c = this.ctx;
        c.save();
        this.font(face, size);
        c.letterSpacing = `${spacing}px`;
        const w = c.measureText(s).width;
        c.restore();
        return w;
    }

    rect(x: number, y: number, w: number, h: number, color: string, alpha = 1) {
        const c = this.ctx;
        c.save();
        c.globalAlpha = alpha;
        c.fillStyle = color;
        c.fillRect(x, y, w, h);
        c.restore();
    }

    // Dark gradient behind text, fading out towards `from`
    scrim(from: number, to: number, alpha = 0.8, color = '3,6,11') {
        const c = this.ctx;
        const g = c.createLinearGradient(0, from, 0, to);
        g.addColorStop(0, `rgba(${color},0)`);
        g.addColorStop(1, `rgba(${color},${alpha})`);
        c.save();
        c.fillStyle = g;
        c.fillRect(0, Math.min(from, to), W, Math.abs(to - from));
        c.restore();
    }

    line(pts: [number, number][], color: string, width = 1, alpha = 1, dash: number[] = []) {
        const c = this.ctx;
        c.save();
        c.globalAlpha = alpha;
        c.strokeStyle = color;
        c.lineWidth = width;
        c.setLineDash(dash);
        c.beginPath();
        pts.forEach(([x, y], i) => (i ? c.lineTo(x, y) : c.moveTo(x, y)));
        c.stroke();
        c.restore();
    }

    // Motif-style bevel: light top-left, dark bottom-right
    bevel(x: number, y: number, w: number, h: number, raised: boolean, d = 2, alpha = 1) {
        const c = this.ctx;
        c.save();
        c.globalAlpha = alpha;
        const tl = raised ? MOTIF.light : MOTIF.dark;
        const br = raised ? MOTIF.dark : MOTIF.light;
        c.fillStyle = tl;
        c.fillRect(x, y, w, d);
        c.fillRect(x, y, d, h);
        c.fillStyle = br;
        c.fillRect(x, y + h - d, w, d);
        c.fillRect(x + w - d, y, d, h);
        c.restore();
    }

    // A window in the manner of a 1990s workstation window manager.
    // Returns the content rectangle.
    window(
        x: number,
        y: number,
        w: number,
        h: number,
        title: string,
        o: { alpha?: number; active?: boolean; well?: string } = {},
    ): { x: number; y: number; w: number; h: number } {
        const a = o.alpha ?? 1;
        const frame = 6;
        const tb = 30;
        this.rect(x, y, w, h, MOTIF.face, a);
        this.bevel(x, y, w, h, true, 2, a);
        this.bevel(x + frame - 2, y + frame - 2, w - 2 * frame + 4, h - 2 * frame + 4, false, 1, a);

        // Title bar with menu button left, iconify and maximize right
        const tx = x + frame;
        const ty = y + frame;
        const tw = w - 2 * frame;
        this.rect(tx, ty, tw, tb, o.active === false ? MOTIF.titleIdle : MOTIF.title, a);
        this.bevel(tx, ty, tb, tb, true, 2, a);
        this.rect(tx + 8, ty + 13, tb - 16, 4, MOTIF.text, a * 0.9);
        this.bevel(tx + tw - tb * 2, ty, tb, tb, true, 2, a);
        this.rect(tx + tw - tb * 2 + 12, ty + 12, 6, 6, MOTIF.text, a * 0.9);
        this.bevel(tx + tw - tb, ty, tb, tb, true, 2, a);
        this.bevel(tx + tw - tb + 7, ty + 7, tb - 14, tb - 14, true, 1, a);
        this.bevel(tx + tb, ty, tw - tb * 3, tb, true, 1, a);
        this.text(title, tx + tw / 2 - tb / 2, ty + tb / 2 + 1, {
            face: 'sans',
            size: 17,
            color: MOTIF.text,
            align: 'center',
            baseline: 'middle',
            alpha: a,
        });

        const cx = x + frame + 2;
        const cy = y + frame + tb + 4;
        const cw = w - 2 * frame - 4;
        const ch = h - 2 * frame - tb - 6;
        this.rect(cx, cy, cw, ch, o.well ?? MOTIF.well, a);
        this.bevel(cx - 2, cy - 2, cw + 4, ch + 4, false, 2, a);
        return { x: cx, y: cy, w: cw, h: ch };
    }

    // Hex dump of bytes in rows, optionally revealing only the first part
    hexBlock(
        bytes: Uint8Array,
        x: number,
        y: number,
        o: {
            cols: number;
            rows?: number;
            size?: number;
            color?: string;
            alpha?: number;
            reveal?: number;
            lineHeight?: number;
            offset?: number;
        },
    ) {
        const size = o.size ?? 16;
        const lh = o.lineHeight ?? size * 1.25;
        const rows = o.rows ?? Math.ceil(bytes.length / o.cols);
        const shown = Math.floor((o.reveal ?? 1) * rows * o.cols);
        const off = o.offset ?? 0;
        for (let r = 0; r < rows; r++) {
            let s = '';
            for (let k = 0; k < o.cols; k++) {
                const i = r * o.cols + k;
                if (i >= shown) break;
                const b = bytes[(i + off) % bytes.length];
                s += b.toString(16).padStart(2, '0');
            }
            if (!s) break;
            this.text(s, x, y + r * lh, {
                face: 'mono',
                size,
                color: o.color ?? C.fog,
                alpha: o.alpha ?? 1,
            });
        }
    }
}
