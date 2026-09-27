// Outro: the name, what it is, where it lives. A pin wheel of 256
// coefficients turns once more, then the PROM waits at "ok".

import * as THREE from 'three';
import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { drawLogo, LOGO_H, LOGO_W } from '../engine/logo.ts';
import { easeOut, hold, smooth, span } from '../engine/math.ts';
import { C } from '../engine/palette.ts';
import { BEAT, kickEnv, section, TAIL } from '../engine/score.ts';
import type { Frame, SceneDef } from '../engine/types.ts';
import { artifacts } from '../data/artifacts.ts';

export const outro: SceneDef = {
    id: 'outro',
    sections: ['outro'],
    create() {
        const a = artifacts();
        const scene = new THREE.Scene();
        scene.background = new THREE.Color('#03070d');
        const cam = new THREE.PerspectiveCamera(35, 16 / 9, 0.1, 50);
        cam.position.set(0, 0, 9);

        // a ring of 256 coefficient bars from the recipient's public key
        const n = 256;
        const ring = new THREE.InstancedMesh(
            new THREE.BoxGeometry(1, 1, 1),
            new THREE.MeshBasicMaterial({ color: '#ffffff', transparent: true }),
            n,
        );
        const m4 = new THREE.Matrix4();
        const col = new THREE.Color();
        for (let i = 0; i < n; i++) {
            const v = (a.kem.pk[i * 3] | ((a.kem.pk[i * 3 + 1] & 15) << 8)) / 3329;
            const ang = (i / n) * Math.PI * 2;
            const len = 0.08 + v * 0.7;
            const q = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 0, 1), ang);
            const p = new THREE.Vector3(
                Math.cos(ang + Math.PI / 2),
                Math.sin(ang + Math.PI / 2),
                0,
            ).multiplyScalar(2.05 + len / 2);
            m4.compose(p, q, new THREE.Vector3(0.018, len, 0.018));
            ring.setMatrixAt(i, m4);
            ring.setColorAt(i, col.set('#1d2c5a').lerp(new THREE.Color(C.phosphor), v * v));
        }
        scene.add(ring);

        const outroBars = section('outro').bars;

        return {
            update(f: Frame, hud: Hud) {
                const bar = f.bar;
                const kick = kickEnv(f.t, 0.3);
                ring.rotation.z = -f.local * 0.08;
                ring.scale.setScalar(1 + 0.02 * kick);
                (ring.material as THREE.MeshBasicMaterial).opacity =
                    0.55 * smooth(span(bar, 0, 1)) * (1 - smooth(span(bar, 4.6, 5.4)));

                const tA = hold(bar, 0.4, 5.3, 0.6, 0.4);
                const rise = easeOut(span(bar, 0.4, 1.4));
                hud.text('Shyake', W / 2, H / 2 + 30 - (1 - rise) * 20, {
                    face: 'serif',
                    size: 190,
                    align: 'center',
                    alpha: tA,
                    glow: 24,
                    color: C.white,
                });
                hud.text('POST-QUANTUM, END-TO-END ENCRYPTED MAIL', W / 2, H / 2 + 100, {
                    face: 'sans',
                    size: 22,
                    spacing: 9,
                    align: 'center',
                    color: C.ice,
                    alpha: hold(bar, 1, 5.3, 0.5, 0.4),
                });
                hud.text('ML-KEM-768  ·  ML-DSA-65  ·  ChaCha20-Poly1305', W / 2, H / 2 + 146, {
                    face: 'mono',
                    size: 22,
                    align: 'center',
                    color: C.fog,
                    alpha: hold(bar, 1.5, 5.3, 0.5, 0.4),
                });
                hud.text('github.com/salmonization/shyake   ·   BSD 2-Clause', W / 2, H - 150, {
                    face: 'sans',
                    size: 20,
                    spacing: 3,
                    align: 'center',
                    color: C.fog,
                    alpha: hold(bar, 2.5, 5.3, 0.5, 0.4),
                });

                // the organisation's mark
                const lA = hold(bar, 3, 5.3, 0.6, 0.4);
                const lh = 54;
                const lw = (LOGO_W / LOGO_H) * lh;
                const label = 'SALMONIZATION';
                const tw = hud.measure(label, 'sansBold', 18, 6);
                const x0 = W / 2 - (lw + 22 + tw) / 2;
                drawLogo(hud.ctx, x0, H - 86, lh, C.ice, lA);
                hud.text(label, x0 + lw + 22, H - 80, {
                    face: 'sansBold',
                    size: 18,
                    spacing: 6,
                    color: C.ice,
                    alpha: lA,
                });

                // back to the PROM prompt
                const end = outroBars + TAIL / (BEAT * 4);
                const okA = span(bar, 5.6, 5.7);
                if (okA > 0) {
                    hud.rect(0, 0, W, H, C.ink, 1);
                    hud.text('ok', 140, 180, {
                        face: 'mono',
                        size: 25,
                        color: C.ice,
                        alpha: okA,
                        glow: 8,
                    });
                    const cx = 140 + hud.measure('ok ', 'mono', 25);
                    if (Math.floor(f.local / (BEAT / 2)) % 2 === 0 && bar < end - 0.15)
                        hud.rect(cx, 180 - 21, 15, 25, C.ice, okA * 0.9);
                }

                return {
                    scene,
                    camera: cam,
                    post: {
                        bloom: 0.6,
                        threshold: 0.4,
                        scan: okA > 0 ? 0.7 : 0.15,
                        curve: okA,
                        aberr: 0.4,
                        grain: 0.3,
                        dither: 0.15,
                        vignette: 0.9,
                        fade:
                            smooth(span(bar, 5.2, 5.55)) * (1 - okA) +
                            smooth(span(bar, end - 0.3, end)),
                    },
                };
            },
        };
    },
};
