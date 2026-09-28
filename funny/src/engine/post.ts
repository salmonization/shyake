// Post-processing: soft bloom, tone mapping, the HUD layer, then a
// finishing pass with grain, vignette, and an ordered dither. With
// `duo` up, the frame becomes a two-tone Bayer print: the archive.

import * as THREE from 'three';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { ShaderPass } from 'three/addons/postprocessing/ShaderPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { DUO } from './palette.ts';

export interface Post {
    // 0..1 to black, and to the paper tone
    fade?: number;
    white?: number;
    bloom?: number;
    threshold?: number;
    grain?: number;
    vignette?: number;
    // two-tone Bayer print: amount, grey levels, cell size in pixels
    duo?: number;
    levels?: number;
    cell?: number;
    // whether the HUD is printed too, or laid over the print
    duoHud?: boolean;
}

const VERT = /* glsl */ `
    varying vec2 vUv;
    void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }
`;

const BAYER = /* glsl */ `
    float bayer(vec2 p) {
        ivec2 i = ivec2(mod(p, 4.0));
        int k = i.x + i.y * 4;
        float m[16] = float[16](0., 8., 2., 10., 12., 4., 14., 6., 3., 11., 1., 9., 15., 7., 13., 5.);
        for (int j = 0; j < 16; j++) if (j == k) return (m[j] + 0.5) / 16.0;
        return 0.5;
    }
`;

const PRINT = /* glsl */ `
    uniform float duo, levels, cell, scale;
    uniform vec3 dark, light;
    vec3 printed(vec3 col, vec2 frag) {
        vec2 c = floor(frag / (cell * scale));
        float l = dot(col, vec3(0.299, 0.587, 0.114));
        l = pow(clamp((l - 0.02) / 0.85, 0.0, 1.0), 0.62);
        float n = max(levels - 1.0, 1.0);
        float q = floor(l * n + bayer(c)) / n;
        return mix(col, mix(dark, light, q), duo);
    }
`;

const HUD_SHADER = {
    uniforms: {
        tDiffuse: { value: null },
        tHud: { value: null },
        tCap: { value: null },
        duo: { value: 0 },
        levels: { value: 4 },
        cell: { value: 2 },
        scale: { value: 1 },
        hudPrint: { value: 0 },
        dark: { value: new THREE.Color(DUO.dark) },
        light: { value: new THREE.Color(DUO.light) },
    },
    vertexShader: VERT,
    fragmentShader: /* glsl */ `
        uniform sampler2D tDiffuse;
        uniform sampler2D tHud;
        uniform sampler2D tCap;
        uniform float hudPrint;
        varying vec2 vUv;
        ${BAYER}
        ${PRINT}
        void main() {
            vec3 base = texture2D(tDiffuse, vUv).rgb;
            vec4 hud = texture2D(tHud, vUv);
            vec3 under = mix(base, hud.rgb, hud.a * hudPrint);
            vec3 col = printed(under, gl_FragCoord.xy);
            col = mix(col, hud.rgb, hud.a * (1.0 - hudPrint));
            vec4 cap = texture2D(tCap, vUv);
            col = mix(col, cap.rgb, cap.a);
            gl_FragColor = vec4(col, 1.0);
        }
    `,
};

const FINISH_SHADER = {
    uniforms: {
        tDiffuse: { value: null },
        time: { value: 0 },
        fade: { value: 0 },
        white: { value: 0 },
        grain: { value: 0 },
        vignette: { value: 0 },
        paper: { value: new THREE.Color(DUO.light) },
    },
    vertexShader: VERT,
    fragmentShader: /* glsl */ `
        uniform sampler2D tDiffuse;
        uniform float time, fade, white, grain, vignette;
        uniform vec3 paper;
        varying vec2 vUv;
        ${BAYER}

        float hash(vec2 p) {
            vec3 p3 = fract(vec3(p.xyx) * 0.1031);
            p3 += dot(p3, p3.yzx + 33.33);
            return fract((p3.x + p3.y) * p3.z);
        }

        void main() {
            vec3 col = texture2D(tDiffuse, vUv).rgb;
            vec2 c = vUv * 2.0 - 1.0;
            float v = smoothstep(1.7, 0.35, length(c * vec2(0.85, 1.0)));
            col *= mix(1.0, v, vignette);
            col = mix(col, paper, white);
            col *= 1.0 - fade;
            float g = hash(gl_FragCoord.xy + fract(time * 7.13) * 1000.0) - 0.5;
            col += g * grain * 0.05;
            col = floor(col * 255.0 + bayer(gl_FragCoord.xy)) / 255.0;
            gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
        }
    `,
};

export class Pipeline {
    composer: EffectComposer;
    render: RenderPass;
    bloom: UnrealBloomPass;
    hud: ShaderPass;
    finish: ShaderPass;
    hudTexture: THREE.CanvasTexture;
    capTexture: THREE.CanvasTexture;
    empty = new THREE.Scene();
    emptyCam = new THREE.PerspectiveCamera();

    constructor(
        renderer: THREE.WebGLRenderer,
        hudCanvas: HTMLCanvasElement,
        capCanvas: HTMLCanvasElement,
        w: number,
        h: number,
        scale: number,
    ) {
        const target = new THREE.WebGLRenderTarget(w, h, {
            type: THREE.HalfFloatType,
            samples: 4,
        });
        this.composer = new EffectComposer(renderer, target);
        this.composer.setPixelRatio(1);
        this.composer.setSize(w, h);
        this.empty.background = new THREE.Color(0x000000);

        this.render = new RenderPass(this.empty, this.emptyCam);
        this.bloom = new UnrealBloomPass(new THREE.Vector2(w / 2, h / 2), 0.3, 0.8, 0.85);
        this.hudTexture = new THREE.CanvasTexture(hudCanvas);
        this.hudTexture.minFilter = THREE.LinearFilter;
        this.hudTexture.generateMipmaps = false;
        this.hud = new ShaderPass(HUD_SHADER);
        this.hud.uniforms.tHud.value = this.hudTexture;
        this.capTexture = new THREE.CanvasTexture(capCanvas);
        this.capTexture.minFilter = THREE.LinearFilter;
        this.capTexture.generateMipmaps = false;
        this.hud.uniforms.tCap.value = this.capTexture;
        this.hud.uniforms.scale.value = scale;
        this.finish = new ShaderPass(FINISH_SHADER);

        this.composer.addPass(this.render);
        this.composer.addPass(this.bloom);
        this.composer.addPass(new OutputPass());
        this.composer.addPass(this.hud);
        this.composer.addPass(this.finish);
    }

    draw(scene: THREE.Scene | undefined, camera: THREE.Camera | undefined, post: Post, t: number) {
        this.render.scene = scene ?? this.empty;
        this.render.camera = camera ?? this.emptyCam;
        this.bloom.strength = post.bloom ?? 0;
        this.bloom.threshold = post.threshold ?? 0.85;
        this.bloom.enabled = (post.bloom ?? 0) > 0;
        const h = this.hud.uniforms;
        h.duo.value = post.duo ?? 0;
        h.levels.value = post.levels ?? 4;
        h.cell.value = post.cell ?? 2;
        h.hudPrint.value = post.duoHud ? 1 : 0;
        const u = this.finish.uniforms;
        u.time.value = t;
        u.fade.value = post.fade ?? 0;
        u.white.value = post.white ?? 0;
        u.grain.value = post.grain ?? 0.35;
        u.vignette.value = post.vignette ?? 0.5;
        this.hudTexture.needsUpdate = true;
        this.capTexture.needsUpdate = true;
        this.composer.render();
    }
}
