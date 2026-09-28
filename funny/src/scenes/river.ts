// A grey-green coast under a northern forest. Every river has its own
// mouth, and at the head of each a light: an instance. Small lights
// come in from the sea and swim upstream, each to its own river.

import * as THREE from 'three';
import { captions } from '../engine/captions.ts';
import type { Hud } from '../engine/hud.ts';
import { RIVER_LINES } from '../engine/lines.ts';
import { clamp, easeInOut, lerp, pulse, smooth, span } from '../engine/math.ts';
import { fbm } from '../engine/noise.ts';
import { mulberry32 } from '../engine/rng.ts';
import { BAR, PULSES } from '../engine/score.ts';
import { Sea, sky } from '../engine/sea.ts';
import type { Env, Frame, SceneDef } from '../engine/types.ts';

const coast = (x: number) => -2 + 5 * (fbm(x * 0.035, 0.5, 3, 9) - 0.5);

// Seven rivers, each a meander from its mouth inland
const RIVERS: THREE.Vector3[][] = [-26, -17, -9, -1, 7, 15, 24].map((x0, i) => {
    const pts: THREE.Vector3[] = [];
    const k = 0.18 + (i % 3) * 0.05;
    for (let s = -6; s <= 26; s += 1) {
        const x = x0 + 2.2 * Math.sin(s * k + i) * Math.min(1, Math.max(0, s) / 6);
        pts.push(new THREE.Vector3(x, 0.02, coast(x0) - s));
    }
    return pts;
});

function riverDist(x: number, z: number): number {
    let d = 1e9;
    for (const r of RIVERS)
        for (let i = 0; i < r.length - 1; i++) {
            const a = r[i];
            const b = r[i + 1];
            const abx = b.x - a.x;
            const abz = b.z - a.z;
            const t = clamp(((x - a.x) * abx + (z - a.z) * abz) / (abx * abx + abz * abz));
            d = Math.min(d, Math.hypot(x - a.x - abx * t, z - a.z - abz * t));
        }
    return d;
}

function height(x: number, z: number): number {
    const c = coast(x);
    const inland = c - z;
    let h =
        inland < 0
            ? -1
            : Math.min(1, inland / 3) * (0.4 + inland * 0.09) +
              2.2 * fbm(x * 0.07, z * 0.07, 4, 3) -
              0.5;
    const g = Math.exp(-Math.pow(riverDist(x, z) / 0.9, 2));
    h = h * (1 - g) - 0.35 * g;
    return h;
}

export const river: SceneDef = {
    id: 'river',
    sections: ['river'],
    create(env: Env) {
        const scene = new THREE.Scene();
        scene.fog = new THREE.Fog('#9ba7a5', 14, 70);
        sky(scene, env.renderer, '#5f6d70', '#a9b3b1', '#3c4845');
        scene.add(new THREE.HemisphereLight('#b3bdbb', '#27302c', 1.4));
        const sun = new THREE.DirectionalLight('#d4dcdb', 0.8);
        sun.position.set(-20, 25, 10);
        scene.add(sun);

        // land with vertex colours: wet moss low, lichen high
        const geo = new THREE.PlaneGeometry(90, 60, 270, 180);
        geo.rotateX(-Math.PI / 2);
        geo.translate(0, 0, -18);
        const pos = geo.getAttribute('position') as THREE.BufferAttribute;
        const colors: number[] = [];
        const moss = new THREE.Color('#4c5a51');
        const lichen = new THREE.Color('#8b988f');
        const sand = new THREE.Color('#7b8580');
        const tmp = new THREE.Color();
        for (let i = 0; i < pos.count; i++) {
            const x = pos.getX(i);
            const z = pos.getZ(i);
            const h = height(x, z);
            pos.setY(i, h);
            tmp.copy(h < 0.15 ? sand : moss).lerp(lichen, clamp((h - 0.8) / 2.5));
            colors.push(tmp.r, tmp.g, tmp.b);
        }
        geo.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3));
        geo.computeVertexNormals();
        scene.add(
            new THREE.Mesh(
                geo,
                new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.95 }),
            ),
        );

        // forest
        const r = mulberry32(64);
        const trees: THREE.Matrix4[] = [];
        for (let n = 0; n < 9000 && trees.length < 2600; n++) {
            const x = (r() - 0.5) * 90;
            const z = -2 - r() * 44;
            const h = height(x, z);
            if (h < 0.35 || riverDist(x, z) < 1.6) continue;
            const s = 0.5 + r() * 0.6;
            trees.push(
                new THREE.Matrix4()
                    .makeScale(s * 0.55, s * 1.4, s * 0.55)
                    .setPosition(x, h + s * 0.65, z),
            );
        }
        const forest = new THREE.InstancedMesh(
            new THREE.ConeGeometry(0.5, 1, 6),
            new THREE.MeshStandardMaterial({ color: '#2f3a34', roughness: 1 }),
            trees.length,
        );
        trees.forEach((m, i) => forest.setMatrixAt(i, m));
        scene.add(forest);

        const water = new Sea(160, 160, '#4f5f63');
        water.mesh.position.z = 10;
        scene.add(water.mesh);

        // a light at the head of each river, and travellers on the way
        const heads = new THREE.InstancedMesh(
            new THREE.SphereGeometry(0.16, 12, 8),
            new THREE.MeshBasicMaterial({ color: '#eef1ee', fog: false }),
            RIVERS.length,
        );
        scene.add(heads);
        const curves = RIVERS.map((p) => new THREE.CatmullRomCurve3(p));
        const trips = Array.from({ length: 16 }, (_, i) => ({
            river: (i * 3) % RIVERS.length,
            start: 0.3 + i * 0.32 + r() * 0.2,
            len: 2 + r() * 0.8,
        }));
        const travellers = new THREE.InstancedMesh(
            new THREE.SphereGeometry(0.09, 10, 6),
            new THREE.MeshBasicMaterial({ color: '#e7ebe8', fog: false }),
            trips.length,
        );
        scene.add(travellers);

        const cam = new THREE.PerspectiveCamera(36, 16 / 9, 0.1, 1000);
        const m4 = new THREE.Matrix4();

        return {
            update(f: Frame, _hud: Hud, cap: Hud) {
                const t = f.t;
                const bar = f.bar;
                water.update(t, 0.4);
                const beat = pulse(PULSES, t, 0.25);

                RIVERS.forEach((p, i) => {
                    const s = 1 + 0.35 * beat;
                    m4.makeScale(s, s, s).setPosition(
                        p[p.length - 1].x,
                        p[p.length - 1].y + 0.25,
                        p[p.length - 1].z,
                    );
                    heads.setMatrixAt(i, m4);
                });
                heads.instanceMatrix.needsUpdate = true;

                trips.forEach((tr, i) => {
                    const u = span(bar, tr.start, tr.start + tr.len);
                    const visible = u > 0 && u < 1;
                    const q = curves[tr.river].getPointAt(easeInOut(u));
                    const s = visible ? smooth(Math.min(u, 1 - u) * 8) : 0;
                    m4.makeScale(s, s, s).setPosition(q.x, q.y + 0.12, q.z);
                    travellers.setMatrixAt(i, m4);
                });
                travellers.instanceMatrix.needsUpdate = true;

                const u = easeInOut(f.local / f.dur);
                cam.position.set(lerp(-9, 8, u), lerp(15, 13.5, u), lerp(24, 22, u));
                cam.lookAt(lerp(-3, 4, u), 0, lerp(-10, -12, u));
                cam.updateMatrixWorld();

                captions(cap, t, RIVER_LINES);
                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.35,
                        threshold: 0.75,
                        grain: 0.35,
                        vignette: 0.55,
                        fade:
                            1 -
                            smooth(f.local / 1.2) +
                            smooth(span(f.local, f.dur - BAR * 0.25, f.dur)),
                    },
                };
            },
        };
    },
};
