// Post-processing: bloom, tone mapping, the HUD layer, then a CRT
// pass with barrel distortion, scanlines, grain and ordered dither.

import * as THREE from 'three';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { ShaderPass } from 'three/addons/postprocessing/ShaderPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';

export interface Post {
    fade?: number;
    flash?: number;
    curve?: number;
    scan?: number;
    aberr?: number;
    grain?: number;
    bloom?: number;
    threshold?: number;
    dither?: number;
    vignette?: number;
}

const HUD_SHADER = {
    uniforms: {
        tDiffuse: { value: null },
        tHud: { value: null },
    },
    vertexShader: /* glsl */ `
        varying vec2 vUv;
        void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }
    `,
    fragmentShader: /* glsl */ `
        uniform sampler2D tDiffuse;
        uniform sampler2D tHud;
        varying vec2 vUv;
        void main() {
            vec4 base = texture2D(tDiffuse, vUv);
            vec4 hud = texture2D(tHud, vUv);
            gl_FragColor = vec4(mix(base.rgb, hud.rgb, hud.a), 1.0);
        }
    `,
};

const CRT_SHADER = {
    uniforms: {
        tDiffuse: { value: null },
        res: { value: new THREE.Vector2(1920, 1080) },
        time: { value: 0 },
        fade: { value: 0 },
        flash: { value: 0 },
        curve: { value: 0 },
        scan: { value: 0 },
        aberr: { value: 0 },
        grain: { value: 0 },
        dither: { value: 0 },
        vignette: { value: 0 },
    },
    vertexShader: HUD_SHADER.vertexShader,
    fragmentShader: /* glsl */ `
        uniform sampler2D tDiffuse;
        uniform vec2 res;
        uniform float time, fade, flash, curve, scan, aberr, grain, dither, vignette;
        varying vec2 vUv;

        float hash(vec2 p) {
            vec3 p3 = fract(vec3(p.xyx) * 0.1031);
            p3 += dot(p3, p3.yzx + 33.33);
            return fract((p3.x + p3.y) * p3.z);
        }

        float bayer(vec2 p) {
            ivec2 i = ivec2(mod(p, 4.0));
            int k = i.x + i.y * 4;
            float m[16] = float[16](0., 8., 2., 10., 12., 4., 14., 6., 3., 11., 1., 9., 15., 7., 13., 5.);
            for (int j = 0; j < 16; j++) if (j == k) return m[j] / 16.0;
            return 0.0;
        }

        void main() {
            vec2 c = vUv * 2.0 - 1.0;
            c *= 1.0 + curve * dot(c, c) * 0.06;
            vec2 uv = c * 0.5 + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }
            vec2 dir = c * aberr * 0.004;
            vec3 col;
            col.r = texture2D(tDiffuse, uv + dir).r;
            col.g = texture2D(tDiffuse, uv).g;
            col.b = texture2D(tDiffuse, uv - dir).b;

            // scanlines and slot mask
            float line = 0.5 + 0.5 * cos(uv.y * res.y * 3.14159);
            col *= 1.0 - scan * 0.35 * (1.0 - line);
            float mask = 0.5 + 0.5 * cos(uv.x * res.x * 2.0944);
            col *= 1.0 - scan * 0.08 * (1.0 - mask);

            // vignette and edge falloff of a curved tube
            float v = smoothstep(1.6, 0.4, length(c * vec2(0.9, 1.0)));
            col *= mix(1.0, v, vignette);
            vec2 e = smoothstep(vec2(0.0), vec2(0.012) * curve, uv) * smoothstep(vec2(0.0), vec2(0.012) * curve, 1.0 - uv);
            if (curve > 0.0) col *= e.x * e.y;

            col += flash * vec3(0.78, 0.9, 1.0);
            col *= 1.0 - fade;

            float g = hash(gl_FragCoord.xy + fract(time * 7.13) * 1000.0) - 0.5;
            col += g * grain * 0.08;

            // ordered dither to a reduced palette
            float levels = mix(255.0, 24.0, dither);
            col = floor(col * levels + bayer(gl_FragCoord.xy)) / levels;
            gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
        }
    `,
};

export class Pipeline {
    composer: EffectComposer;
    render: RenderPass;
    bloom: UnrealBloomPass;
    hud: ShaderPass;
    crt: ShaderPass;
    hudTexture: THREE.CanvasTexture;
    empty = new THREE.Scene();
    emptyCam = new THREE.PerspectiveCamera();

    constructor(renderer: THREE.WebGLRenderer, hudCanvas: HTMLCanvasElement, w: number, h: number) {
        const target = new THREE.WebGLRenderTarget(w, h, {
            type: THREE.HalfFloatType,
            samples: 4,
        });
        this.composer = new EffectComposer(renderer, target);
        this.composer.setPixelRatio(1);
        this.composer.setSize(w, h);
        this.empty.background = new THREE.Color(0x000000);

        this.render = new RenderPass(this.empty, this.emptyCam);
        this.bloom = new UnrealBloomPass(new THREE.Vector2(w / 2, h / 2), 0.6, 0.55, 0.8);
        this.hudTexture = new THREE.CanvasTexture(hudCanvas);
        this.hudTexture.minFilter = THREE.LinearFilter;
        this.hudTexture.generateMipmaps = false;
        this.hud = new ShaderPass(HUD_SHADER);
        this.hud.uniforms.tHud.value = this.hudTexture;
        this.crt = new ShaderPass(CRT_SHADER);
        this.crt.uniforms.res.value.set(w, h);

        this.composer.addPass(this.render);
        this.composer.addPass(this.bloom);
        this.composer.addPass(new OutputPass());
        this.composer.addPass(this.hud);
        this.composer.addPass(this.crt);
    }

    draw(scene: THREE.Scene | undefined, camera: THREE.Camera | undefined, post: Post, t: number) {
        this.render.scene = scene ?? this.empty;
        this.render.camera = camera ?? this.emptyCam;
        this.bloom.strength = post.bloom ?? 0.5;
        this.bloom.threshold = post.threshold ?? 0.8;
        this.bloom.enabled = (post.bloom ?? 0.5) > 0;
        const u = this.crt.uniforms;
        u.time.value = t;
        u.fade.value = post.fade ?? 0;
        u.flash.value = post.flash ?? 0;
        u.curve.value = post.curve ?? 0;
        u.scan.value = post.scan ?? 0.25;
        u.aberr.value = post.aberr ?? 0.4;
        u.grain.value = post.grain ?? 0.35;
        u.dither.value = post.dither ?? 0.15;
        u.vignette.value = post.vignette ?? 0.6;
        this.hudTexture.needsUpdate = true;
        this.composer.render();
    }
}
