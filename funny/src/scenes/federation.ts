// Federation: instances as peers on a wireframe globe. On each beat a
// relay crosses between two of them and lands within the beat:
// synchronous, never queued.

import * as THREE from 'three';
import { toScreen } from '../engine/camera.ts';
import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { clamp, easeInOut, hold, smooth, span } from '../engine/math.ts';
import { C, MOTIF } from '../engine/palette.ts';
import { hash1, mulberry32 } from '../engine/rng.ts';
import { kickEnv } from '../engine/score.ts';
import type { Frame, SceneDef } from '../engine/types.ts';

const R = 2;

const PEERS: [string, number, number][] = [
    ['a.example', 48, 8],
    ['b.example', 36, -100],
    ['mail.example.org', 52, 100],
    ['relay.example.net', -30, -60],
    ['home.example', 12, 20],
    ['lab.example.edu', 60, -30],
    ['c.example.net', -18, 140],
    ['kitchen.example', 25, 75],
    ['d.example.org', -40, 30],
    ['tiny.example', 5, -150],
    ['north.example', 70, 170],
    ['harbor.example', -5, -20],
];

const USERS = [
    'salmon',
    'flat_white',
    'mocha',
    'cortado',
    'ristretto',
    'doppio',
    'lungo',
    'affogato',
];

function latLon(lat: number, lon: number, r = R): THREE.Vector3 {
    const phi = THREE.MathUtils.degToRad(90 - lat);
    const th = THREE.MathUtils.degToRad(lon);
    return new THREE.Vector3(
        r * Math.sin(phi) * Math.cos(th),
        r * Math.cos(phi),
        r * Math.sin(phi) * Math.sin(th),
    );
}

// Great-circle arc lifted off the surface
function arc(a: THREE.Vector3, b: THREE.Vector3, segs = 64): THREE.Vector3[] {
    const pts: THREE.Vector3[] = [];
    const ang = a.angleTo(b);
    for (let i = 0; i <= segs; i++) {
        const u = i / segs;
        const p = new THREE.Vector3()
            .copy(a)
            .normalize()
            .multiplyScalar(Math.sin((1 - u) * ang))
            .add(
                b
                    .clone()
                    .normalize()
                    .multiplyScalar(Math.sin(u * ang)),
            )
            .divideScalar(Math.sin(ang));
        p.multiplyScalar(R * (1 + 0.28 * Math.sin(Math.PI * u) * (ang / Math.PI + 0.3)));
        pts.push(p);
    }
    return pts;
}

export const federation: SceneDef = {
    id: 'federation',
    sections: ['federation'],
    create() {
        const scene = new THREE.Scene();
        scene.background = new THREE.Color('#030810');
        const cam = new THREE.PerspectiveCamera(38, 16 / 9, 0.05, 100);
        const globe = new THREE.Group();
        scene.add(globe);

        // Graticule
        const grat: number[] = [];
        for (let lat = -75; lat <= 75; lat += 15)
            for (let lon = 0; lon < 360; lon += 3) {
                const p = latLon(lat, lon);
                const q = latLon(lat, lon + 3);
                grat.push(p.x, p.y, p.z, q.x, q.y, q.z);
            }
        for (let lon = 0; lon < 360; lon += 15)
            for (let lat = -90; lat < 90; lat += 3) {
                const p = latLon(lat, lon);
                const q = latLon(lat + 3, lon);
                grat.push(p.x, p.y, p.z, q.x, q.y, q.z);
            }
        const gg = new THREE.BufferGeometry();
        gg.setAttribute('position', new THREE.Float32BufferAttribute(grat, 3));
        const gratMat = new THREE.LineBasicMaterial({
            color: '#1f3b5c',
            transparent: true,
            opacity: 0.7,
        });
        globe.add(new THREE.LineSegments(gg, gratMat));
        const core = new THREE.Mesh(
            new THREE.SphereGeometry(R * 0.995, 64, 32),
            new THREE.MeshBasicMaterial({ color: '#040b16' }),
        );
        globe.add(core);

        // Surface dust (fibonacci sphere)
        const dust: number[] = [];
        for (let i = 0; i < 2400; i++) {
            const y = 1 - (i / 2399) * 2;
            const rr = Math.sqrt(1 - y * y);
            const th = i * 2.399963;
            dust.push(Math.cos(th) * rr * R * 1.002, y * R * 1.002, Math.sin(th) * rr * R * 1.002);
        }
        const dg = new THREE.BufferGeometry();
        dg.setAttribute('position', new THREE.Float32BufferAttribute(dust, 3));
        globe.add(
            new THREE.Points(dg, new THREE.PointsMaterial({ color: '#35577e', size: 0.012 })),
        );

        // Peers
        const nodes = PEERS.map(([, lat, lon]) => latLon(lat, lon));
        const nodeMesh = new THREE.InstancedMesh(
            new THREE.SphereGeometry(0.045, 16, 8),
            new THREE.MeshBasicMaterial({ color: '#ffffff' }),
            nodes.length,
        );
        globe.add(nodeMesh);

        // Relays: one per beat, between two different peers
        const beats = 32;
        const relays = Array.from({ length: beats }, (_, b) => {
            const i = Math.floor(hash1(b * 3 + 1) * nodes.length);
            let j = Math.floor(hash1(b * 5 + 7) * (nodes.length - 1));
            if (j >= i) j++;
            const pts = arc(nodes[i], nodes[j]);
            const geo = new THREE.BufferGeometry().setFromPoints(pts);
            const mat = new THREE.LineBasicMaterial({
                color: C.phosphor,
                transparent: true,
                opacity: 0,
            });
            const line = new THREE.Line(geo, mat);
            globe.add(line);
            const from = USERS[Math.floor(hash1(b * 11) * USERS.length)];
            let to = USERS[Math.floor(hash1(b * 17 + 3) * USERS.length)];
            if (to === from) to = USERS[(USERS.indexOf(from) + 1) % USERS.length];
            return { i, j, pts, line, mat, from, to };
        });
        const packet = new THREE.Mesh(
            new THREE.SphereGeometry(0.05, 12, 8),
            new THREE.MeshBasicMaterial({ color: '#ffffff' }),
        );
        globe.add(packet);

        // Stars
        const r = mulberry32(99);
        const st: number[] = [];
        for (let i = 0; i < 1600; i++) {
            const v = new THREE.Vector3(r() - 0.5, r() - 0.5, r() - 0.5)
                .normalize()
                .multiplyScalar(30 + r() * 20);
            st.push(v.x, v.y, v.z);
        }
        const sg = new THREE.BufferGeometry();
        sg.setAttribute('position', new THREE.Float32BufferAttribute(st, 3));
        scene.add(new THREE.Points(sg, new THREE.PointsMaterial({ color: '#6f86a3', size: 0.06 })));

        const m4 = new THREE.Matrix4();
        const col = new THREE.Color();
        const wp = new THREE.Vector3();

        return {
            update(f: Frame, hud: Hud) {
                const bar = f.bar;
                const beat = f.beat;
                const kick = kickEnv(f.t, 0.22);
                const cur = Math.floor(beat);
                const u = beat - cur;

                globe.rotation.y = f.local * 0.12 + 0.6;
                globe.rotation.x = 0.25;

                // camera: in from far, around, then close over the surface
                const d = 9 - 3.2 * easeInOut(span(bar, 0, 3)) - 0.8 * smooth(span(bar, 6, 8));
                const az = -0.4 + bar * 0.1;
                cam.position.set(
                    Math.sin(az) * d,
                    1.2 + 0.6 * Math.sin(bar * 0.3),
                    Math.cos(az) * d,
                );
                cam.lookAt(0, 0, 0);
                cam.updateMatrixWorld();

                relays.forEach((rl, b) => {
                    const age = beat - b;
                    rl.mat.opacity = age >= 0 ? Math.max(0.12, Math.exp(-age / 2) * 0.95) : 0;
                });
                const live = relays[Math.min(cur, beats - 1)];
                const travel = clamp(u / 0.7);
                const idx = Math.min(
                    live.pts.length - 1,
                    Math.floor(easeInOut(travel) * (live.pts.length - 1)),
                );
                packet.position.copy(live.pts[idx]);
                packet.visible = u < 0.85 && beat >= 0 && cur < beats;

                nodes.forEach((p, i) => {
                    const lit =
                        (live.i === i ? 1 - travel : 0) +
                        (live.j === i ? smooth(span(u, 0.6, 0.75)) * (1 - u) * 3 : 0);
                    const s = 1 + kick * 0.5 + lit * 1.2;
                    m4.makeScale(s, s, s).setPosition(p);
                    nodeMesh.setMatrixAt(i, m4);
                    col.set(C.ice).lerp(new THREE.Color(C.phosphor), clamp(lit));
                    nodeMesh.setColorAt(i, col);
                });
                nodeMesh.instanceMatrix.needsUpdate = true;
                if (nodeMesh.instanceColor) nodeMesh.instanceColor.needsUpdate = true;
                gratMat.opacity = 0.5 + 0.3 * kick;

                // labels for peers facing the camera
                globe.updateMatrixWorld();
                PEERS.forEach(([name], i) => {
                    wp.copy(nodes[i]).applyMatrix4(globe.matrixWorld);
                    const facing = wp.clone().normalize().dot(cam.position.clone().normalize());
                    if (facing < 0.15) return;
                    const [x, y] = toScreen(cam, wp);
                    hud.text(name, x + 14, y - 10, {
                        face: 'sans',
                        size: 17,
                        spacing: 1,
                        color: C.ice,
                        alpha: clamp((facing - 0.15) * 3) * 0.85,
                    });
                });

                drawHud(hud, bar, beat);

                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.5 + kick * 0.3,
                        threshold: 0.5,
                        scan: 0.12,
                        aberr: 0.5 + kick * 0.7,
                        grain: 0.3,
                        dither: 0.1,
                        vignette: 0.85,
                        flash: Math.exp(-f.local / 0.3) * 0.7,
                        fade: smooth(span(bar, 7.8, 8)),
                    },
                };
            },
        };

        function drawHud(hud: Hud, bar: number, beat: number) {
            hud.scrim(280, 0, 0.6);
            hud.scrim(H - 320, H, 0.85);
            hud.text('FEDERATION', 120, 130, {
                face: 'sansBold',
                size: 26,
                spacing: 10,
                alpha: hold(bar, 0.3, 7.8, 0.3, 0.3),
            });
            const lines: [number, number, string][] = [
                [0.6, 3.4, 'Every instance is a peer.'],
                [3.5, 5.9, 'One static Go binary, or a Worker. Almost nothing to run.'],
                [
                    6,
                    7.9,
                    'Relayed while you wait. Never queued. If it fails, the draft stays with you.',
                ],
            ];
            for (const [a, b, s] of lines)
                hud.text(s, W / 2, H - 110, {
                    face: 'italic',
                    size: 44,
                    align: 'center',
                    alpha: hold(bar, a, b, 0.35, 0.3),
                });

            // relay log, one line per beat
            const lA = hold(bar, 1, 7.8, 0.3, 0.3);
            if (lA > 0) {
                const w = 560;
                const win = hud.window(W - w - 90, 110, w, 300, 'relay.log', { alpha: lA * 0.95 });
                const cur = Math.floor(beat);
                const first = Math.max(0, cur - 5);
                for (let b = first; b <= Math.min(cur, 31); b++) {
                    const rl = relays[b];
                    const line = `${rl.from}@${PEERS[rl.i][0]}  →  ${rl.to}@${PEERS[rl.j][0]}`;
                    const done = b < cur || beat - cur > 0.7;
                    hud.text(line, win.x + 16, win.y + 34 + (b - first) * 40, {
                        face: 'mono',
                        size: 15,
                        color: MOTIF.text,
                        alpha: lA * (b === cur ? 1 : 0.6),
                    });
                    hud.text(
                        done ? '201' : '···',
                        win.x + win.w - 16,
                        win.y + 34 + (b - first) * 40,
                        {
                            face: 'monoBold',
                            size: 15,
                            align: 'right',
                            color: done ? C.phosphor : C.fog,
                            alpha: lA,
                        },
                    );
                }
            }
        }
    },
};
