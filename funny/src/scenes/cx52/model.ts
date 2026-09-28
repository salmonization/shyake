// A procedural CX-52, after photographs from several angles. Printer
// housing on the left with the letter dial on its front and the knobs
// on its end; six key wheels on a front axle, each with a guide arm
// in a slot of the tray; the brass cage behind them under a locking
// bar; the advance lever on the right frame; a hinged hood behind,
// and the case lid, which can close over everything.

import * as THREE from 'three';
import { RoundedBoxGeometry } from 'three/addons/geometries/RoundedBoxGeometry.js';
import { mulberry32 } from '../../engine/rng.ts';
import { canvas, hammertone, toTexture } from '../../engine/textures.ts';

export const WHEEL_PINS = [47, 43, 42, 41, 38, 37];
export const WHEEL_X = WHEEL_PINS.map((_, i) => -0.55 + i * 0.25);
export const WHEEL_Y = 0.92;
export const WHEEL_Z = 0.42;
export const WHEEL_R = 0.4;
export const CAGE_Y = 1.2;
export const CAGE_Z = -0.45;
export const CAGE_BARS = 27;
export const TAPE_CELL = 0.07;

export interface Machine {
    root: THREE.Group;
    wheels: THREE.Group[];
    cage: THREE.Group;
    lever: THREE.Group;
    indicator: THREE.Object3D;
    tapeTex: THREE.CanvasTexture;
    tapeChars: number;
    tapeLen: number;
    caseLid: THREE.Group;
    anchors: Record<string, THREE.Vector3>;
}

const ALPHA = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';

/* ------------------------------------------------------------------ */
/* Textures                                                           */
/* ------------------------------------------------------------------ */

function rimTexture(pins: number): THREE.CanvasTexture {
    const [c, ctx] = canvas(2048, 96);
    ctx.fillStyle = '#101214';
    ctx.fillRect(0, 0, 2048, 96);
    ctx.fillStyle = '#d4d8d6';
    ctx.font = '700 30px "FreeSans"';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const w = 2048 / pins;
    for (let i = 0; i < pins; i++) {
        ctx.save();
        ctx.translate(i * w + w / 2, 48);
        ctx.rotate(Math.PI / 2);
        ctx.fillText(String(i + 1).padStart(2, '0'), 0, 0);
        ctx.restore();
    }
    return toTexture(c);
}

function ringTexture(): THREE.CanvasTexture {
    const S = 1024;
    const m = S / 2;
    const [c, ctx] = canvas(S, S);
    ctx.fillStyle = '#6f7470';
    ctx.fillRect(0, 0, S, S);
    ctx.beginPath();
    ctx.arc(m, m, 505, 0, Math.PI * 2);
    ctx.fillStyle = '#131517';
    ctx.fill();
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (let i = 0; i < 26; i++) {
        const a = (i / 26) * Math.PI * 2 - Math.PI / 2;
        ctx.save();
        ctx.translate(m + Math.cos(a) * 440, m + Math.sin(a) * 440);
        ctx.rotate(a + Math.PI / 2);
        ctx.fillStyle = '#e1e3df';
        ctx.font = '700 70px "FreeSans"';
        ctx.fillText(ALPHA[i], 0, 0);
        ctx.restore();
        if (i < 10) {
            const b = ((i + 1) / 26) * Math.PI * 2 - Math.PI / 2;
            ctx.save();
            ctx.translate(m + Math.cos(b) * 345, m + Math.sin(b) * 345);
            ctx.rotate(b + Math.PI / 2);
            ctx.fillStyle = '#8c7775';
            ctx.font = '400 50px "FreeSans"';
            ctx.fillText(String((i + 1) % 10), 0, 0);
            ctx.restore();
        }
    }
    return toTexture(c);
}

function indicatorTexture(): THREE.CanvasTexture {
    const S = 512;
    const m = S / 2;
    const [c, ctx] = canvas(S, S);
    ctx.clearRect(0, 0, S, S);
    ctx.fillStyle = '#dfe1dd';
    ctx.beginPath();
    ctx.arc(m, m, 200, -Math.PI * 0.42, Math.PI * 1.42);
    ctx.lineTo(m, m - 250);
    ctx.closePath();
    ctx.fill();
    ctx.fillStyle = '#8c7775';
    ctx.fillRect(m - 3, m - 150, 6, 60);
    return toTexture(c);
}

function counterTexture(): THREE.CanvasTexture {
    const [c, ctx] = canvas(256, 96);
    ctx.fillStyle = '#16181a';
    ctx.fillRect(0, 0, 256, 96);
    ctx.fillStyle = '#d9d6c8';
    ctx.font = '700 64px "FreeMono"';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ['1', '2', '5'].forEach((d, i) => ctx.fillText(d, 60 + i * 68, 52));
    return toTexture(c);
}

function barTexture(): THREE.CanvasTexture {
    const [c, ctx] = canvas(1024, 48);
    ctx.fillStyle = '#b8bebe';
    ctx.fillRect(0, 0, 1024, 48);
    ctx.fillStyle = '#2a2e30';
    ctx.font = '700 30px "FreeSans"';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    WHEEL_X.forEach((x, i) => ctx.fillText(String(i + 1), ((x + 0.875) / 1.95) * 1024, 26));
    return toTexture(c);
}

function tapeTexture(text: string): [THREE.CanvasTexture, number] {
    const cell = 64;
    const [c, ctx] = canvas(text.length * cell, 128);
    ctx.fillStyle = '#e4e5df';
    ctx.fillRect(0, 0, c.width, 128);
    ctx.fillStyle = '#2b3236';
    ctx.font = '700 66px "FreeMono"';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (let i = 0; i < text.length; i++) ctx.fillText(text[i], i * cell + cell / 2, 68);
    const t = toTexture(c);
    t.wrapS = THREE.ClampToEdgeWrapping;
    return [t, text.length];
}

/* ------------------------------------------------------------------ */
/* Geometry helpers                                                   */
/* ------------------------------------------------------------------ */

// Extrude a side profile given as (z, y) points along +x from x0
function profile(
    pts: [number, number][],
    x0: number,
    depth: number,
    bevel = 0.03,
): THREE.BufferGeometry {
    const shape = new THREE.Shape(pts.map(([z, y]) => new THREE.Vector2(-z, y)));
    const g = new THREE.ExtrudeGeometry(shape, {
        depth: depth - bevel * 2,
        bevelEnabled: bevel > 0,
        bevelSize: bevel,
        bevelThickness: bevel,
        bevelSegments: 3,
        curveSegments: 12,
    });
    g.rotateY(Math.PI / 2);
    g.translate(x0 + bevel, 0, 0);
    g.computeVertexNormals();
    return g;
}

// A curved sheet: an arc in the y-z plane swept along x
function shell(
    x0: number,
    x1: number,
    cy: number,
    cz: number,
    r: number,
    a0: number,
    a1: number,
): THREE.BufferGeometry {
    const seg = 48;
    const pos: number[] = [];
    const idx: number[] = [];
    for (let i = 0; i <= seg; i++) {
        const a = a0 + ((a1 - a0) * i) / seg;
        const y = cy + r * Math.sin(a);
        const z = cz + r * Math.cos(a);
        pos.push(x0, y, z, x1, y, z);
        if (i < seg) idx.push(i * 2, i * 2 + 1, i * 2 + 2, i * 2 + 1, i * 2 + 3, i * 2 + 2);
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    g.setIndex(idx);
    g.computeVertexNormals();
    return g;
}

// A ribbon along a spline in the x-y plane, width along z
function ribbon(pts: THREE.Vector3[], width: number, segs: number): [THREE.BufferGeometry, number] {
    const curve = new THREE.CatmullRomCurve3(pts, false, 'centripetal');
    const len = curve.getLength();
    const pos: number[] = [];
    const uv: number[] = [];
    const idx: number[] = [];
    for (let i = 0; i <= segs; i++) {
        const u = i / segs;
        const p = curve.getPointAt(u);
        pos.push(p.x, p.y, p.z - width / 2, p.x, p.y, p.z + width / 2);
        uv.push(u, 1, u, 0);
        if (i < segs) idx.push(i * 2, i * 2 + 1, i * 2 + 2, i * 2 + 1, i * 2 + 3, i * 2 + 2);
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
    g.setIndex(idx);
    g.computeVertexNormals();
    return [g, len];
}

function mesh(
    g: THREE.BufferGeometry,
    m: THREE.Material | THREE.Material[],
    at?: [number, number, number],
): THREE.Mesh {
    const o = new THREE.Mesh(g, m);
    if (at) o.position.set(...at);
    o.castShadow = true;
    o.receiveShadow = true;
    return o;
}

// A cylinder whose axis runs along x
function xCyl(
    r: number,
    len: number,
    m: THREE.Material | THREE.Material[],
    at: [number, number, number],
    seg = 32,
) {
    const g = new THREE.CylinderGeometry(r, r, len, seg);
    g.rotateZ(Math.PI / 2);
    return mesh(g, m, at);
}

// A cylinder whose axis runs along z
function zCyl(
    r: number,
    len: number,
    m: THREE.Material | THREE.Material[],
    at: [number, number, number],
    seg = 48,
) {
    const g = new THREE.CylinderGeometry(r, r, len, seg);
    g.rotateX(Math.PI / 2);
    return mesh(g, m, at);
}

/* ------------------------------------------------------------------ */
/* Assembly                                                           */
/* ------------------------------------------------------------------ */

export function buildMachine(cipherText: string): Machine {
    const bump = hammertone(1952);
    bump.repeat.set(1.5, 1.5);
    const body = new THREE.MeshPhysicalMaterial({
        color: '#6d726e',
        roughness: 0.62,
        metalness: 0.3,
        bumpMap: bump,
        bumpScale: 0.9,
        clearcoat: 0.12,
        clearcoatRoughness: 0.6,
    });
    const bodyDark = body.clone();
    bodyDark.color.set('#5d625f');
    bodyDark.side = THREE.DoubleSide;
    const bakelite = new THREE.MeshPhysicalMaterial({
        color: '#121416',
        roughness: 0.38,
        metalness: 0.05,
        clearcoat: 0.45,
    });
    const steel = new THREE.MeshStandardMaterial({
        color: '#b6bcbd',
        metalness: 1,
        roughness: 0.32,
    });
    const brass = new THREE.MeshStandardMaterial({
        color: '#8a8060',
        metalness: 0.85,
        roughness: 0.45,
    });
    const sheet = new THREE.MeshStandardMaterial({
        color: '#8d938f',
        metalness: 0.55,
        roughness: 0.42,
    });
    const dark = new THREE.MeshStandardMaterial({ color: '#0e1012', roughness: 0.9 });
    const grey = new THREE.MeshStandardMaterial({
        color: '#a9afad',
        metalness: 0.4,
        roughness: 0.5,
    });

    const root = new THREE.Group();
    const anchors: Record<string, THREE.Vector3> = {};
    const r = mulberry32(52);

    // Tray, with a slot for each guide arm and the alignment holes
    root.add(mesh(new RoundedBoxGeometry(4.5, 0.36, 2.8, 4, 0.1), body, [0.05, 0.18, 0.05]));
    for (const x of WHEEL_X)
        root.add(mesh(new THREE.BoxGeometry(0.05, 0.006, 0.52), dark, [x, 0.362, 1.0]));
    for (const x of [-1.25, 1.55]) root.add(zCyl(0.045, 0.02, dark, [x, 0.18, 1.455], 20));
    root.add(mesh(new THREE.BoxGeometry(0.16, 0.05, 0.1), steel, [0.95, 0.39, 1.15]));

    // Printer housing: a faceted block, dial on the front
    const housing: [number, number][] = [
        [-0.95, 0.36],
        [1.2, 0.36],
        [1.2, 1.2],
        [1.02, 1.42],
        [0.55, 1.72],
        [-0.55, 1.78],
        [-0.95, 1.6],
    ];
    root.add(mesh(profile(housing, -2.05, 1.2, 0.04), body));
    root.add(mesh(new THREE.BoxGeometry(0.05, 1.25, 1.95), brass, [-0.82, 0.98, 0.12]));

    // Letter ring, fixed, and the indicator plate that turns
    const ringTex = ringTexture();
    const ringMat = new THREE.MeshStandardMaterial({
        map: ringTex,
        roughness: 0.45,
        metalness: 0.2,
    });
    root.add(zCyl(0.4, 0.04, [bakelite, ringMat, bakelite], [-1.45, 0.84, 1.22], 96));
    const indicator = mesh(
        new THREE.CircleGeometry(0.29, 64),
        new THREE.MeshStandardMaterial({
            map: indicatorTexture(),
            transparent: true,
            roughness: 0.4,
        }),
    );
    indicator.position.set(-1.45, 0.84, 1.247);
    root.add(indicator);
    root.add(zCyl(0.025, 0.03, steel, [-1.45, 0.84, 1.255], 16));
    anchors.dial = new THREE.Vector3(-1.45, 0.84, 1.26);

    // End face: selection knob, mode selector, paper advance, counter
    const knurl = new THREE.MeshStandardMaterial({
        color: '#c3c8c8',
        metalness: 1,
        roughness: 0.35,
        bumpMap: stripes(),
        bumpScale: 3,
    });
    root.add(xCyl(0.14, 0.22, knurl, [-2.16, 0.62, 0.55], 48));
    root.add(xCyl(0.09, 0.05, dark, [-2.28, 0.62, 0.55], 32));
    root.add(xCyl(0.065, 0.12, knurl, [-2.1, 0.46, 0.93], 32));
    root.add(xCyl(0.08, 0.14, knurl, [-2.11, 0.98, 0.78], 32));
    root.add(xCyl(0.07, 0.02, dark, [-2.052, 0.98, 0.3], 32));
    const counter = mesh(
        new THREE.PlaneGeometry(0.34, 0.13),
        new THREE.MeshStandardMaterial({ map: counterTexture(), roughness: 0.5 }),
    );
    counter.rotation.y = -Math.PI / 2;
    counter.position.set(-2.056, 1.42, 0.1);
    root.add(counter);
    anchors.knobs = new THREE.Vector3(-2.2, 0.62, 0.55);

    // Printer on top: roller assembly and the bracket over the paper
    root.add(xCyl(0.06, 0.34, steel, [-1.05, 1.72, 0.62]));
    root.add(xCyl(0.08, 0.06, knurl, [-0.86, 1.72, 0.62]));
    const bracket = mesh(new THREE.BoxGeometry(0.46, 0.025, 0.34), steel, [-1.62, 1.86, 0.12]);
    bracket.rotation.z = 0.08;
    root.add(bracket);

    // Paper tape: out of the top, over the end, hanging in a loop
    const [tapeTex, tapeChars] = tapeTexture(cipherText);
    const [tapeGeo, tapeLen] = ribbon(
        [
            [-1.2, 1.8],
            [-1.7, 1.83],
            [-2.15, 1.9],
            [-2.45, 1.76],
            [-2.56, 1.45],
            [-2.47, 1.15],
            [-2.32, 1.0],
            [-2.36, 0.8],
            [-2.55, 0.7],
        ].map(([x, y]) => new THREE.Vector3(x, y, 0.12)),
        0.3,
        160,
    );
    const paper = new THREE.MeshStandardMaterial({
        map: tapeTex,
        roughness: 0.85,
        side: THREE.DoubleSide,
    });
    const tape = mesh(tapeGeo, paper);
    tape.castShadow = false;
    root.add(tape);
    anchors.tape = new THREE.Vector3(-2.2, 1.9, 0.12);

    // Key wheels with numbered rims and toothed edges
    const wheels: THREE.Group[] = [];
    WHEEL_PINS.forEach((pins, i) => {
        const g = new THREE.Group();
        g.position.set(WHEEL_X[i], WHEEL_Y, WHEEL_Z);
        const rim = new THREE.MeshPhysicalMaterial({
            map: rimTexture(pins),
            roughness: 0.4,
            clearcoat: 0.4,
        });
        const disc = new THREE.CylinderGeometry(WHEEL_R - 0.02, WHEEL_R - 0.02, 0.1, 96);
        disc.rotateZ(Math.PI / 2);
        g.add(mesh(disc, [rim, bakelite, bakelite]));
        g.add(xCyl(0.1, 0.16, steel, [0, 0, 0]));
        const teeth = new THREE.InstancedMesh(
            new THREE.BoxGeometry(0.03, 0.035, 0.04),
            bakelite,
            pins * 2,
        );
        const m = new THREE.Matrix4();
        for (let k = 0; k < pins * 2; k++) {
            const a = ((k % pins) / pins) * Math.PI * 2;
            m.makeRotationX(a);
            m.setPosition(k < pins ? -0.058 : 0.058, Math.cos(a) * WHEEL_R, Math.sin(a) * WHEEL_R);
            teeth.setMatrixAt(k, m);
        }
        teeth.castShadow = true;
        g.add(teeth);
        root.add(g);
        wheels.push(g);

        // guide arm down into the tray slot
        const arm = profile(
            [
                [0.25, 0.6],
                [0.55, 0.6],
                [1.1, 0.37],
                [0.84, 0.37],
            ],
            WHEEL_X[i] - 0.016,
            0.032,
            0,
        );
        root.add(mesh(arm, sheet));
    });
    root.add(xCyl(0.03, 1.8, steel, [0.1, WHEEL_Y, WHEEL_Z]));
    anchors.wheels = new THREE.Vector3(WHEEL_X[2], WHEEL_Y + WHEEL_R, WHEEL_Z + 0.1);

    // Cage: brass slide bars around a drum, lugs set by the key
    const cage = new THREE.Group();
    cage.position.set(0.115, CAGE_Y, CAGE_Z);
    const len = 1.67;
    const bars = new THREE.InstancedMesh(
        new THREE.BoxGeometry(len, 0.035, 0.075),
        brass,
        CAGE_BARS,
    );
    const lugs = new THREE.InstancedMesh(
        new THREE.BoxGeometry(0.06, 0.06, 0.05),
        steel,
        CAGE_BARS * 2,
    );
    const m = new THREE.Matrix4();
    for (let b = 0; b < CAGE_BARS; b++) {
        const a = (b / CAGE_BARS) * Math.PI * 2;
        m.makeRotationX(a);
        m.setPosition(0, Math.cos(a) * 0.46, Math.sin(a) * 0.46);
        bars.setMatrixAt(b, m);
        for (let k = 0; k < 2; k++) {
            const w = Math.floor(r() * 6);
            m.makeRotationX(a);
            m.setPosition(WHEEL_X[w] - 0.115, Math.cos(a) * 0.5, Math.sin(a) * 0.5);
            lugs.setMatrixAt(b * 2 + k, m);
        }
    }
    bars.castShadow = lugs.castShadow = true;
    cage.add(bars, lugs);
    cage.add(xCyl(0.5, 0.03, steel, [-len / 2 - 0.02, 0, 0], 64));
    cage.add(xCyl(0.48, 0.04, brass, [len / 2 + 0.02, 0, 0], 64));
    cage.add(xCyl(0.05, len + 0.3, steel, [0, 0, 0]));
    root.add(cage);
    anchors.cage = new THREE.Vector3(0.4, CAGE_Y + 0.46, CAGE_Z);

    // Locking bar, numbered for the wheels
    const barMat = new THREE.MeshStandardMaterial({
        map: barTexture(),
        metalness: 0.8,
        roughness: 0.35,
    });
    const lock = new THREE.BoxGeometry(1.95, 0.06, 0.08);
    root.add(mesh(lock, [steel, steel, steel, steel, barMat, steel], [0.1, 1.68, -0.06]));
    for (const x of [-0.87, 1.02])
        root.add(mesh(new THREE.BoxGeometry(0.06, 0.2, 0.12), steel, [x, 1.6, -0.1]));

    // Right frame, spindle and the advance lever
    const frame: [number, number][] = [
        [-1.2, 0.36],
        [1.05, 0.36],
        [1.05, 0.8],
        [0.6, 1.5],
        [0.1, 1.95],
        [-0.6, 2.0],
        [-1.05, 1.85],
        [-1.2, 1.5],
    ];
    root.add(mesh(profile(frame, 1.02, 0.1, 0.02), body));
    root.add(xCyl(0.025, 1.0, steel, [1.6, 0.62, 0.55]));
    root.add(xCyl(0.07, 0.02, steel, [2.1, 0.62, 0.55], 24));

    const lever = new THREE.Group();
    lever.position.set(1.2, CAGE_Y, CAGE_Z);
    lever.add(mesh(new RoundedBoxGeometry(0.08, 1.05, 0.17, 2, 0.03), grey, [0, 0.45, 0]));
    lever.add(xCyl(0.12, 0.1, grey, [0, 0, 0], 32));
    lever.add(xCyl(0.1, 0.5, bakelite, [0.33, 0.95, 0], 40));
    lever.add(xCyl(0.108, 0.08, grey, [0.1, 0.95, 0], 40));
    root.add(lever);
    anchors.lever = new THREE.Vector3(1.55, CAGE_Y + 0.95, CAGE_Z);

    // Hinged hood behind the cage, standing open
    root.add(mesh(shell(-0.85, 1.02, 1.05, -0.35, 1.05, 1.4, 3.1), bodyDark));

    // Case lid, hinged at the back of the tray
    const caseLid = new THREE.Group();
    caseLid.position.set(0.05, 0.36, -1.35);
    const D = 2.35;
    const L = 2.85;
    const top = mesh(new RoundedBoxGeometry(4.6, 0.06, L, 2, 0.03), body, [0, D, L / 2]);
    caseLid.add(top);
    for (const x of [-2.27, 2.27])
        caseLid.add(mesh(new THREE.BoxGeometry(0.06, D, L), bodyDark, [x, D / 2, L / 2]));
    caseLid.add(mesh(new THREE.BoxGeometry(4.6, D, 0.06), bodyDark, [0, D / 2, L - 0.03]));
    caseLid.add(mesh(new THREE.BoxGeometry(4.6, D, 0.06), bodyDark, [0, D / 2, 0.03]));
    root.add(caseLid);

    return { root, wheels, cage, lever, indicator, tapeTex, tapeChars, tapeLen, caseLid, anchors };
}

// Knurling: fine stripes used as a bump map
function stripes(): THREE.CanvasTexture {
    const [c, ctx] = canvas(256, 16);
    for (let x = 0; x < 256; x += 4) {
        ctx.fillStyle = x % 8 ? '#000' : '#fff';
        ctx.fillRect(x, 0, 4, 16);
    }
    const t = new THREE.CanvasTexture(c);
    t.wrapS = t.wrapT = THREE.RepeatWrapping;
    t.repeat.set(6, 1);
    return t;
}
