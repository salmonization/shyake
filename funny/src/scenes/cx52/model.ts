// A procedural CX-52: hammertone body, six pin wheels, a lug cage,
// an alphabet dial, a crank, and a paper tape printer. Built from
// primitives; proportions follow photographs, not drawings.

import * as THREE from 'three';
import { RoundedBoxGeometry } from 'three/addons/geometries/RoundedBoxGeometry.js';
import { C } from '../../engine/palette.ts';
import { mulberry32 } from '../../engine/rng.ts';
import { canvas, hammertone, toTexture } from '../../engine/textures.ts';

export const WHEEL_PINS = [47, 43, 42, 41, 38, 37];
export const WHEEL_X = WHEEL_PINS.map((_, i) => -0.3 + i * 0.3);
export const AXIS_Y = 1.02;
export const WHEEL_Z = 0.3;
export const CAGE_Z = -0.62;
export const CAGE_BARS = 32;
export const TAPE_CELL = 0.068;
export const TAPE_LEN = 2.3;

export interface Materials {
    body: THREE.MeshPhysicalMaterial;
    bakelite: THREE.MeshPhysicalMaterial;
    steel: THREE.MeshStandardMaterial;
    paper: THREE.MeshStandardMaterial;
    dial: THREE.MeshStandardMaterial;
    rims: THREE.MeshPhysicalMaterial[];
    edges: THREE.LineBasicMaterial;
    all: THREE.Material[];
}

export interface Machine {
    root: THREE.Group;
    wheels: THREE.Group[];
    cage: THREE.Group;
    crank: THREE.Group;
    dial: THREE.Mesh;
    tape: THREE.Mesh;
    tapeTex: THREE.CanvasTexture;
    tapeChars: number;
    lid: THREE.Group;
    mats: Materials;
    anchors: Record<string, THREE.Vector3>;
}

/* ------------------------------------------------------------------ */
/* Textures                                                           */
/* ------------------------------------------------------------------ */

const ALPHA = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';

function rimTexture(pins: number): THREE.CanvasTexture {
    const [c, ctx] = canvas(2048, 64);
    ctx.fillStyle = '#0c0f13';
    ctx.fillRect(0, 0, 2048, 64);
    ctx.fillStyle = '#c9d6e2';
    ctx.font = '700 34px "FreeSans"';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const w = 2048 / pins;
    for (let i = 0; i < pins; i++) {
        ctx.save();
        ctx.translate(i * w + w / 2, 33);
        ctx.rotate(Math.PI / 2);
        ctx.fillText(ALPHA[i % 26], 0, 0);
        ctx.restore();
    }
    return toTexture(c);
}

function dialTexture(): THREE.CanvasTexture {
    const S = 1024;
    const [c, ctx] = canvas(S, S);
    const m = S / 2;
    ctx.fillStyle = '#10161d';
    ctx.fillRect(0, 0, S, S);
    // outer steel-blue ring with letters
    ctx.beginPath();
    ctx.arc(m, m, 500, 0, Math.PI * 2);
    ctx.fillStyle = '#1d3350';
    ctx.fill();
    ctx.beginPath();
    ctx.arc(m, m, 372, 0, Math.PI * 2);
    ctx.fillStyle = '#131c26';
    ctx.fill();
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (let i = 0; i < 26; i++) {
        const a = (i / 26) * Math.PI * 2 - Math.PI / 2;
        ctx.save();
        ctx.translate(m + Math.cos(a) * 436, m + Math.sin(a) * 436);
        ctx.rotate(a + Math.PI / 2);
        ctx.fillStyle = '#e4eef6';
        ctx.font = '700 78px "FreeSans"';
        ctx.fillText(ALPHA[i], 0, 0);
        ctx.restore();
        ctx.save();
        ctx.translate(m + Math.cos(a) * 330, m + Math.sin(a) * 330);
        ctx.rotate(a + Math.PI / 2);
        ctx.fillStyle = C.cyan;
        ctx.font = '400 44px "FreeSans"';
        ctx.fillText(String((i % 10) + 1 === 10 ? 0 : (i % 10) + 1), 0, 0);
        ctx.restore();
    }
    // inner plate and pointer
    const g = ctx.createRadialGradient(m - 60, m - 60, 20, m, m, 290);
    g.addColorStop(0, '#9aa7b3');
    g.addColorStop(1, '#46525e');
    ctx.beginPath();
    ctx.arc(m, m, 290, 0, Math.PI * 2);
    ctx.fillStyle = g;
    ctx.fill();
    ctx.fillStyle = '#2a333c';
    ctx.beginPath();
    ctx.moveTo(m - 26, m);
    ctx.lineTo(m, m - 270);
    ctx.lineTo(m + 26, m);
    ctx.arc(m, m, 26, 0, Math.PI);
    ctx.fill();
    ctx.beginPath();
    ctx.arc(m, m, 18, 0, Math.PI * 2);
    ctx.fillStyle = '#c3ccd4';
    ctx.fill();
    return toTexture(c);
}

// Printed tape: cipher text in five-letter groups
function tapeTexture(text: string): [THREE.CanvasTexture, number] {
    const cell = 64;
    const [c, ctx] = canvas(text.length * cell, 128);
    ctx.fillStyle = '#e3e9ec';
    ctx.fillRect(0, 0, c.width, 128);
    ctx.fillStyle = '#1b2733';
    ctx.font = '700 70px "FreeMono"';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (let i = 0; i < text.length; i++) ctx.fillText(text[i], i * cell + cell / 2, 68);
    // perforation-free paper grain
    const r = mulberry32(77);
    for (let i = 0; i < 4000; i++) {
        ctx.fillStyle = `rgba(40,60,80,${r() * 0.05})`;
        ctx.fillRect(r() * c.width, r() * 128, 2, 2);
    }
    const t = toTexture(c);
    t.wrapS = THREE.ClampToEdgeWrapping;
    return [t, text.length];
}

/* ------------------------------------------------------------------ */
/* Geometry helpers                                                   */
/* ------------------------------------------------------------------ */

function withEdges(mesh: THREE.Mesh, mat: THREE.LineBasicMaterial, angle = 25): THREE.Mesh {
    const e = new THREE.LineSegments(new THREE.EdgesGeometry(mesh.geometry, angle), mat);
    e.renderOrder = 2;
    mesh.add(e);
    return mesh;
}

function shadow<T extends THREE.Object3D>(o: T): T {
    o.traverse((x) => {
        if (x instanceof THREE.Mesh) {
            x.castShadow = true;
            x.receiveShadow = true;
        }
    });
    return o;
}

// Ribbon along a path, for the paper tape
function ribbon(path: (s: number) => THREE.Vector3, len: number, width: number, segs: number) {
    const pos: number[] = [];
    const uv: number[] = [];
    const idx: number[] = [];
    const side = new THREE.Vector3(0, 0, 1);
    for (let i = 0; i <= segs; i++) {
        const u = i / segs;
        const p = path(u * len);
        pos.push(p.x, p.y, p.z + (width / 2) * side.z, p.x, p.y, p.z - (width / 2) * side.z);
        uv.push(u, 0, u, 1);
        if (i < segs) {
            const a = i * 2;
            idx.push(a, a + 1, a + 2, a + 1, a + 3, a + 2);
        }
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
    g.setIndex(idx);
    g.computeVertexNormals();
    return g;
}

/* ------------------------------------------------------------------ */
/* Assembly                                                           */
/* ------------------------------------------------------------------ */

export function buildMachine(cipherText: string): Machine {
    const bump = hammertone(1952);
    bump.repeat.set(2, 2);
    const body = new THREE.MeshPhysicalMaterial({
        color: '#4c544c',
        roughness: 0.62,
        metalness: 0.35,
        bumpMap: bump,
        bumpScale: 1.4,
        roughnessMap: bump,
        clearcoat: 0.25,
        clearcoatRoughness: 0.5,
    });
    const bakelite = new THREE.MeshPhysicalMaterial({
        color: '#0b0e12',
        roughness: 0.32,
        metalness: 0.1,
        clearcoat: 0.8,
        clearcoatRoughness: 0.25,
    });
    const steel = new THREE.MeshStandardMaterial({
        color: '#b9c4ce',
        metalness: 1,
        roughness: 0.28,
    });
    const paper = new THREE.MeshStandardMaterial({
        color: '#9fb0bc',
        roughness: 0.85,
        side: THREE.DoubleSide,
    });
    const dialTex = dialTexture();
    const dial = new THREE.MeshStandardMaterial({ map: dialTex, roughness: 0.4, metalness: 0.4 });
    const rims = WHEEL_PINS.map(
        (n) =>
            new THREE.MeshPhysicalMaterial({
                map: rimTexture(n),
                roughness: 0.35,
                metalness: 0.1,
                clearcoat: 0.6,
            }),
    );
    const edges = new THREE.LineBasicMaterial({ color: C.phosphor, transparent: true, opacity: 0 });
    const all: THREE.Material[] = [body, bakelite, steel, paper, dial, ...rims];

    const root = new THREE.Group();
    const anchors: Record<string, THREE.Vector3> = {};

    // Base plate
    const base = withEdges(
        new THREE.Mesh(new RoundedBoxGeometry(4.0, 0.34, 2.5, 4, 0.08), body),
        edges,
    );
    base.position.set(0.1, 0.17, 0);
    root.add(base);

    // Front housing with the dial, printer on top
    const housing = withEdges(
        new THREE.Mesh(new RoundedBoxGeometry(1.25, 1.35, 2.1, 5, 0.16), body),
        edges,
    );
    housing.position.set(-1.25, 0.34 + 0.67, 0.05);
    root.add(housing);
    const cap = withEdges(
        new THREE.Mesh(new RoundedBoxGeometry(1.0, 0.3, 1.2, 4, 0.12), body),
        edges,
    );
    cap.position.set(-1.3, 1.78, 0.45);
    root.add(cap);

    const dialMesh = withEdges(
        new THREE.Mesh(new THREE.CylinderGeometry(0.44, 0.44, 0.06, 96), [steel, dial, steel]),
        edges,
    );
    dialMesh.rotation.x = Math.PI / 2;
    dialMesh.position.set(-1.12, 1.05, 1.11);
    root.add(dialMesh);
    const bezel = new THREE.Mesh(new THREE.TorusGeometry(0.45, 0.025, 12, 96), steel);
    bezel.position.set(-1.12, 1.05, 1.12);
    root.add(bezel);
    anchors.dial = new THREE.Vector3(-1.12, 1.05, 1.15);

    // Knurled knobs on the front
    const knobGeo = new THREE.CylinderGeometry(0.1, 0.1, 0.14, 40);
    const knurl = new THREE.CylinderGeometry(0.105, 0.105, 0.08, 40, 1, true);
    const knobPos: [number, number, number][] = [
        [-1.62, 0.62, 1.13],
        [-1.62, 1.35, 1.13],
        [-1.45, 0.55, 1.12],
    ];
    knobPos.forEach((p, i) => {
        const k = new THREE.Group();
        const s = i === 2 ? 1.5 : 1;
        k.add(new THREE.Mesh(knobGeo, steel));
        const kn = new THREE.Mesh(knurl, bakelite);
        kn.position.y = 0.02;
        k.add(kn);
        k.scale.setScalar(s);
        k.rotation.x = Math.PI / 2;
        k.position.set(...p);
        root.add(k);
    });

    // Printer: roller and head
    const roller = new THREE.Mesh(new THREE.CylinderGeometry(0.07, 0.07, 0.3, 32), bakelite);
    roller.rotation.x = Math.PI / 2;
    roller.position.set(-1.62, 1.96, 0.45);
    root.add(roller);
    const head = new THREE.Mesh(new THREE.BoxGeometry(0.22, 0.08, 0.3), steel);
    head.position.set(-1.45, 1.97, 0.45);
    root.add(head);
    anchors.printer = new THREE.Vector3(-1.62, 2.0, 0.45);

    // Paper tape: out of the head, across, then drooping off the edge
    const [tapeTex, tapeChars] = tapeTexture(cipherText);
    const tapePath = (s: number) => {
        const x = -1.62 - s;
        const droop = Math.max(0, s - 0.55);
        const y = 2.02 + 0.08 * Math.sin(Math.min(s, 0.55) * 4) - droop * droop * 0.55;
        const z = 0.45 + droop * 0.35 + 0.05 * Math.sin(s * 2.5);
        return new THREE.Vector3(x, y, z);
    };
    const tape = new THREE.Mesh(ribbon(tapePath, TAPE_LEN, 0.17, 120), paper);
    paper.map = tapeTex;
    root.add(tape);

    // Wheel axle and cage axle
    const axle = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.03, 2.3, 16), steel);
    axle.rotation.z = Math.PI / 2;
    axle.position.set(0.45, AXIS_Y, WHEEL_Z);
    root.add(axle);
    const axle2 = axle.clone();
    axle2.position.z = CAGE_Z;
    root.add(axle2);

    // Pin wheels
    const wheels: THREE.Group[] = [];
    const r = mulberry32(52);
    WHEEL_PINS.forEach((pins, i) => {
        const g = new THREE.Group();
        g.position.set(WHEEL_X[i], AXIS_Y, WHEEL_Z);
        const disc = withEdges(
            new THREE.Mesh(new THREE.CylinderGeometry(0.5, 0.5, 0.13, 96), [
                rims[i],
                bakelite,
                bakelite,
            ]),
            edges,
            40,
        );
        disc.rotation.z = Math.PI / 2;
        g.add(disc);
        const hub = new THREE.Mesh(new THREE.CylinderGeometry(0.16, 0.16, 0.17, 32), steel);
        hub.rotation.z = Math.PI / 2;
        g.add(hub);

        // pins: active ones pushed to the right face
        const pin = new THREE.InstancedMesh(new THREE.BoxGeometry(0.05, 0.024, 0.024), steel, pins);
        const m = new THREE.Matrix4();
        for (let k = 0; k < pins; k++) {
            const a = (k / pins) * Math.PI * 2;
            const active = r() < 0.5;
            m.makeRotationX(a);
            m.setPosition(active ? 0.085 : 0.045, Math.cos(a) * 0.44, Math.sin(a) * 0.44);
            pin.setMatrixAt(k, m);
        }
        g.add(pin);
        root.add(g);
        wheels.push(g);
    });
    anchors.wheels = new THREE.Vector3(WHEEL_X[3], AXIS_Y + 0.5, WHEEL_Z + 0.2);

    // Feeler levers under the wheels
    WHEEL_X.forEach((x) => {
        const lever = new THREE.Mesh(new THREE.BoxGeometry(0.06, 0.5, 0.12), body);
        lever.position.set(x, 0.55, WHEEL_Z + 0.52);
        lever.rotation.x = -0.35;
        root.add(lever);
    });

    // Lug cage: bars around a drum, lugs set against wheel positions
    const cage = new THREE.Group();
    cage.position.set(0.45, AXIS_Y, CAGE_Z);
    const len = 2.0;
    const bars = new THREE.InstancedMesh(
        new THREE.BoxGeometry(len, 0.05, 0.07),
        bakelite,
        CAGE_BARS,
    );
    const lugs = new THREE.InstancedMesh(
        new THREE.BoxGeometry(0.07, 0.07, 0.05),
        steel,
        CAGE_BARS * 6,
    );
    const m = new THREE.Matrix4();
    let nl = 0;
    for (let b = 0; b < CAGE_BARS; b++) {
        const a = (b / CAGE_BARS) * Math.PI * 2;
        m.makeRotationX(a);
        m.setPosition(0, Math.cos(a) * 0.42, Math.sin(a) * 0.42);
        bars.setMatrixAt(b, m);
        for (let w = 0; w < 6; w++) {
            if (r() > 0.28) continue;
            m.makeRotationX(a);
            m.setPosition(WHEEL_X[w] - 0.45, Math.cos(a) * 0.47, Math.sin(a) * 0.47);
            lugs.setMatrixAt(nl++, m);
        }
    }
    lugs.count = nl;
    cage.add(bars, lugs);
    for (const x of [-len / 2, len / 2]) {
        const end = new THREE.Mesh(new THREE.CylinderGeometry(0.45, 0.45, 0.04, 64), steel);
        end.rotation.z = Math.PI / 2;
        end.position.x = x;
        cage.add(end);
    }
    root.add(cage);
    anchors.cage = new THREE.Vector3(0.9, AXIS_Y + 0.46, CAGE_Z);

    // Right-hand frame plate
    const frame = withEdges(
        new THREE.Mesh(new RoundedBoxGeometry(0.1, 1.55, 2.0, 4, 0.05), body),
        edges,
    );
    frame.position.set(1.55, 0.34 + 0.77, -0.15);
    root.add(frame);

    // Crank on the cage axle
    const crank = new THREE.Group();
    crank.position.set(1.66, AXIS_Y, CAGE_Z);
    const arm = new THREE.Mesh(new RoundedBoxGeometry(0.06, 0.62, 0.12, 2, 0.02), steel);
    arm.position.y = 0.28;
    crank.add(arm);
    const handle = new THREE.Mesh(new THREE.CylinderGeometry(0.075, 0.075, 0.42, 32), bakelite);
    handle.rotation.z = Math.PI / 2;
    handle.position.set(0.24, 0.56, 0);
    crank.add(handle);
    const collar = new THREE.Mesh(new THREE.CylinderGeometry(0.05, 0.05, 0.1, 24), steel);
    collar.rotation.z = Math.PI / 2;
    collar.position.set(0.06, 0.56, 0);
    crank.add(collar);
    root.add(crank);
    anchors.crank = new THREE.Vector3(1.9, AXIS_Y + 0.56, CAGE_Z);

    // Lid: a curved shell hinged at the back, standing open
    const lid = new THREE.Group();
    lid.position.set(0.1, 0.34, -1.25);
    const shell = new THREE.Mesh(
        new THREE.CylinderGeometry(1.0, 1.0, 3.9, 64, 1, true, -Math.PI / 2, Math.PI * 0.62),
        new THREE.MeshPhysicalMaterial({
            color: '#454c45',
            roughness: 0.65,
            metalness: 0.35,
            bumpMap: bump,
            bumpScale: 1.4,
            side: THREE.DoubleSide,
        }),
    );
    all.push(shell.material as THREE.Material);
    shell.rotation.z = Math.PI / 2;
    shell.position.set(0, 1.0, -0.05);
    withEdges(shell, edges, 30);
    lid.add(shell);
    lid.rotation.x = -0.25;
    root.add(lid);

    shadow(root);
    tape.castShadow = false;
    return {
        root,
        wheels,
        cage,
        crank,
        dial: dialMesh,
        tape,
        tapeTex,
        tapeChars,
        lid,
        mats: { body, bakelite, steel, paper, dial, rims, edges, all },
        anchors,
    };
}
