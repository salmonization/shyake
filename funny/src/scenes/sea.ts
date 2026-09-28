// Before morning: a grey sea under mist. A fish passes under the
// surface once, and nothing explains it.

import * as THREE from 'three';
import { captions } from '../engine/captions.ts';
import type { Hud } from '../engine/hud.ts';
import { logoShapes } from '../engine/logo.ts';
import { easeInOut, smooth, span } from '../engine/math.ts';
import { at, BAR, section } from '../engine/score.ts';
import { Sea, sky, waveHeight } from '../engine/sea.ts';
import type { Env, Frame, SceneDef } from '../engine/types.ts';

const s = section('sea').bar;

export const sea: SceneDef = {
    id: 'sea',
    sections: ['sea'],
    create(env: Env) {
        const scene = new THREE.Scene();
        scene.fog = new THREE.Fog('#9ea9ac', 6, 90);
        sky(scene, env.renderer, '#53636c', '#a7b1b3', '#3b4850');
        const water = new Sea(160, 200, '#4e5e66', { opacity: 0.86 });
        scene.add(water.mesh);
        scene.add(new THREE.HemisphereLight('#aeb8ba', '#28323a', 1.3));

        const fish = new THREE.Mesh(
            new THREE.ShapeGeometry(logoShapes(2.2)),
            new THREE.MeshBasicMaterial({ color: '#a9b6ba', transparent: true, depthTest: false }),
        );
        fish.rotation.x = -Math.PI / 2;
        fish.renderOrder = 5;
        scene.add(fish);

        const cam = new THREE.PerspectiveCamera(34, 16 / 9, 0.1, 1000);

        return {
            update(f: Frame, _hud: Hud, cap: Hud) {
                const t = f.t;
                water.update(t);

                // eye level, drifting forward by a hand's width
                const dz = easeInOut(f.local / f.dur) * 0.6;
                cam.position.set(0, 1.5 + waveHeight(0, 6 - dz, t) * 0.15, 6 - dz);
                cam.lookAt(0, 1.05, -40);
                cam.updateMatrixWorld();

                // the fish, right to left, a little below the surface
                const u = span(t, at(s + 1.5), at(s + 4.8));
                fish.position.set(4.2 - u * 8.4, -0.1, -6.5 - u * 0.6);
                fish.rotation.z = 0.08 * Math.sin(t * 1.3);
                (fish.material as THREE.MeshBasicMaterial).opacity =
                    0.2 *
                    Math.min(smooth(u * 5), smooth((1 - u) * 5)) *
                    (0.8 + 0.2 * Math.sin(t * 2.1));

                captions(cap, t, [
                    {
                        from: at(s + 2.5),
                        to: at(s + 5.6),
                        text: 'Before morning, the sea keeps no records.',
                    },
                ]);

                return {
                    scene,
                    camera: cam,
                    post: {
                        grain: 0.4,
                        vignette: 0.5,
                        fade: 1 - smooth(f.local / 2.5),
                        white: smooth(span(f.local, f.dur - BAR * 0.9, f.dur)),
                    },
                };
            },
        };
    },
};
