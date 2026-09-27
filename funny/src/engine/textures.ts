// Procedural textures drawn once with Canvas2D

import * as THREE from 'three';
import { mulberry32 } from './rng.ts';

export function canvas(w: number, h: number): [HTMLCanvasElement, CanvasRenderingContext2D] {
    const c = document.createElement('canvas');
    c.width = w;
    c.height = h;
    const ctx = c.getContext('2d');
    if (!ctx) throw new Error('no 2d context');
    return [c, ctx];
}

// Hammertone: overlapping soft dimples, used as bump and roughness
export function hammertone(seed: number, size = 1024): THREE.CanvasTexture {
    const [c, ctx] = canvas(size, size);
    const r = mulberry32(seed);
    ctx.fillStyle = '#808080';
    ctx.fillRect(0, 0, size, size);
    for (let i = 0; i < 5200; i++) {
        const x = r() * size;
        const y = r() * size;
        const rad = 4 + r() * 16;
        const g = ctx.createRadialGradient(x, y, 0, x, y, rad);
        const v = r() < 0.5 ? 0 : 255;
        g.addColorStop(0, `rgba(${v},${v},${v},0.16)`);
        g.addColorStop(1, `rgba(${v},${v},${v},0)`);
        ctx.fillStyle = g;
        for (const dx of [-size, 0, size])
            for (const dy of [-size, 0, size]) {
                ctx.save();
                ctx.translate(dx, dy);
                ctx.beginPath();
                ctx.arc(x, y, rad, 0, Math.PI * 2);
                ctx.fill();
                ctx.restore();
            }
    }
    const tex = new THREE.CanvasTexture(c);
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    return tex;
}

export function toTexture(c: HTMLCanvasElement, srgb = true): THREE.CanvasTexture {
    const t = new THREE.CanvasTexture(c);
    if (srgb) t.colorSpace = THREE.SRGBColorSpace;
    t.anisotropy = 8;
    return t;
}
