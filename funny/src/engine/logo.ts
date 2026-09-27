// The Salmonization mark: U+1F41F cut from Noto Sans Symbols 2

import svg from '../assets/salmonization.svg?raw';

const d = /\sd="([^"]+)"/.exec(svg)?.[1] ?? '';
const vb = (/viewBox="([^"]+)"/.exec(svg)?.[1] ?? '0 0 1 1').split(/\s+/).map(Number);

export const LOGO_W = vb[2];
export const LOGO_H = vb[3];

let path: Path2D | null = null;

// Draw the fish with its left edge at x, vertically centered on y
export function drawLogo(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    height: number,
    color: string,
    alpha = 1,
) {
    path ??= new Path2D(d);
    const s = height / LOGO_H;
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.translate(x, y - height / 2);
    ctx.scale(s, s);
    ctx.fillStyle = color;
    ctx.fill(path);
    ctx.restore();
}
