// CX-52, then Rubicon: the machine at work, then the same machine as
// an x-ray while the story of its vendor is told.

import * as THREE from 'three';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';
import { fly, toScreen, type Key } from '../../engine/camera.ts';
import type { Hud } from '../../engine/hud.ts';
import { H, W } from '../../engine/hud.ts';
import { clamp, countUpTo, easeOut, hold, smooth, span } from '../../engine/math.ts';
import { C } from '../../engine/palette.ts';
import { hash1 } from '../../engine/rng.ts';
import { BAR, BEAT, CX_STEPS, sectionStart } from '../../engine/score.ts';
import type { Env, Frame, SceneDef } from '../../engine/types.ts';
import { artifacts, BODY } from '../../data/artifacts.ts';
import { buildMachine, TAPE_CELL, TAPE_LEN, WHEEL_PINS } from './model.ts';

const STEP_T = CX_STEPS.map((s) => s.t);
const LETTER_T = CX_STEPS.filter((s) => s.letter).map((s) => s.t);

// Which wheels advance on each step: irregular, as the real stepping was
const WHEEL_COUNTS: number[][] = WHEEL_PINS.map((_, w) => {
    let n = 0;
    return STEP_T.map((_, e) => {
        const letter = CX_STEPS[e].letter;
        if (letter ? hash1(e * 7 + w) < 0.85 : hash1(e * 13 + w * 31) < 0.3) n++;
        return n;
    });
});

const PLAIN = BODY.toUpperCase().replace(/[^A-Z]/g, '');

// Cipher letters from the real ChaCha20 ciphertext bytes
function cipherLetters(): string {
    const raw = atob(artifacts().encBody);
    let s = '';
    for (let i = 0; i < raw.length && s.length < 110; i++) {
        s += String.fromCharCode(65 + (raw.charCodeAt(i) % 26));
        if (s.replace(/ /g, '').length % 5 === 0) s += ' ';
    }
    return s;
}

// Tape position of letter k, with a space after each group of five
const tapePos = (k: number) => k + Math.floor(k / 5);

const KEYS: Key[] = [
    { bar: 0, pos: [1.9, 1.25, 2.0], look: [0.5, 1.0, 0.2], fov: 30 },
    { bar: 2, pos: [1.2, 2.1, 3.1], look: [0.2, 1.0, 0.0], fov: 32 },
    { bar: 4.5, pos: [-2.9, 2.9, 4.3], look: [-0.2, 1.0, 0.0], fov: 30 },
    { bar: 6.5, pos: [-2.5, 3.3, 1.9], look: [-2.1, 1.95, 0.5], fov: 27 },
    { bar: 8, pos: [-0.6, 3.3, 4.6], look: [0.0, 1.0, 0.0], fov: 32 },
    { bar: 10, pos: [0.9, 4.4, 5.6], look: [0.1, 0.9, 0.0], fov: 30 },
    { bar: 12, pos: [0.4, 6.8, 3.2], look: [0.1, 0.8, 0.0], fov: 30 },
    { bar: 14, pos: [0.15, 8.6, 1.0], look: [0.1, 0.6, 0.0], fov: 28 },
];

function callout(
    hud: Hud,
    cam: THREE.Camera,
    p: THREE.Vector3,
    label: string,
    dx: number,
    dy: number,
    a: number,
) {
    if (a <= 0) return;
    const [x, y] = toScreen(cam, p);
    const ex = x + dx;
    const ey = y + dy;
    const grow = easeOut(a);
    hud.line(
        [
            [x, y],
            [x + (ex - x) * grow, y + (ey - y) * grow],
        ],
        C.ice,
        1.5,
        a,
    );
    hud.ctx.save();
    hud.ctx.globalAlpha = a;
    hud.ctx.strokeStyle = C.ice;
    hud.ctx.lineWidth = 1.5;
    hud.ctx.beginPath();
    hud.ctx.arc(x, y, 6, 0, Math.PI * 2);
    hud.ctx.stroke();
    hud.ctx.restore();
    const right = dx >= 0;
    hud.line(
        [
            [ex, ey],
            [ex + (right ? 1 : -1) * 30 * grow, ey],
        ],
        C.ice,
        1.5,
        a,
    );
    hud.text(label, ex + (right ? 40 : -40), ey + 6, {
        face: 'sansBold',
        size: 17,
        spacing: 3,
        color: C.ice,
        alpha: a,
        align: right ? 'left' : 'right',
    });
}

function blueprintGrid(hud: Hud, a: number) {
    if (a <= 0) return;
    for (let x = 0; x <= W; x += 48)
        hud.line(
            [
                [x, 0],
                [x, H],
            ],
            C.cyan,
            1,
            a * (x % 240 === 0 ? 0.14 : 0.05),
        );
    for (let y = 0; y <= H; y += 48)
        hud.line(
            [
                [0, y],
                [W, y],
            ],
            C.cyan,
            1,
            a * (y % 240 === 0 ? 0.14 : 0.05),
        );
}

export const cx52: SceneDef = {
    id: 'cx52',
    sections: ['cx52', 'rubicon'],
    create(env: Env) {
        const scene = new THREE.Scene();
        scene.background = new THREE.Color('#050b14');
        scene.fog = new THREE.FogExp2('#050b14', 0.045);
        const pmrem = new THREE.PMREMGenerator(env.renderer);
        scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
        scene.environmentIntensity = 0.35;
        env.renderer.shadowMap.enabled = true;
        env.renderer.shadowMap.type = THREE.PCFShadowMap;

        const cam = new THREE.PerspectiveCamera(30, 16 / 9, 0.05, 100);
        const m = buildMachine(cipherLetters());
        scene.add(m.root);

        const floor = new THREE.Mesh(
            new THREE.PlaneGeometry(60, 60),
            new THREE.MeshStandardMaterial({ color: '#0a1523', roughness: 0.5, metalness: 0.25 }),
        );
        floor.rotation.x = -Math.PI / 2;
        floor.receiveShadow = true;
        scene.add(floor);
        const fading = [...m.mats.all, floor.material];
        for (const mat of fading) mat.transparent = true;

        const key = new THREE.DirectionalLight('#d2e6ff', 3.2);
        key.position.set(-4, 7, 5);
        key.castShadow = true;
        key.shadow.mapSize.set(2048, 2048);
        key.shadow.camera.left = -4;
        key.shadow.camera.right = 4;
        key.shadow.camera.top = 4;
        key.shadow.camera.bottom = -4;
        key.shadow.bias = -0.0004;
        key.shadow.normalBias = 0.02;
        scene.add(key);
        const rim = new THREE.SpotLight('#4fc8ff', 60, 20, 0.6, 0.5, 1.5);
        rim.position.set(4, 3.5, -4);
        rim.target.position.set(0, 1, 0);
        scene.add(rim, rim.target);
        const fill = new THREE.HemisphereLight('#28446a', '#04070b', 0.9);
        scene.add(fill);
        const sweep = new THREE.PointLight('#8fe9ff', 0, 6, 1.5);
        scene.add(sweep);

        const rubicon = sectionStart('rubicon');
        const tmp = new THREE.Vector3();

        return {
            update(f: Frame, hud: Hud) {
                const t = f.t;

                // Stepping state from the score
                const e = countUpTo(STEP_T, t) - 1;
                const sinceStep = e >= 0 ? t - STEP_T[e] : 0;
                const snap = easeOut(clamp(sinceStep / 0.07), 3);
                m.wheels.forEach((g, w) => {
                    const n = e >= 0 ? WHEEL_COUNTS[w][e] : 0;
                    const prev = e >= 1 ? WHEEL_COUNTS[w][e - 1] : 0;
                    const pos = prev + (n - prev) * snap;
                    g.rotation.x = -(pos / WHEEL_PINS[w]) * Math.PI * 2;
                });

                const L = countUpTo(LETTER_T, t);
                const sinceLetter = L > 0 ? t - LETTER_T[L - 1] : 0;
                const turn = L > 0 ? L - 1 + easeOut(clamp(sinceLetter / (BEAT * 0.6)), 2) : 0;
                m.cage.rotation.x = -turn * Math.PI * 2;
                m.crank.rotation.x = -turn * Math.PI * 2;

                // Dial points at the plaintext letter being enciphered
                const letter = PLAIN.charCodeAt((Math.max(0, L - 1) + 0) % PLAIN.length) - 65;
                const prevLetter = PLAIN.charCodeAt(Math.max(0, L - 2) % PLAIN.length) - 65;
                const dialTurn =
                    prevLetter + (letter - prevLetter) * easeOut(clamp(sinceLetter / 0.2));
                m.dial.rotation.y = (dialTurn / 26) * Math.PI * 2;

                // Tape feeds one cell per printed letter
                const printed =
                    L > 0 ? tapePos(L - 1) + 1 - (1 - easeOut(clamp(sinceLetter / 0.12))) : 0;
                const cells = TAPE_LEN / TAPE_CELL;
                m.tapeTex.repeat.set(-cells / m.tapeChars, 1);
                m.tapeTex.offset.set(printed / m.tapeChars, 0);

                // Light: rise from darkness, a cold sweep across the wheels
                const reveal = smooth(f.bar / 1.5);
                key.intensity = 3.2 * reveal;
                rim.intensity = 60 * (0.3 + 0.7 * reveal);
                sweep.position.set(-2 + ((f.bar * 0.5) % 1) * 5, 1.9, 1.2);
                sweep.intensity = 0.7 * hold(f.bar, 0.5, 6, 1, 1);

                // Rubicon: fade solids to an x-ray
                const xr = smooth(span(t, rubicon - BEAT, rubicon + BAR));
                for (const mat of fading) mat.opacity = 1 - 0.93 * xr;
                m.mats.edges.opacity = 0.85 * xr;
                scene.background = new THREE.Color('#050b14').lerp(new THREE.Color('#061426'), xr);
                (scene.fog as THREE.FogExp2).density = 0.045 * (1 - xr * 0.8);

                fly(cam, KEYS, f.bar, 0.04, t);

                // HUD: caption, callouts, then the Rubicon text
                const bars = f.bar;
                const capA = hold(bars, 2.5, 9.6, 0.6, 0.4);
                if (capA > 0) {
                    hud.text('CX-52', 120, H - 150, {
                        face: 'sansBold',
                        size: 22,
                        spacing: 8,
                        alpha: capA,
                    });
                    hud.text('1952', 120 + hud.measure('CX-52', 'sansBold', 22, 8) + 24, H - 150, {
                        face: 'sans',
                        size: 22,
                        spacing: 8,
                        color: C.fog,
                        alpha: capA,
                    });
                    hud.text(
                        'Six pin wheels, a lug cage, and a key you could set by hand.',
                        120,
                        H - 104,
                        {
                            face: 'italic',
                            size: 40,
                            alpha: capA,
                        },
                    );
                }
                const co = (a: number, b: number) => hold(bars, a, b, 0.4, 0.3);
                callout(hud, cam, m.anchors.wheels, 'PIN WHEELS', 180, -140, co(8.1, 9.9));
                callout(hud, cam, tmp.copy(m.anchors.cage), 'LUG CAGE', 200, -60, co(8.3, 9.9));
                callout(hud, cam, m.anchors.dial, 'ALPHABET DIAL', -160, 120, co(8.5, 9.9));
                callout(hud, cam, m.anchors.printer, 'PRINTER', -140, -110, co(8.7, 9.9));
                callout(hud, cam, m.anchors.crank, 'CRANK', 120, 80, co(8.9, 9.9));

                blueprintGrid(hud, xr);
                const rb = (t - rubicon) / BAR;
                const lines: [number, string, 'italic' | 'sans', number, string][] = [
                    [0, 'The machine was sound.', 'italic', 64, C.ice],
                    [1, 'The vendor was not.', 'italic', 64, C.ice],
                    [
                        2,
                        'From 1970, Crypto AG was secretly owned by the CIA and the BND.',
                        'sans',
                        26,
                        C.fog,
                    ],
                    [
                        2.5,
                        'Operation Rubicon. Its machines were sold to more than a hundred countries.',
                        'sans',
                        26,
                        C.fog,
                    ],
                ];
                const y0 = 300;
                lines.forEach(([at, s, face, size, color], i) => {
                    const a = hold(rb, at, 3.55, 0.35, 0.3);
                    hud.text(s, 150, y0 + i * 84 + (i >= 2 ? 30 - (i - 2) * 40 : 0), {
                        face,
                        size,
                        color,
                        alpha: a,
                        glow: face === 'italic' ? 12 : 0,
                    });
                });
                const tr = hold(rb, 3, 4.2, 0.2, 0.2);
                if (tr > 0) {
                    hud.rect(0, 0, W, H, C.ink, 0.75 * tr);
                    hud.text('Trust the math, not the vendor.', W / 2, H / 2 + 24, {
                        face: 'italic',
                        size: 92,
                        align: 'center',
                        alpha: tr,
                        glow: 16,
                    });
                }

                const start = Math.exp(-f.local / 0.3) * 0.5;
                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.35 + 0.25 * xr,
                        threshold: 0.82 - 0.4 * xr,
                        scan: 0.1 + 0.25 * xr,
                        aberr: 0.35 + 0.3 * xr,
                        grain: 0.3,
                        dither: 0.1 + 0.15 * xr,
                        vignette: 0.85,
                        flash: start,
                        fade: smooth(span(t, rubicon + BAR * 3.75, rubicon + BAR * 4)),
                    },
                };
            },
        };
    },
};
