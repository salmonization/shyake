// ML-DSA-65: a header-signed request binds the SHA-256 of its body.
// The 3309-byte signature streams past as punched tape, and the
// public key turns out to be 1952 bytes long.

import * as THREE from 'three';
import { fly, type Key } from '../engine/camera.ts';
import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { easeOut, hold, span } from '../engine/math.ts';
import { C, MOTIF } from '../engine/palette.ts';
import { kickEnv } from '../engine/score.ts';
import type { Frame, SceneDef } from '../engine/types.ts';
import { artifacts, b64, POW, SENDER, TIMESTAMP } from '../data/artifacts.ts';

const ROW = 0.1;
const HOLE = 0.036;

const KEYS: Key[] = [
    { bar: 0, pos: [2.5, 1.6, 3.2], look: [0, 0, -1], fov: 40 },
    { bar: 4, pos: [-1.5, 1.1, 2.4], look: [0.5, 0, -2], fov: 38 },
    { bar: 8, pos: [0.2, 3.5, 3.0], look: [0, 0, -1.5], fov: 44 },
];

export const sign: SceneDef = {
    id: 'sign',
    sections: ['sign'],
    create() {
        const a = artifacts();
        const sig = a.mailSig;
        const scene = new THREE.Scene();
        scene.background = new THREE.Color('#030810');
        scene.fog = new THREE.Fog('#030810', 3, 16);
        const cam = new THREE.PerspectiveCamera(40, 16 / 9, 0.05, 100);

        // Eight-hole tape, one row per signature byte, plus sprocket holes
        const rows = sig.length;
        let holes = 0;
        for (const b of sig) for (let k = 0; k < 8; k++) if ((b >> k) & 1) holes++;
        const holeMesh = new THREE.InstancedMesh(
            new THREE.CircleGeometry(HOLE / 2, 12),
            new THREE.MeshBasicMaterial({ color: C.phosphor }),
            holes + rows,
        );
        const tapeGroup = new THREE.Group();
        const m4 = new THREE.Matrix4();
        const rot = new THREE.Matrix4().makeRotationX(-Math.PI / 2);
        let n = 0;
        const holeX = (k: number) => (k < 3 ? k - 4 : k - 3) * 0.055;
        for (let r = 0; r < rows; r++) {
            const z = -r * ROW;
            for (let k = 0; k < 8; k++)
                if ((sig[r] >> k) & 1) {
                    m4.copy(rot).setPosition(holeX(k), 0.002, z);
                    holeMesh.setMatrixAt(n++, m4);
                }
            m4.copy(rot)
                .scale(new THREE.Vector3(0.45, 0.45, 1))
                .setPosition(-0.055 * 0.5, 0.002, z);
            holeMesh.setMatrixAt(n++, m4);
        }
        const paper = new THREE.Mesh(
            new THREE.PlaneGeometry(0.62, rows * ROW + 1),
            new THREE.MeshStandardMaterial({ color: '#16283d', roughness: 0.7 }),
        );
        paper.rotation.x = -Math.PI / 2;
        paper.position.z = -(rows * ROW) / 2;
        const feedGroup = new THREE.Group();
        feedGroup.add(paper, holeMesh);
        tapeGroup.add(feedGroup);
        scene.add(tapeGroup);
        scene.add(new THREE.HemisphereLight('#4b6d99', '#020406', 1.4));

        const sigB64 = b64(sig);

        return {
            update(f: Frame, hud: Hud) {
                const bar = f.bar;
                const kick = kickEnv(f.t, 0.2);

                // the tape runs towards the camera as the signature is made
                const feed = easeOut(span(bar, 1.8, 5.2), 2) * rows * ROW * 0.55 + bar * 0.4;
                feedGroup.position.z = feed;
                tapeGroup.position.x = 0.35 * Math.sin(bar * 0.4);
                tapeGroup.rotation.y = -0.18 + 0.05 * Math.sin(bar * 0.3);
                fly(cam, KEYS, bar, 0.04, f.t);

                drawHud(hud, bar, sigB64);

                const stamp = (b: number) => (bar > b ? Math.exp(-(bar - b) / 0.05) : 0);
                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.6 + 0.2 * kick,
                        threshold: 0.45,
                        scan: 0.18,
                        aberr: 0.4 + kick * 0.4,
                        grain: 0.3,
                        dither: 0.15,
                        vignette: 0.9,
                        flash:
                            Math.exp(-f.local / 0.25) * 0.3 +
                            0.25 * (stamp(2) + stamp(4) + stamp(6)),
                    },
                };
            },
        };

        function drawHud(hud: Hud, bar: number, sigB64: string) {
            hud.scrim(0, 300, 0.7);
            hud.text('ML-DSA-65', 120, 130, {
                face: 'sansBold',
                size: 26,
                spacing: 10,
                alpha: hold(bar, 0.1, 7.9, 0.3, 0.3),
            });
            hud.text('FIPS 204  ·  signatures', 120, 166, {
                face: 'sans',
                size: 18,
                spacing: 3,
                color: C.fog,
                alpha: hold(bar, 0.1, 4.8, 0.3, 0.3),
            });

            // The request, one line per eighth
            const rA = hold(bar, 0.15, 4.7, 0.3, 0.3);
            if (rA > 0) {
                const sigShown = bar >= 4 ? `${sigB64.slice(0, 26)}…` : '';
                const lines: [string, string][] = [
                    ['POST /api/block HTTP/1.1', C.ice],
                    ['X-Shyake-Username:  ' + SENDER, MOTIF.text],
                    ['X-Shyake-Timestamp: ' + TIMESTAMP, MOTIF.text],
                    ['X-Shyake-Pow:       ' + POW, MOTIF.text],
                    ['X-Shyake-Signature: ' + sigShown, C.phosphor],
                    ['Content-Type: application/json', MOTIF.text],
                    ['', MOTIF.text],
                    [a.blockBody, C.frost],
                ];
                const win = hud.window(110, 210, 800, 420, 'winterm · request', { alpha: rA });
                lines.forEach(([s, color], i) => {
                    const at = 0.25 + i * 0.22;
                    if (bar < at) return;
                    hud.text(s, win.x + 20, win.y + 40 + i * 40, {
                        face: 'mono',
                        size: 21,
                        color,
                        alpha: rA,
                    });
                });
                hud.text('every request also pays 20 bits of proof of work', 120, 680, {
                    face: 'sans',
                    size: 18,
                    spacing: 2,
                    color: C.fog,
                    alpha: rA * hold(bar, 1, 4.7, 0.3, 0.3),
                });
            }

            // What gets signed: method, path, user, time, body digest
            const sA = hold(bar, 1.5, 4.7, 0.3, 0.3);
            if (sA > 0) {
                const x = 990;
                hud.rect(x - 30, 236, 880, 390, C.ink, sA * 0.6);
                hud.text('sha256(body)', x, 280, {
                    face: 'sans',
                    size: 18,
                    spacing: 3,
                    color: C.fog,
                    alpha: sA,
                });
                const dShown = Math.floor(easeOut(span(bar, 1.5, 1.9)) * 64);
                hud.text(a.blockDigest.slice(0, dShown), x, 316, {
                    face: 'mono',
                    size: 20,
                    color: C.frost,
                    alpha: sA,
                });
                const mA = hold(bar, 2, 4.7, 0.2, 0.3);
                hud.text('signed message', x, 400, {
                    face: 'sans',
                    size: 18,
                    spacing: 3,
                    color: C.fog,
                    alpha: mA,
                });
                const parts = a.blockSigned.split(':');
                let px = x;
                parts.forEach((p, i) => {
                    const s = i < parts.length - 1 ? p + ':' : p.slice(0, 16) + '…';
                    const color = i === parts.length - 1 ? C.frost : i === 2 ? C.phosphor : C.ice;
                    hud.text(s, px, 436, { face: 'monoBold', size: 20, color, alpha: mA });
                    px += hud.measure(s, 'monoBold', 20);
                });
                hud.text('The digest binds the body: no one in between can swap it.', x, 510, {
                    face: 'italic',
                    size: 32,
                    alpha: hold(bar, 2.4, 4.7, 0.3, 0.3),
                });
                const cA = hold(bar, 4, 4.7, 0.1, 0.3);
                hud.text(`signature  ·  ${a.mailSig.length} bytes`, x, 590, {
                    face: 'sansBold',
                    size: 22,
                    spacing: 4,
                    color: C.phosphor,
                    alpha: cA,
                    glow: 10,
                });
            }

            // 1952
            const kA = hold(bar, 5, 8.1, 0.25, 0.25);
            if (kA > 0) {
                hud.rect(0, 0, W, H, C.ink, kA * 0.72);
                const grow = easeOut(span(bar, 5, 5.6));
                hud.text('1952', W / 2, H / 2 + 110, {
                    face: 'serif',
                    size: 300 + grow * 40,
                    align: 'center',
                    alpha: kA,
                    glow: 24,
                    color: C.white,
                });
                hud.text('BYTES  ·  ONE ML-DSA-65 PUBLIC KEY', W / 2, H / 2 + 190, {
                    face: 'sans',
                    size: 22,
                    spacing: 8,
                    align: 'center',
                    color: C.fog,
                    alpha: kA,
                });
                hud.text('The same number as the machine.', W / 2, H - 150, {
                    face: 'italic',
                    size: 44,
                    align: 'center',
                    alpha: hold(bar, 6, 8.1, 0.4, 0.25),
                });
                hud.hexBlock(a.dsa.pk, 120, 110, {
                    cols: 72,
                    rows: 4,
                    size: 13,
                    lineHeight: 17,
                    color: C.steel,
                    alpha: kA * 0.6,
                    reveal: easeOut(span(bar, 5, 6)),
                });
            }
        }
    },
};
