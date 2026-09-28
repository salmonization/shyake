// Picks the scene for a moment in the song and renders one frame.
// A frame depends on t alone: preview and export look the same.

import * as THREE from 'three';
import { Hud } from './hud.ts';
import { Pipeline } from './post.ts';
import { BAR, BEAT, section, sectionAt } from './score.ts';
import type { Env, SceneDef, SceneInstance } from './types.ts';

interface Slot {
    def: SceneDef;
    inst: SceneInstance;
    start: number;
    dur: number;
}

export class Director {
    renderer: THREE.WebGLRenderer;
    hud: Hud;
    cap: Hud;
    pipe: Pipeline;
    slots: Slot[];

    constructor(canvas: HTMLCanvasElement, defs: SceneDef[], scale = 1) {
        const w = Math.round(1920 * scale);
        const h = Math.round(1080 * scale);
        this.renderer = new THREE.WebGLRenderer({
            canvas,
            antialias: false,
            preserveDrawingBuffer: true,
            powerPreference: 'high-performance',
        });
        this.renderer.setPixelRatio(1);
        this.renderer.setSize(w, h, false);
        this.renderer.toneMapping = THREE.NeutralToneMapping;
        this.renderer.toneMappingExposure = 1;
        this.hud = new Hud(scale);
        this.cap = new Hud(scale);
        this.pipe = new Pipeline(this.renderer, this.hud.canvas, this.cap.canvas, w, h, scale);
        const env: Env = { renderer: this.renderer };
        this.slots = defs.map((def) => {
            const first = section(def.sections[0]);
            const bars = def.sections.reduce((n, id) => n + section(id).bars, 0);
            return { def, inst: def.create(env), start: first.bar * BAR, dur: bars * BAR };
        });
    }

    slotAt(t: number): Slot | undefined {
        const id = sectionAt(t).id;
        return this.slots.find((x) => x.def.sections.includes(id));
    }

    frame(t: number) {
        const slot = this.slotAt(t);
        if (!slot) {
            this.hud.clear();
            this.cap.clear();
            this.pipe.draw(undefined, undefined, {}, t);
            return;
        }
        const local = t - slot.start;
        this.hud.clear();
        this.cap.clear();
        const shot = slot.inst.update(
            {
                t,
                local,
                dur: slot.dur,
                bar: local / BAR,
                beat: local / BEAT,
                section: sectionAt(t).id,
            },
            this.hud,
            this.cap,
        );
        this.pipe.draw(shot.scene, shot.camera, shot.post ?? {}, t);
    }
}
