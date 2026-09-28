// PGP: a terminal in 1994 prints the export warning; a book of source
// code turns its pages; then a sentence from 1991 over the sea, while
// the archive's dither slowly lets go of the picture.

import * as THREE from 'three';
import { captions } from '../engine/captions.ts';
import type { Hud } from '../engine/hud.ts';
import { easeInOut, easeOut, lerp, smooth, span } from '../engine/math.ts';
import { MOTIF } from '../engine/palette.ts';
import { at, BEAT, section } from '../engine/score.ts';
import { PAGE_TURNS, PGP_CAPTIONS } from '../engine/lines.ts';
import { PGP_LINES, PGP_TYPED } from '../engine/script.ts';
import { Sea, sky, waveHeight } from '../engine/sea.ts';
import { canvas, toTexture } from '../engine/textures.ts';
import type { Env, Frame, SceneDef, Shot } from '../engine/types.ts';
import { clock, desktop } from '../engine/widgets.ts';
import crypto from '../../../client/src/lib/crypto_ops.c?raw';
import mail from '../../../client/src/lib/mail.c?raw';

const p = section('pgp').bar;
const BOOK_FROM = 4.15;
const SEA_FROM = 8.15;

/* ------------------------------------------------------------------ */
/* The book                                                           */
/* ------------------------------------------------------------------ */

const SOURCE = (crypto + '\n' + mail).replace(/\t/g, '    ').split('\n');
const PW = 1.45;
const PH = 2.05;
const ROWS = 46;

// A page of source, each line led by a checksum, as scanned books were
function pageTexture(n: number): THREE.CanvasTexture {
    const [c, ctx] = canvas(1024, 1448);
    ctx.fillStyle = '#e3e4de';
    ctx.fillRect(0, 0, 1024, 1448);
    ctx.fillStyle = '#23292c';
    ctx.font = '400 19px "FreeMono"';
    for (let i = 0; i < ROWS; i++) {
        const line = SOURCE[(n * ROWS + i) % SOURCE.length].slice(0, 68);
        let sum = 0;
        for (let k = 0; k < line.length; k++) sum = (sum * 31 + line.charCodeAt(k)) & 0xffff;
        ctx.fillStyle = '#6b7478';
        ctx.fillText(sum.toString(16).padStart(4, '0'), 70, 120 + i * 27);
        ctx.fillStyle = '#23292c';
        ctx.fillText(line, 150, 120 + i * 27);
    }
    ctx.fillStyle = '#6b7478';
    ctx.font = '400 20px "FreeMono"';
    ctx.textAlign = 'center';
    ctx.fillText(String(212 + n), 512, 1400);
    return toTexture(c);
}

function book(env: Env) {
    const scene = new THREE.Scene();
    scene.background = new THREE.Color('#1c2327');
    scene.fog = new THREE.Fog('#1c2327', 6, 16);
    const desk = new THREE.Mesh(
        new THREE.PlaneGeometry(30, 30),
        new THREE.MeshStandardMaterial({ color: '#3a4449', roughness: 0.8 }),
    );
    desk.rotation.x = -Math.PI / 2;
    desk.receiveShadow = true;
    scene.add(desk);
    env.renderer.shadowMap.enabled = true;

    const pages = Array.from({ length: 12 }, (_, i) => pageTexture(i));
    const paper = (map: THREE.Texture) => new THREE.MeshStandardMaterial({ map, roughness: 0.9 });
    const edge = new THREE.MeshStandardMaterial({ color: '#cfd1ca', roughness: 0.95 });
    const cover = new THREE.MeshStandardMaterial({ color: '#2f3a40', roughness: 0.7 });

    // two stacks and the cover under them
    const coverMesh = new THREE.Mesh(new THREE.BoxGeometry(PW * 2 + 0.12, 0.03, PH + 0.1), cover);
    coverMesh.position.y = 0.015;
    coverMesh.castShadow = coverMesh.receiveShadow = true;
    scene.add(coverMesh);
    const left = new THREE.Mesh(new THREE.BoxGeometry(PW, 0.1, PH), [
        edge,
        edge,
        paper(pages[0]),
        edge,
        edge,
        edge,
    ]);
    const right = new THREE.Mesh(new THREE.BoxGeometry(PW, 0.1, PH), [
        edge,
        edge,
        paper(pages[1]),
        edge,
        edge,
        edge,
    ]);
    left.position.set(-PW / 2, 0.08, 0);
    right.position.set(PW / 2, 0.08, 0);
    for (const m of [left, right]) {
        m.castShadow = m.receiveShadow = true;
        scene.add(m);
    }

    // the turning page: a bent plane, front and back
    const geo = new THREE.PlaneGeometry(PW, PH, 32, 1);
    geo.rotateX(-Math.PI / 2);
    const base = Float32Array.from(geo.getAttribute('position').array as Float32Array);
    const front = new THREE.Mesh(
        geo,
        new THREE.MeshStandardMaterial({ map: pages[1], roughness: 0.9, side: THREE.FrontSide }),
    );
    const back = new THREE.Mesh(
        geo,
        new THREE.MeshStandardMaterial({ map: pages[2], roughness: 0.9, side: THREE.BackSide }),
    );
    scene.add(front, back);

    const key = new THREE.DirectionalLight('#dfe6e8', 1.7);
    key.position.set(-4, 6, 2);
    key.castShadow = true;
    key.shadow.mapSize.set(2048, 2048);
    Object.assign(key.shadow.camera, { left: -4, right: 4, top: 4, bottom: -4 });
    scene.add(key);
    scene.add(new THREE.HemisphereLight('#8997a0', '#1c2327', 0.9));

    function setPage(t: number) {
        let turned = 0;
        let theta = 0;
        for (const tt of PAGE_TURNS) {
            if (t >= tt + BEAT * 2.5) turned++;
            else if (t >= tt) theta = easeInOut((t - tt) / (BEAT * 2.5)) * Math.PI;
        }
        const k = turned * 2;
        (left.material as THREE.Material[])[2] = paper(pages[k % 12]);
        (right.material as THREE.Material[])[2] = paper(pages[(k + 1) % 12]);
        (front.material as THREE.MeshStandardMaterial).map = pages[(k + 1) % 12];
        (back.material as THREE.MeshStandardMaterial).map = pages[(k + 2) % 12];
        front.visible = back.visible = theta > 0;

        const pos = geo.getAttribute('position') as THREE.BufferAttribute;
        const a = pos.array as Float32Array;
        for (let i = 0; i < a.length; i += 3) {
            const d = base[i] + PW / 2;
            const bend = theta + 0.45 * Math.sin(theta) * (d / PW);
            a[i] = Math.cos(bend) * d;
            a[i + 1] = 0.135 + Math.sin(bend) * d * 0.98;
            a[i + 2] = base[i + 2];
        }
        pos.needsUpdate = true;
        geo.computeVertexNormals();
    }
    return { scene, setPage };
}

function seaAtDusk(env: Env) {
    const scene = new THREE.Scene();
    scene.fog = new THREE.Fog('#7f8b90', 6, 90);
    sky(scene, env.renderer, '#3e4b53', '#8a969a', '#2d383f');
    const water = new Sea(160, 200, '#46555d');
    scene.add(water.mesh);
    scene.add(new THREE.HemisphereLight('#9aa6aa', '#222c32', 1.1));
    return { scene, water };
}

/* ------------------------------------------------------------------ */
/* The terminal, 1994                                                 */
/* ------------------------------------------------------------------ */

function terminal(hud: Hud, t: number) {
    desktop(hud);
    const win = hud.window(250, 170, 1300, 560, 'xterm', {});
    let typed = '';
    for (const k of PGP_TYPED) if (t >= k.t) typed += k.ch;
    const rows = ['% ' + typed];
    for (const l of PGP_LINES) if (t >= l.t) rows.push(l.text);
    if (t >= PGP_LINES[PGP_LINES.length - 1].t + BEAT * 2) rows.push('');
    rows.forEach((r, i) =>
        hud.text(r, win.x + 22, win.y + 44 + i * 34, {
            face: 'mono',
            size: 23,
            color: r.startsWith('Export') ? '#ffffff' : MOTIF.text,
        }),
    );
    const last = rows[rows.length - 1];
    if (Math.floor(t / (BEAT / 2)) % 2 === 0)
        hud.rect(
            win.x + 22 + last.length * 23 * 0.6,
            win.y + 44 + (rows.length - 1) * 34 - 19,
            14,
            22,
            MOTIF.text,
            0.85,
        );
    const ck = hud.window(1590, 170, 230, 220, 'xclock', { active: false });
    // 11 October 1994, a little after ten at night
    clock(hud, ck, 22 * 3600 + 14 * 60 + (t - at(p)), 1);
}

export const pgp: SceneDef = {
    id: 'pgp',
    sections: ['pgp'],
    create(env: Env) {
        const bk = book(env);
        const sd = seaAtDusk(env);
        const cam = new THREE.PerspectiveCamera(32, 16 / 9, 0.05, 1000);

        return {
            update(f: Frame, hud: Hud, cap: Hud): Shot {
                const bar = f.bar;
                const t = f.t;
                captions(cap, t, PGP_CAPTIONS);

                if (bar < BOOK_FROM) {
                    terminal(hud, t);
                    const edge = Math.min(smooth(f.local / 1), smooth((BOOK_FROM - bar) / 0.18));
                    return {
                        post: {
                            duo: 1,
                            duoHud: true,
                            levels: 5,
                            cell: 2,
                            grain: 0.25,
                            vignette: 0.6,
                            fade: 1 - edge,
                        },
                    };
                }

                if (bar < SEA_FROM) {
                    bk.setPage(t);
                    const u = easeInOut(span(bar, BOOK_FROM, SEA_FROM));
                    cam.position.set(lerp(0.5, 0.2, u), lerp(2.9, 2.5, u), lerp(2.9, 2.5, u));
                    cam.lookAt(0, 0.1, lerp(0.5, 0.55, u));
                    cam.updateMatrixWorld();
                    const edge = Math.min(
                        smooth((bar - BOOK_FROM) / 0.18),
                        smooth((SEA_FROM - bar) / 0.18),
                    );
                    return {
                        scene: bk.scene,
                        camera: cam,
                        post: {
                            duo: 1,
                            levels: 5,
                            cell: 2,
                            grain: 0.25,
                            vignette: 0.7,
                            fade: 1 - edge,
                        },
                    };
                }

                // the sea at dusk, the dither letting go
                sd.water.update(t);
                const u = span(bar, SEA_FROM, section('pgp').bars);
                cam.position.set(0, 1.5 + waveHeight(0, 6, t) * 0.12, 6 - u * 0.3);
                cam.lookAt(0, 1.1, -40);
                if (cam.fov !== 34) {
                    cam.fov = 34;
                    cam.updateProjectionMatrix();
                }
                cam.updateMatrixWorld();
                const release = easeOut(span(bar, 9.2, 11), 2);
                return {
                    scene: sd.scene,
                    camera: cam,
                    post: {
                        duo: 1 - release,
                        levels: lerp(5, 16, release),
                        cell: 2,
                        grain: 0.35,
                        vignette: 0.55,
                        fade: 1 - smooth((bar - SEA_FROM) / 0.3),
                    },
                };
            },
        };
    },
};
