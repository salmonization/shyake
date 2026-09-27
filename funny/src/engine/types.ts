import type * as THREE from 'three';
import type { Hud } from './hud.ts';
import type { Post } from './post.ts';
import type { SectionId } from './score.ts';

export interface Frame {
    // song time, seconds
    t: number;
    // seconds since this scene began, and its length
    local: number;
    dur: number;
    // bars and beats since this scene began, fractional
    bar: number;
    beat: number;
    section: SectionId;
}

export interface Shot {
    scene?: THREE.Scene;
    camera?: THREE.Camera;
    post?: Post;
}

export interface SceneInstance {
    update(f: Frame, hud: Hud): Shot;
}

export interface Env {
    renderer: THREE.WebGLRenderer;
}

export interface SceneDef {
    id: string;
    sections: SectionId[];
    create(env: Env): SceneInstance;
}
