// The Salmonization mark: U+1F41F cut from Noto Sans Symbols 2

import * as THREE from 'three';
import { SVGLoader } from 'three/addons/loaders/SVGLoader.js';
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

// The mark as a flat shape, centred on the origin, `length` units long
export function logoShapes(length: number): THREE.Shape[] {
    const data = new SVGLoader().parse(svg.replace('currentColor', '#000'));
    const s = length / LOGO_W;
    const shapes: THREE.Shape[] = [];
    for (const p of data.paths) for (const sh of p.toShapes()) shapes.push(sh);
    const m = new THREE.Matrix3().set(s, 0, -length / 2, 0, -s, (LOGO_H * s) / 2, 0, 0, 1);
    return shapes.map((sh) => {
        const pts = sh.extractPoints(8);
        const out = new THREE.Shape(pts.shape.map((v) => v.clone().applyMatrix3(m)));
        out.holes = pts.holes.map((h) => new THREE.Path(h.map((v) => v.clone().applyMatrix3(m))));
        return out;
    });
}
