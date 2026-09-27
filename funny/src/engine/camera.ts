// Camera moves as Catmull-Rom splines through keyframes in bars

import * as THREE from 'three';

export interface Key {
    bar: number;
    pos: [number, number, number];
    look: [number, number, number];
    fov?: number;
}

function cr(p0: number, p1: number, p2: number, p3: number, t: number): number {
    const t2 = t * t;
    const t3 = t2 * t;
    return (
        0.5 *
        (2 * p1 +
            (-p0 + p2) * t +
            (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
            (-p0 + 3 * p1 - 3 * p2 + p3) * t3)
    );
}

function sample(keys: Key[], bar: number, get: (k: Key) => number[]): number[] {
    if (bar <= keys[0].bar) return get(keys[0]);
    const last = keys.length - 1;
    if (bar >= keys[last].bar) return get(keys[last]);
    let i = 0;
    while (keys[i + 1].bar < bar) i++;
    const u = (bar - keys[i].bar) / (keys[i + 1].bar - keys[i].bar);
    const a = get(keys[Math.max(0, i - 1)]);
    const b = get(keys[i]);
    const c = get(keys[i + 1]);
    const d = get(keys[Math.min(last, i + 2)]);
    return b.map((_, j) => cr(a[j], b[j], c[j], d[j], u));
}

export function fly(cam: THREE.PerspectiveCamera, keys: Key[], bar: number, drift = 0, t = 0) {
    const p = sample(keys, bar, (k) => k.pos);
    const l = sample(keys, bar, (k) => k.look);
    const f = sample(keys, bar, (k) => [k.fov ?? 35])[0];
    cam.position.set(
        p[0] + drift * Math.sin(t * 0.37),
        p[1] + drift * 0.6 * Math.sin(t * 0.51 + 1.3),
        p[2] + drift * Math.sin(t * 0.29 + 2.1),
    );
    cam.lookAt(l[0], l[1], l[2]);
    if (cam.fov !== f) {
        cam.fov = f;
        cam.updateProjectionMatrix();
    }
    cam.updateMatrixWorld();
}

// Project a world point to HUD coordinates (1920x1080)
export function toScreen(cam: THREE.Camera, p: THREE.Vector3): [number, number, boolean] {
    const v = p.clone().project(cam);
    return [(v.x * 0.5 + 0.5) * 1920, (-v.y * 0.5 + 0.5) * 1080, v.z < 1];
}
