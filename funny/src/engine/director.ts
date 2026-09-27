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
    pipe: Pipeline;
    slots: Slot[];

    constructor(canvas: HTMLCanvasElement, defs: SceneDef[], scale = 1) {
        const w = 1920 * scale;
        const h = 1080 * scale;
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
        this.pipe = new Pipeline(this.renderer, this.hud.canvas, w, h);
        const env: Env = { renderer: this.renderer };
        this.slots = defs.map((def) => {
            const first = section(def.sections[0]);
            const bars = def.sections.reduce((n, id) => n + section(id).bars, 0);
            return { def, inst: def.create(env), start: first.bar * BAR, dur: bars * BAR };
        });
    }

    slotAt(t: number): Slot {
        const id = sectionAt(t).id;
        const s = this.slots.find((x) => x.def.sections.includes(id));
        if (!s) throw new Error(`no scene for ${id}`);
        return s;
    }

    frame(t: number) {
        const slot = this.slotAt(t);
        const local = t - slot.start;
        this.hud.clear();
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
        );
        this.pipe.draw(shot.scene, shot.camera, shot.post ?? {}, t);
    }
}
