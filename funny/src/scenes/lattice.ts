// ML-KEM-768: where the pin wheels were, three rings of 256
// coefficients (decoded from a real public key) turn on the beat,
// fold into a noisy lattice, and collapse into a shared secret.

import * as THREE from 'three';
import { fly, type Key } from '../engine/camera.ts';
import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { clamp, countUpTo, easeInOut, easeOut, hold, smooth, span } from '../engine/math.ts';
import { C, MOTIF } from '../engine/palette.ts';
import { mulberry32 } from '../engine/rng.ts';
import { kickEnv, KICKS } from '../engine/score.ts';
import type { Frame, SceneDef } from '../engine/types.ts';
import { artifacts, hex } from '../data/artifacts.ts';

const K = 3;
const N = 256;
const Q = 3329;
const COUNT = K * N;
const RING_R = 1.7;
const RING_X = [-1.6, 0, 1.6];

// ByteDecode12 of the t-hat part of an ML-KEM public key (FIPS 203)
function coefficients(pk: Uint8Array): number[] {
    const out: number[] = [];
    for (let i = 0; i < K * N * 1.5; i += 3) {
        const b0 = pk[i];
        const b1 = pk[i + 1];
        const b2 = pk[i + 2];
        out.push(b0 | ((b1 & 0x0f) << 8), (b1 >> 4) | (b2 << 4));
    }
    return out;
}

// Centered binomial noise with eta = 2, as ML-KEM samples it
function cbd2(r: () => number): number {
    const bit = () => (r() < 0.5 ? 1 : 0);
    return bit() + bit() - bit() - bit();
}

const KEYS: Key[] = [
    { bar: 0, pos: [-4.2, 0.2, 0.3], look: [2, 0, 0], fov: 60 },
    { bar: 2, pos: [-4.5, 1.4, 3.6], look: [0, 0, 0], fov: 42 },
    { bar: 4.5, pos: [0.5, 2.4, 6.6], look: [0, 0, 0], fov: 36 },
    { bar: 6.5, pos: [4.6, 2.8, 5.0], look: [0, 0, 0], fov: 36 },
    { bar: 8.3, pos: [2.2, 1.2, 4.2], look: [0, 0, 0], fov: 38 },
    { bar: 10, pos: [0.6, 0.4, 2.6], look: [0, 0, 0], fov: 44 },
];

export const lattice: SceneDef = {
    id: 'lattice',
    sections: ['lattice'],
    create() {
        const a = artifacts();
        const coeff = coefficients(a.kem.pk);
        const scene = new THREE.Scene();
        scene.background = new THREE.Color('#040a13');
        scene.fog = new THREE.FogExp2('#040a13', 0.06);
        const cam = new THREE.PerspectiveCamera(50, 16 / 9, 0.05, 100);

        // One instance per coefficient: a radial bar that becomes a point
        const geo = new THREE.BoxGeometry(1, 1, 1);
        const mat = new THREE.MeshBasicMaterial({ color: '#ffffff', transparent: true });
        const mesh = new THREE.InstancedMesh(geo, mat, COUNT);
        mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
        scene.add(mesh);
        const col = new THREE.Color();
        const lo = new THREE.Color('#1d2c5a');
        const hi = new THREE.Color(C.phosphor);
        for (let i = 0; i < COUNT; i++) {
            col.copy(lo).lerp(hi, Math.pow(coeff[i] / Q, 1.4));
            mesh.setColorAt(i, col);
        }

        // Lattice positions on a skewed basis, and their noise
        const b1 = new THREE.Vector3(0.42, 0.06, 0.04);
        const b2 = new THREE.Vector3(0.12, 0.4, 0.05);
        const b3 = new THREE.Vector3(0.1, 0.09, 0.44);
        const grid: THREE.Vector3[] = [];
        const noise: THREE.Vector3[] = [];
        const r = mulberry32(768);
        for (let i = 0; i < 12; i++)
            for (let j = 0; j < 8; j++)
                for (let k = 0; k < 8; k++) {
                    grid.push(
                        new THREE.Vector3()
                            .addScaledVector(b1, i - 5.5)
                            .addScaledVector(b2, j - 3.5)
                            .addScaledVector(b3, k - 3.5),
                    );
                    noise.push(new THREE.Vector3(cbd2(r), cbd2(r), cbd2(r)).multiplyScalar(0.05));
                }

        // Basis vectors drawn from the origin
        const basis = new THREE.Group();
        for (const b of [b1, b2, b3]) {
            const g = new THREE.BufferGeometry().setFromPoints([
                new THREE.Vector3(),
                b.clone().multiplyScalar(2),
            ]);
            basis.add(
                new THREE.Line(g, new THREE.LineBasicMaterial({ color: C.ice, transparent: true })),
            );
        }
        scene.add(basis);

        // Ring guides
        const guides = RING_X.map((x) => {
            const c = new THREE.Mesh(
                new THREE.TorusGeometry(RING_R, 0.006, 6, 256),
                new THREE.MeshBasicMaterial({ color: C.steel, transparent: true }),
            );
            c.rotation.y = Math.PI / 2;
            c.position.x = x;
            scene.add(c);
            return c;
        });

        // Dust
        const dustGeo = new THREE.BufferGeometry();
        const dp: number[] = [];
        for (let i = 0; i < 1500; i++)
            dp.push((r() - 0.5) * 30, (r() - 0.5) * 16, (r() - 0.5) * 30);
        dustGeo.setAttribute('position', new THREE.Float32BufferAttribute(dp, 3));
        const dust = new THREE.Points(
            dustGeo,
            new THREE.PointsMaterial({
                color: C.steel,
                size: 0.02,
                transparent: true,
                opacity: 0.6,
            }),
        );
        scene.add(dust);

        const m4 = new THREE.Matrix4();
        const q = new THREE.Quaternion();
        const pos = new THREE.Vector3();
        const scl = new THREE.Vector3();
        const ringPos = new THREE.Vector3();
        const zAxis = new THREE.Vector3(0, 0, 1);
        const radial = new THREE.Vector3();
        const qRing = new THREE.Quaternion();

        return {
            update(f: Frame, hud: Hud) {
                const bar = f.bar;
                const t = f.t;
                const kick = kickEnv(t, 0.2);

                // ring turns stepped on the kick, like the wheels before
                const beats = countUpTo(KICKS, t);
                const since = beats > 0 ? t - KICKS[beats - 1] : 0;
                const stepped = beats - 1 + easeOut(clamp(since / 0.12));

                const morph = easeInOut(span(bar, 4.6, 6.2));
                const err = smooth(span(bar, 6.4, 7)) * (0.6 + 0.4 * kick);
                const collapse = easeInOut(span(bar, 8, 8.6));

                for (let i = 0; i < COUNT; i++) {
                    const ring = Math.floor(i / N);
                    const k = i % N;
                    const val = coeff[i] / Q;

                    // ring layout: bar radiating out of the ring
                    const ang = (k / N) * Math.PI * 2 + (stepped * (ring + 1) * Math.PI * 2) / 64;
                    radial.set(0, Math.cos(ang), Math.sin(ang));
                    const len = 0.05 + val * 0.9 * (1 + 0.25 * kick);
                    ringPos.set(RING_X[ring], 0, 0).addScaledVector(radial, RING_R + len / 2);
                    qRing.setFromUnitVectors(zAxis, radial);
                    q.copy(qRing);

                    // lattice layout: a point, jittered by its noise
                    const lp = grid[i].clone().addScaledVector(noise[i], err);
                    const stagger = clamp(morph * 1.6 - (k / N) * 0.6);
                    pos.lerpVectors(ringPos, lp, stagger);
                    pos.multiplyScalar(1 - collapse);
                    const thick = 0.022 + 0.03 * stagger;
                    scl.set(thick, thick, len + (0.045 - len) * stagger);
                    scl.multiplyScalar(1 - collapse * 0.7);
                    if (stagger > 0.999) q.identity();
                    m4.compose(pos, q, scl);
                    mesh.setMatrixAt(i, m4);
                }
                mesh.instanceMatrix.needsUpdate = true;
                mat.opacity = 1 - smooth(span(bar, 8.4, 8.9));

                for (const g of guides)
                    (g.material as THREE.MeshBasicMaterial).opacity =
                        0.5 * (1 - smooth(span(bar, 4.4, 5.4)));
                basis.children.forEach((l) => {
                    ((l as THREE.Line).material as THREE.LineBasicMaterial).opacity = hold(
                        bar,
                        6,
                        8.2,
                        0.4,
                        0.3,
                    );
                });
                dust.rotation.y = t * 0.02;

                fly(cam, KEYS, bar, 0.05, t);

                drawHud(hud, bar);

                const flash =
                    Math.exp(-f.local / 0.35) * 0.7 +
                    Math.exp(-Math.max(0, bar - 8.55) / 0.08) * (bar > 8.55 ? 0.8 : 0);
                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.55 + kick * 0.25,
                        threshold: 0.5,
                        scan: 0.12,
                        aberr: 0.5 + kick * 0.6,
                        grain: 0.3,
                        dither: 0.12,
                        vignette: 0.9,
                        flash,
                    },
                };
            },
        };

        function drawHud(hud: Hud, bar: number) {
            // Title and parameters
            const scA = hold(bar, 0.5, 7.9, 0.4, 0.4) * (1 - hold(bar, 4.8, 6.1, 0.2, 0.2));
            hud.scrim(H - 330, H, 0.85 * Math.max(scA, hold(bar, 6.5, 7.9, 0.3, 0.3)));
            hud.scrim(330, 0, 0.7 * hold(bar, 0.5, 7.9, 0.4, 0.3));
            const tA = hold(bar, 0.5, 4.7, 0.4, 0.4);
            hud.text('ML-KEM-768', 120, 150, {
                face: 'sansBold',
                size: 30,
                spacing: 10,
                alpha: tA,
            });
            hud.text('FIPS 203  ·  key encapsulation', 120, 190, {
                face: 'sans',
                size: 20,
                spacing: 3,
                color: C.fog,
                alpha: tA,
            });
            hud.text(
                'Where the wheels were: three rings of 256 coefficients, mod 3329.',
                120,
                H - 110,
                {
                    face: 'italic',
                    size: 42,
                    alpha: hold(bar, 1, 4.7, 0.5, 0.4),
                },
            );

            const pA = hold(bar, 1.6, 4.7, 0.3, 0.3);
            if (pA > 0) {
                const w = 380;
                const h = 360;
                const x = W - w - 110;
                const y = 150;
                const inner = hud.window(x, y, w, h, 'ml-kem-768', { alpha: pA * 0.95 });
                const rows: [string, string][] = [
                    ['k', '3'],
                    ['n', '256'],
                    ['q', '3329'],
                    ['η1, η2', '2, 2'],
                    ['du, dv', '10, 4'],
                    ['pk', '1184 bytes'],
                    ['ct', '1088 bytes'],
                    ['ss', '32 bytes'],
                ];
                const shown = Math.floor(span(bar, 1.7, 3.2) * rows.length + 0.001);
                rows.slice(0, shown).forEach(([k, v], i) => {
                    hud.text(k, inner.x + 24, inner.y + 44 + i * 36, {
                        face: 'mono',
                        size: 22,
                        color: C.fog,
                        alpha: pA,
                    });
                    hud.text(v, inner.x + 170, inner.y + 44 + i * 36, {
                        face: 'monoBold',
                        size: 22,
                        color: MOTIF.text,
                        alpha: pA,
                    });
                });
            }

            // Learning with errors
            const lA = hold(bar, 6.2, 7.9, 0.4, 0.3);
            hud.text('t = A s + e', W / 2, 190, {
                face: 'italic',
                size: 88,
                align: 'center',
                alpha: lA,
                glow: 14,
            });
            hud.text('A public   ·   s secret   ·   e small noise', W / 2, 250, {
                face: 'sans',
                size: 22,
                spacing: 4,
                align: 'center',
                color: C.fog,
                alpha: lA,
            });
            hud.text(
                'Easy with s. Without it, no known algorithm, classical or quantum, finds it.',
                W / 2,
                H - 110,
                {
                    face: 'italic',
                    size: 40,
                    align: 'center',
                    alpha: hold(bar, 6.6, 7.9, 0.4, 0.3),
                },
            );

            // Encapsulation to a shared secret
            const eA = hold(bar, 8.5, 10.2, 0.25, 0.2);
            if (eA > 0) {
                hud.hexBlock(a.kem.ct, 120, 150, {
                    cols: 48,
                    size: 15,
                    lineHeight: 20,
                    color: C.steel,
                    alpha: eA * 0.55,
                    reveal: easeOut(span(bar, 8.5, 9.3)),
                });
                hud.rect(0, H / 2 - 150, W, 300, C.ink, eA * 0.8);
                hud.text('Encaps(pk) → (ct, K)        Decaps(sk, ct) → K', W / 2, H / 2 - 70, {
                    face: 'italic',
                    size: 44,
                    align: 'center',
                    alpha: eA,
                });
                const ss = hex(a.kem.ss);
                const shown = Math.floor(easeOut(span(bar, 8.6, 9.2)) * 64);
                hud.text(ss.slice(0, shown), W / 2, H / 2 + 30, {
                    face: 'monoBold',
                    size: 38,
                    align: 'center',
                    color: C.phosphor,
                    alpha: eA,
                    glow: 16,
                });
                hud.text('K  ·  32 bytes, the same on both ends', W / 2, H / 2 + 90, {
                    face: 'sans',
                    size: 20,
                    spacing: 4,
                    align: 'center',
                    color: C.fog,
                    alpha: eA * hold(bar, 9, 10.2, 0.3, 0.2),
                });
            }
        }
    },
};
