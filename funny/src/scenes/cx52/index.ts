// CX-52, then Crypto AG. The machine at work in an empty museum; then
// the same machine, a lake, a globe of customers and a closed case,
// printed as an archive while the story is told one sentence at a time.

import * as THREE from 'three';
import { captions } from '../../engine/captions.ts';
import type { Hud } from '../../engine/hud.ts';
import { CRYPTOAG_LINES, CX52_LINES } from '../../engine/lines.ts';
import { clamp, countUpTo, easeInOut, easeOut, lerp, smooth, span } from '../../engine/math.ts';
import { fbm } from '../../engine/noise.ts';
import { hash1, mulberry32 } from '../../engine/rng.ts';
import { at, BAR, BEAT, CX_STEPS, section } from '../../engine/score.ts';
import { Sea, sky } from '../../engine/sea.ts';
import type { Env, Frame, SceneDef, Shot } from '../../engine/types.ts';
import { artifacts, BODY } from '../../data/artifacts.ts';
import { buildMachine, TAPE_CELL, WHEEL_PINS } from './model.ts';

const STEP_T = CX_STEPS.map((s) => s.t);
const LETTER_T = CX_STEPS.filter((s) => s.letter).map((s) => s.t);
const PLAIN = BODY.toUpperCase().replace(/[^A-Z]/g, '');

// Which wheels advance on each step: irregular, as the real stepping was
const WHEEL_COUNTS: number[][] = WHEEL_PINS.map((_, w) => {
    let n = 0;
    return STEP_T.map((_, e) => {
        if (CX_STEPS[e].letter ? hash1(e * 7 + w) < 0.8 : hash1(e * 13 + w * 31) < 0.25) n++;
        return n;
    });
});

// Cipher letters in groups of five, from the real ChaCha20 ciphertext
function cipherLetters(): string {
    const raw = atob(artifacts().encBody);
    let s = '';
    let n = 0;
    for (let i = 0; i < raw.length && s.length < 120; i++) {
        s += String.fromCharCode(65 + (raw.charCodeAt(i) % 26));
        if (++n % 5 === 0) s += ' ';
    }
    return s;
}
const tapePos = (k: number) => k + Math.floor(k / 5);

const cx = section('cx52').bar;
const ca = section('cryptoag').bar;

// Crypto AG shots, in bars from the start of the section
const SHOTS: [number, number, 'lake' | 'wheels' | 'closing' | 'globe' | 'night' | 'closed'][] = [
    [0, 2.15, 'lake'],
    [2.15, 4.15, 'wheels'],
    [4.15, 6.15, 'closing'],
    [6.15, 8.15, 'globe'],
    [8.15, 11.65, 'night'],
    [11.65, 14, 'closed'],
];

type V3 = [number, number, number];
function move(cam: THREE.PerspectiveCamera, u: number, a: V3, b: V3, la: V3, lb: V3, fov: number) {
    const k = easeInOut(u);
    cam.position.set(lerp(a[0], b[0], k), lerp(a[1], b[1], k), lerp(a[2], b[2], k));
    cam.lookAt(lerp(la[0], lb[0], k), lerp(la[1], lb[1], k), lerp(la[2], lb[2], k));
    if (cam.fov !== fov) {
        cam.fov = fov;
        cam.updateProjectionMatrix();
    }
    cam.updateMatrixWorld();
}

function museum(env: Env) {
    const scene = new THREE.Scene();
    scene.background = new THREE.Color('#1b2226');
    scene.fog = new THREE.Fog('#1b2226', 9, 24);
    const pmrem = new THREE.PMREMGenerator(env.renderer);
    const envScene = new THREE.Scene();
    envScene.background = new THREE.Color('#56636a');
    scene.environment = pmrem.fromScene(envScene).texture;
    scene.environmentIntensity = 0.35;
    env.renderer.shadowMap.enabled = true;
    env.renderer.shadowMap.type = THREE.PCFShadowMap;

    const m = buildMachine(cipherLetters());
    scene.add(m.root);
    const floor = new THREE.Mesh(
        new THREE.PlaneGeometry(80, 80),
        new THREE.MeshStandardMaterial({ color: '#2b3438', roughness: 0.85 }),
    );
    floor.rotation.x = -Math.PI / 2;
    floor.receiveShadow = true;
    scene.add(floor);

    // a high window to the left, a faint cold bounce from the right
    const key = new THREE.DirectionalLight('#dde4e6', 3.2);
    key.position.set(-5, 8, 4.5);
    key.castShadow = true;
    key.shadow.mapSize.set(2048, 2048);
    key.shadow.radius = 4;
    Object.assign(key.shadow.camera, { left: -5, right: 5, top: 5, bottom: -5 });
    key.shadow.bias = -0.0004;
    key.shadow.normalBias = 0.02;
    scene.add(key);
    const bounce = new THREE.DirectionalLight('#8fa0a8', 0.5);
    bounce.position.set(6, 3, -2);
    scene.add(bounce);
    scene.add(new THREE.HemisphereLight('#7f8e95', '#1b2226', 0.8));
    return { scene, m };
}

function lake(env: Env) {
    const scene = new THREE.Scene();
    scene.fog = new THREE.Fog('#98a3a6', 12, 150);
    sky(scene, env.renderer, '#5f6d74', '#a3adaf', '#46535a');
    const water = new Sea(200, 140, '#56656c');
    scene.add(water.mesh);
    scene.add(new THREE.HemisphereLight('#aab4b6', '#2a343a', 1.2));

    // ridges across the lake, fading into the mist
    [
        [34, '#4d5a60', 4.5, 1],
        [52, '#62707a', 7, 2],
        [75, '#768389', 10, 3],
    ].forEach(([dist, color, h, seed]) => {
        const pts: THREE.Vector2[] = [new THREE.Vector2(-160, -2)];
        for (let x = -160; x <= 160; x += 2)
            pts.push(
                new THREE.Vector2(x, (h as number) * (0.35 + fbm(x * 0.018, 0, 4, seed as number))),
            );
        pts.push(new THREE.Vector2(160, -2));
        const ridge = new THREE.Mesh(
            new THREE.ShapeGeometry(new THREE.Shape(pts)),
            new THREE.MeshBasicMaterial({ color: color as string, fog: true }),
        );
        ridge.position.z = -(dist as number);
        scene.add(ridge);
    });

    // one small light across the water, for the night shot
    const lamp = new THREE.Mesh(
        new THREE.SphereGeometry(0.32, 12, 8),
        new THREE.MeshBasicMaterial({ color: '#ffffff', fog: false }),
    );
    lamp.position.set(-5, 0.7, -30);
    scene.add(lamp);
    return { scene, water, lamp };
}

function globe() {
    const scene = new THREE.Scene();
    scene.background = new THREE.Color('#1c2428');
    const g = new THREE.Group();
    scene.add(g);
    const dots: number[] = [];
    for (let i = 0; i < 3000; i++) {
        const y = 1 - (i / 2999) * 2;
        const rr = Math.sqrt(1 - y * y);
        const th = i * 2.399963;
        dots.push(Math.cos(th) * rr * 2, y * 2, Math.sin(th) * rr * 2);
    }
    const dg = new THREE.BufferGeometry();
    dg.setAttribute('position', new THREE.Float32BufferAttribute(dots, 3));
    g.add(new THREE.Points(dg, new THREE.PointsMaterial({ color: '#5c696f', size: 0.025 })));
    const r = mulberry32(120);
    const lights = new THREE.InstancedMesh(
        new THREE.SphereGeometry(0.035, 8, 6),
        new THREE.MeshBasicMaterial({ color: '#e6ebe9' }),
        124,
    );
    const m = new THREE.Matrix4();
    for (let i = 0; i < 124; i++) {
        // countries cluster on land: bias towards a few bands
        const lat = (r() - 0.5) * 2.2 + (r() < 0.5 ? 0.5 : -0.2);
        const lon = r() * Math.PI * 2;
        const p = new THREE.Vector3(
            Math.cos(lat) * Math.cos(lon),
            Math.sin(lat),
            Math.cos(lat) * Math.sin(lon),
        ).multiplyScalar(2.01);
        m.makeTranslation(p.x, p.y, p.z);
        lights.setMatrixAt(i, m);
    }
    lights.count = 0;
    g.add(lights);
    return { scene, g, lights };
}

export const cx52: SceneDef = {
    id: 'cx52',
    sections: ['cx52', 'cryptoag'],
    create(env: Env) {
        const mus = museum(env);
        const lk = lake(env);
        const gl = globe();
        const m = mus.m;
        const cam = new THREE.PerspectiveCamera(30, 16 / 9, 0.05, 1000);

        // the machine's state at song time t
        function animate(t: number, caseClose: number) {
            const e = countUpTo(STEP_T, t) - 1;
            const snap = e >= 0 ? easeOut(clamp((t - STEP_T[e]) / 0.09), 3) : 0;
            m.wheels.forEach((g, w) => {
                const n = e >= 0 ? WHEEL_COUNTS[w][e] : 0;
                const prev = e >= 1 ? WHEEL_COUNTS[w][e - 1] : 0;
                g.rotation.x = -((prev + (n - prev) * snap) / WHEEL_PINS[w]) * Math.PI * 2;
            });
            const L = countUpTo(LETTER_T, t);
            const since = L > 0 ? t - LETTER_T[L - 1] : 99;
            const turn = L > 0 ? L - 1 + easeOut(clamp(since / (BEAT * 0.7)), 2) : 0;
            m.cage.rotation.x = -turn * Math.PI * 2;

            // the lever is pulled once per letter, then rests folded to close
            const pull = since < BEAT ? Math.sin(Math.PI * clamp(since / (BEAT * 0.8))) : 0;
            m.lever.rotation.x = -0.15 - 0.55 * pull - 1.3 * smooth(caseClose * 3);

            const letter = PLAIN.charCodeAt(Math.max(0, L - 1) % PLAIN.length) - 65;
            const prevL = PLAIN.charCodeAt(Math.max(0, L - 2) % PLAIN.length) - 65;
            m.indicator.rotation.z =
                -((prevL + (letter - prevL) * easeOut(clamp(since / 0.25))) / 26) * Math.PI * 2;

            const printed = L > 0 ? tapePos(L - 1) + 1 - (1 - easeOut(clamp(since / 0.15))) : 0;
            const cells = m.tapeLen / TAPE_CELL;
            m.tapeTex.repeat.set(-cells / m.tapeChars, 1);
            m.tapeTex.offset.set((printed + 6) / m.tapeChars, 0);

            // open lid rests back at about 94 degrees; closed is 0
            m.caseLid.rotation.x = -1.62 * (1 - easeInOut(caseClose));
        }

        function cx52Shot(f: Frame, cap: Hud): Shot {
            animate(f.t, 0);
            const u = f.local / (section('cx52').bars * BAR);
            move(cam, u, [-4.6, 2.5, 4.4], [2.9, 2.8, 5.4], [-0.5, 1.05, 0.2], [0.1, 1.1, 0.1], 30);
            captions(cap, f.t, CX52_LINES);
            return {
                scene: mus.scene,
                camera: cam,
                post: {
                    grain: 0.35,
                    vignette: 0.7,
                    white: 1 - smooth(f.local / 2.2),
                    fade: smooth(span(f.bar, section('cx52').bars - 0.35, section('cx52').bars)),
                },
            };
        }

        function archive(f: Frame, cap: Hud): Shot {
            const bar = f.bar - section('cx52').bars;
            const t = f.t;
            const [s0, s1, kind] =
                SHOTS.find(([a, b]) => bar >= a && bar < b) ?? SHOTS[SHOTS.length - 1];
            const u = (bar - s0) / (s1 - s0);
            const edge = Math.min(smooth((bar - s0) / 0.18), smooth((s1 - bar) / 0.18));
            let scene: THREE.Scene = mus.scene;

            if (kind === 'lake' || kind === 'night') {
                lk.water.update(t, 0.35);
                const night = kind === 'night';
                move(
                    cam,
                    u,
                    [0, 1.3, 8],
                    [0, 1.25, 7.2],
                    [0, 1.5, -60],
                    [0, 1.45, -60],
                    night ? 26 : 32,
                );
                lk.lamp.visible = night && t < at(ca + 10.3);
                scene = lk.scene;
            } else if (kind === 'globe') {
                gl.g.rotation.y = t * 0.05;
                gl.g.rotation.x = 0.35;
                gl.lights.count = Math.round(124 * easeOut(clamp(u * 1.25)));
                move(cam, u, [0, 0.4, 7.5], [0, 0.3, 6.6], [0, 0, 0], [0, 0, 0], 38);
                scene = gl.scene;
            } else {
                const closing = kind === 'closing' ? span(u, 0.2, 1) : kind === 'closed' ? 1 : 0;
                animate(kind === 'wheels' ? t : at(cx + 6), closing);
                if (kind === 'wheels')
                    move(
                        cam,
                        u,
                        [1.6, 1.55, 2.4],
                        [1.2, 1.5, 2.1],
                        [0.1, 1.0, 0.3],
                        [0.0, 1.0, 0.3],
                        30,
                    );
                else if (kind === 'closing')
                    move(cam, u, [-3.8, 3.4, 6.2], [-4.2, 3.6, 6.8], [0, 1.1, 0], [0, 1.0, 0], 30);
                else
                    move(cam, u, [-5.2, 3.0, 7.4], [-5.3, 3.05, 7.6], [0, 0.9, 0], [0, 0.9, 0], 28);
                scene = mus.scene;
            }

            captions(cap, t, CRYPTOAG_LINES);
            return {
                scene,
                camera: cam,
                post: {
                    duo: smooth(bar / 0.4),
                    levels: 4,
                    cell: 3,
                    grain: 0.25,
                    vignette: 0.6,
                    fade: 1 - edge * (kind === 'night' ? 0.7 : 1),
                },
            };
        }

        return {
            update(f: Frame, _hud: Hud, cap: Hud) {
                return f.section === 'cx52' ? cx52Shot(f, cap) : archive(f, cap);
            },
        };
    },
};
