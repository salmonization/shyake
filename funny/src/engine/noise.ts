// Smooth 2D value noise and fractal sums, deterministic

import { hash1 } from './rng.ts';

function lattice(ix: number, iy: number, seed: number): number {
    return hash1(ix * 73856093 + iy * 19349663 + seed * 83492791);
}

export function noise2(x: number, y: number, seed = 0): number {
    const ix = Math.floor(x);
    const iy = Math.floor(y);
    const fx = x - ix;
    const fy = y - iy;
    const sx = fx * fx * (3 - 2 * fx);
    const sy = fy * fy * (3 - 2 * fy);
    const a = lattice(ix, iy, seed);
    const b = lattice(ix + 1, iy, seed);
    const c = lattice(ix, iy + 1, seed);
    const d = lattice(ix + 1, iy + 1, seed);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

export function fbm(x: number, y: number, octaves = 4, seed = 0): number {
    let sum = 0;
    let amp = 0.5;
    let f = 1;
    for (let i = 0; i < octaves; i++) {
        sum += amp * noise2(x * f, y * f, seed + i * 17);
        f *= 2;
        amp *= 0.5;
    }
    return sum;
}
