// ChaCha20-Poly1305: the real 4x4 state of one block, round by round
// on the beat, while its 512 bits churn as a slab below. Then the
// keystream meets a line of the letter and a tag seals it.

import * as THREE from 'three';
import { fly, type Key } from '../engine/camera.ts';
import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { clamp, easeOut, hold, span } from '../engine/math.ts';
import { C, MOTIF } from '../engine/palette.ts';
import { kickEnv, STEP } from '../engine/score.ts';
import type { Frame, SceneDef } from '../engine/types.ts';
import { artifacts, hex } from '../data/artifacts.ts';
import { COLUMNS, DIAGONALS } from '../data/chacha.ts';

const ROLE_COLOR = [C.fog, C.phosphor, C.phosphor, C.frost];
const ROLE = ['constant', 'key', 'key', 'counter · nonce'];

const QR = [
    'a += b;  d ^= a;  d <<<= 16;',
    'c += d;  b ^= c;  b <<<= 12;',
    'a += b;  d ^= a;  d <<<= 8;',
    'c += d;  b ^= c;  b <<<= 7;',
];

const KEYS: Key[] = [
    { bar: 0, pos: [0, 7.5, 5.5], look: [0, 0, 0.3], fov: 40 },
    { bar: 3, pos: [3.5, 5.5, 5.8], look: [0, 0, 0], fov: 38 },
    { bar: 5.5, pos: [-4, 4.2, 5.5], look: [0, 0, 0], fov: 36 },
    { bar: 8, pos: [-1, 9.5, 2.5], look: [0, 0, 0], fov: 36 },
];

const w32 = (x: number) => x.toString(16).padStart(8, '0');

export const chacha: SceneDef = {
    id: 'chacha',
    sections: ['chacha'],
    create() {
        const a = artifacts();
        const rounds = a.trace.rounds;
        const scene = new THREE.Scene();
        scene.background = new THREE.Color('#040a13');
        scene.fog = new THREE.FogExp2('#040a13', 0.05);
        const cam = new THREE.PerspectiveCamera(40, 16 / 9, 0.05, 100);

        // 512 bits: 4x4 words, each word an 8x4 tile of bits
        const S = 0.16;
        const mesh = new THREE.InstancedMesh(
            new THREE.BoxGeometry(S * 0.84, 1, S * 0.84),
            new THREE.MeshStandardMaterial({ color: '#ffffff', roughness: 0.4, metalness: 0.2 }),
            512,
        );
        mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
        scene.add(mesh);
        const cellPos = (word: number, bit: number): [number, number] => {
            const wr = Math.floor(word / 4);
            const wc = word % 4;
            const br = Math.floor(bit / 8);
            const bc = bit % 8;
            return [(wc * 9 + bc - 17.5) * S, (wr * 5 + br - 9.5) * S];
        };
        const frame = new THREE.Mesh(
            new THREE.PlaneGeometry(38 * S, 22 * S),
            new THREE.MeshStandardMaterial({ color: '#0a1624', roughness: 0.8 }),
        );
        frame.rotation.x = -Math.PI / 2;
        frame.position.y = -0.01;
        scene.add(frame);
        scene.add(new THREE.HemisphereLight('#3a5c86', '#05080c', 1.2));
        const key = new THREE.DirectionalLight('#dcecff', 2.2);
        key.position.set(-3, 6, 4);
        scene.add(key);

        const m4 = new THREE.Matrix4();
        const col = new THREE.Color();
        const on = new THREE.Color(C.phosphor);
        const off = new THREE.Color('#16263a');
        const hot = new THREE.Color('#ffffff');

        return {
            update(f: Frame, hud: Hud) {
                const beat = f.beat;
                const round = Math.min(20, Math.max(0, Math.floor(beat) + 1));
                const since = beat - Math.floor(beat);
                const cur = rounds[round];
                const prev = rounds[Math.max(0, round - 1)];
                const kick = kickEnv(f.t, 0.2);
                const diag = round % 2 === 0;
                const groups = diag ? DIAGONALS : COLUMNS;
                const active = round >= 1 && round <= 20 && beat < 20;

                // bits rise and settle into the new round
                for (let w = 0; w < 16; w++)
                    for (let b = 0; b < 32; b++) {
                        const i = w * 32 + b;
                        const bit = (cur[w] >>> (31 - b)) & 1;
                        const was = (prev[w] >>> (31 - b)) & 1;
                        const settle = easeOut(clamp((since - (b / 32) * 0.3) / 0.35));
                        const h0 = was ? 0.5 : 0.04;
                        const h1 = bit ? 0.5 : 0.04;
                        const h = (h0 + (h1 - h0) * settle) * (1 + kick * 0.3);
                        const [x, z] = cellPos(w, b);
                        m4.makeScale(1, h, 1).setPosition(x, h / 2, z);
                        mesh.setMatrixAt(i, m4);
                        col.copy(bit ? on : off);
                        if (bit !== was && active) col.lerp(hot, 1 - settle);
                        mesh.setColorAt(i, col);
                    }
                mesh.instanceMatrix.needsUpdate = true;
                if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;

                fly(cam, KEYS, f.bar, 0.04, f.t);
                drawHud(hud, f, round, since, groups);

                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.5 + 0.2 * kick,
                        threshold: 0.55,
                        scan: 0.15,
                        aberr: 0.4 + kick * 0.4,
                        grain: 0.3,
                        dither: 0.15,
                        vignette: 0.9,
                        flash: Math.exp(-f.local / 0.25) * 0.4,
                    },
                };
            },
        };

        function drawHud(hud: Hud, f: Frame, round: number, since: number, groups: number[][]) {
            const bar = f.bar;
            const stateA = hold(bar, 0.1, 5.15, 0.3, 0.3);
            hud.scrim(0, 360, 0.75, '3,6,11');
            hud.text('CHACHA20-POLY1305', 120, 130, {
                face: 'sansBold',
                size: 26,
                spacing: 10,
                alpha: hold(bar, 0.1, 7.9, 0.3, 0.3),
            });

            if (stateA > 0) {
                const inner = hud.window(110, 170, 560, 330, 'chacha20 · state', { alpha: stateA });
                const quarter = Math.floor(since / 0.25);
                const hl = round >= 1 && f.beat < 20 ? groups[quarter] : [];
                const cur = rounds[round];
                for (let w = 0; w < 16; w++) {
                    const r = Math.floor(w / 4);
                    const c = w % 4;
                    const x = inner.x + 26 + c * 130;
                    const y = inner.y + 60 + r * 62;
                    if (hl.includes(w)) hud.rect(x - 8, y - 30, 124, 42, C.indigo, stateA * 0.7);
                    hud.text(w32(cur[w]), x, y, {
                        face: 'monoBold',
                        size: 24,
                        color: hl.includes(w) ? C.white : ROLE_COLOR[r],
                        alpha: stateA,
                    });
                }
                hud.text(ROLE.join('   ·   '), 110, 540, {
                    face: 'sans',
                    size: 17,
                    spacing: 2,
                    color: C.fog,
                    alpha: stateA * 0.9,
                });

                // round counter
                const label =
                    round === 0
                        ? 'initial state'
                        : round % 2 === 1
                          ? 'column round'
                          : 'diagonal round';
                hud.text(`round ${String(round).padStart(2, '0')} / 20`, W - 120, 150, {
                    face: 'monoBold',
                    size: 44,
                    align: 'right',
                    color: C.ice,
                    alpha: stateA,
                    glow: 8,
                });
                hud.text(label, W - 120, 190, {
                    face: 'sans',
                    size: 20,
                    spacing: 4,
                    align: 'right',
                    color: C.fog,
                    alpha: stateA,
                });

                // the quarter round, current line lit per sixteenth
                const qr = hud.window(W - 110 - 520, 230, 520, 210, 'quarter round', {
                    alpha: stateA,
                });
                const line = Math.floor((f.local % (STEP * 4)) / STEP);
                QR.forEach((s, i) => {
                    hud.text(s, qr.x + 22, qr.y + 42 + i * 40, {
                        face: 'mono',
                        size: 22,
                        color: i === line && round >= 1 && f.beat < 20 ? C.phosphor : MOTIF.text,
                        alpha: stateA * (i === line ? 1 : 0.6),
                    });
                });
            }

            hud.scrim(H - 300, H, 0.85);
            hud.text('Add, rotate, xor. Twenty rounds, and the order is gone.', 120, H - 110, {
                face: 'italic',
                size: 42,
                alpha: hold(bar, 1, 5.1, 0.5, 0.3),
            });

            // keystream xor plaintext, then the tag
            const xA = hold(bar, 5.2, 8.1, 0.3, 0.25);
            if (xA > 0) {
                hud.rect(0, 0, W, H, C.ink, xA * 0.55);
                const x = 240;
                const y = 330;
                const n = a.plainLine.length;
                const ks = a.trace.keystream;
                const reveal = Math.floor(easeOut(span(bar, 5.4, 6.4)) * n);
                const cellW = 44;
                const row = (
                    label: string,
                    yy: number,
                    cells: string[],
                    color: string,
                    alpha: number,
                ) => {
                    hud.text(label, x - 30, yy, {
                        face: 'sans',
                        size: 18,
                        spacing: 3,
                        align: 'right',
                        color: C.fog,
                        alpha: xA * alpha,
                    });
                    cells.forEach((s, i) =>
                        hud.text(s, x + i * cellW + cellW / 2, yy, {
                            face: 'monoBold',
                            size: 22,
                            align: 'center',
                            color,
                            alpha: xA * alpha,
                        }),
                    );
                };
                const plain = a.plainLine.split('');
                const keyHex = [...ks.subarray(0, n)].map((b) => b.toString(16).padStart(2, '0'));
                const ctHex = [...a.cipherLine].map((b) => b.toString(16).padStart(2, '0'));
                row('PLAINTEXT', y, plain, C.ice, 1);
                row('KEYSTREAM', y + 60, keyHex.slice(0, Math.max(reveal, 0)), C.steel, 1);
                row('XOR', y + 120, ctHex.slice(0, reveal), C.phosphor, 1);
                hud.line(
                    [
                        [x, y + 84],
                        [x + n * cellW, y + 84],
                    ],
                    C.slate,
                    2,
                    xA,
                );

                const tagA = hold(bar, 6.6, 8.1, 0.3, 0.25);
                row('POLY1305 TAG', y + 230, hex(a.tagLine).match(/../g) ?? [], C.white, tagA);
                hud.text(
                    'A 256-bit key, a 96-bit nonce, and a 16-byte tag that nobody else can forge.',
                    W / 2,
                    H - 110,
                    {
                        face: 'italic',
                        size: 40,
                        align: 'center',
                        alpha: hold(bar, 6.8, 8.1, 0.4, 0.25),
                    },
                );
            }
        }
    },
};
