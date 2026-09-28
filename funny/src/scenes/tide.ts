// Tide: the sea again, lighter now. 768 buoys ride the waves, one per
// coefficient of a real ML-KEM-768 public key. Then twenty tides, one
// a beat: the buoys take the bits of a ChaCha20 block, round by round.

import * as THREE from 'three';
import { captions } from '../engine/captions.ts';
import type { Hud } from '../engine/hud.ts';
import { TIDE_LINES } from '../engine/lines.ts';
import { clamp, easeInOut, easeOut, smooth } from '../engine/math.ts';
import { at, BEAT, section } from '../engine/score.ts';
import { Sea, sky, waveHeight } from '../engine/sea.ts';
import type { Env, Frame, SceneDef } from '../engine/types.ts';
import { artifacts } from '../data/artifacts.ts';

const COLS = 32;
const ROWS = 24;
const N = COLS * ROWS;
const ROUNDS_AT = at(section('tide').bar + 2);

// ByteDecode12 of the t-hat part of an ML-KEM public key (FIPS 203)
function coefficients(pk: Uint8Array): number[] {
    const out: number[] = [];
    for (let i = 0; i < N * 1.5; i += 3)
        out.push(pk[i] | ((pk[i + 1] & 15) << 8), (pk[i + 1] >> 4) | (pk[i + 2] << 4));
    return out;
}

export const tide: SceneDef = {
    id: 'tide',
    sections: ['tide'],
    create(env: Env) {
        const a = artifacts();
        const coeff = coefficients(a.kem.pk);
        const rounds = a.trace.rounds;

        const scene = new THREE.Scene();
        scene.fog = new THREE.Fog('#a3adaf', 8, 110);
        sky(scene, env.renderer, '#5a6a72', '#b3bcbd', '#3f4c53');
        const water = new Sea(160, 200, '#52626a');
        scene.add(water.mesh);
        scene.add(new THREE.HemisphereLight('#b9c2c3', '#28333a', 1.3));

        const buoys = new THREE.InstancedMesh(
            new THREE.SphereGeometry(0.045, 10, 6),
            new THREE.MeshBasicMaterial({ color: '#ffffff' }),
            N,
        );
        scene.add(buoys);
        const grid = Array.from({ length: N }, (_, i) => {
            const cx = (i % COLS) - COLS / 2 + 0.5;
            const rz = Math.floor(i / COLS);
            return [cx * 0.62 + ((i * 7) % 5) * 0.03, -3 - rz * 1.05] as const;
        });

        const cam = new THREE.PerspectiveCamera(34, 16 / 9, 0.1, 1000);
        const m4 = new THREE.Matrix4();
        const col = new THREE.Color();
        const lo = new THREE.Color('#58666d');
        const hi = new THREE.Color('#eef1ee');

        return {
            update(f: Frame, _hud: Hud, cap: Hud) {
                const t = f.t;
                water.update(t);

                const beat = (t - ROUNDS_AT) / BEAT;
                const r = clamp(Math.floor(beat) + 1, 0, 20);
                const since = beat - Math.floor(beat);
                const mix = smooth((t - ROUNDS_AT) / BEAT);
                const rise = easeOut(clamp(f.local / 4));

                for (let i = 0; i < N; i++) {
                    const [x, z] = grid[i];
                    const y = waveHeight(x, z, t) + 0.03;
                    m4.makeTranslation(x, y, z);
                    buoys.setMatrixAt(i, m4);
                    let v = Math.pow(coeff[i] / 3329, 1.3);
                    if (i < 512 && mix > 0) {
                        const w = i >> 5;
                        const bit = (rounds[r][w] >>> (31 - (i & 31))) & 1;
                        const was = (rounds[Math.max(0, r - 1)][w] >>> (31 - (i & 31))) & 1;
                        const b = was + (bit - was) * easeOut(clamp(since / 0.5));
                        v = v * (1 - mix) + (0.15 + 0.85 * b) * mix;
                    }
                    col.copy(lo).lerp(hi, v * rise);
                    buoys.setColorAt(i, col);
                }
                buoys.instanceMatrix.needsUpdate = true;
                if (buoys.instanceColor) buoys.instanceColor.needsUpdate = true;

                const u = easeInOut(f.local / f.dur);
                cam.position.set(0, 1.5 + waveHeight(0, 5.7, t) * 0.12 + u * 0.6, 5.7 - u * 1.8);
                cam.lookAt(0, 0.6 - u * 0.2, -30);
                cam.updateMatrixWorld();

                captions(cap, t, TIDE_LINES);
                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.25,
                        threshold: 0.7,
                        grain: 0.35,
                        vignette: 0.5,
                        fade: smooth((f.local - f.dur + 0.8) / 0.8),
                    },
                };
            },
        };
    },
};
