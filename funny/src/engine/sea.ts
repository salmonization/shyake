// The sea and its sky. Waves are a sum of slow swells evaluated on
// the CPU, so every frame is a function of time alone.

import * as THREE from 'three';

const WAVES: [number, number, number, number, number][] = [
    // direction x, direction z, wavelength, amplitude, speed
    [1, 0.3, 14, 0.12, 0.35],
    [0.6, 1, 7, 0.06, 0.5],
    [-0.4, 1, 3.6, 0.03, 0.7],
    [1, -0.7, 2.1, 0.015, 0.9],
];

export function waveHeight(x: number, z: number, t: number, amp = 1): number {
    let h = 0;
    for (const [dx, dz, len, a, sp] of WAVES) {
        const n = Math.hypot(dx, dz);
        const k = (Math.PI * 2) / len;
        h += a * Math.sin(((dx * x + dz * z) / n) * k - t * sp * k * 0.4 + len);
    }
    return h * amp;
}

export class Sea {
    mesh: THREE.Mesh;
    material: THREE.MeshStandardMaterial;
    private pos: THREE.BufferAttribute;
    private base: Float32Array;
    private geo: THREE.PlaneGeometry;

    constructor(size: number, segs: number, color: string, opts: { opacity?: number } = {}) {
        this.geo = new THREE.PlaneGeometry(size, size, segs, segs);
        this.geo.rotateX(-Math.PI / 2);
        this.pos = this.geo.getAttribute('position') as THREE.BufferAttribute;
        this.base = Float32Array.from(this.pos.array as Float32Array);
        this.material = new THREE.MeshStandardMaterial({
            color,
            roughness: 0.3,
            metalness: 0.05,
            transparent: opts.opacity !== undefined,
            opacity: opts.opacity ?? 1,
        });
        this.mesh = new THREE.Mesh(this.geo, this.material);
        this.mesh.receiveShadow = true;
    }

    update(t: number, amp = 1) {
        const a = this.pos.array as Float32Array;
        for (let i = 0; i < a.length; i += 3)
            a[i + 1] = waveHeight(this.base[i], this.base[i + 2], t, amp);
        this.pos.needsUpdate = true;
        this.geo.computeVertexNormals();
    }
}

// A gradient sky dome, also used as the environment for reflections
export function sky(
    scene: THREE.Scene,
    renderer: THREE.WebGLRenderer,
    top: string,
    horizon: string,
    below: string,
): THREE.Mesh {
    const mat = new THREE.ShaderMaterial({
        side: THREE.BackSide,
        depthWrite: false,
        fog: false,
        uniforms: {
            top: { value: new THREE.Color(top) },
            horizon: { value: new THREE.Color(horizon) },
            below: { value: new THREE.Color(below) },
        },
        vertexShader: /* glsl */ `
            varying vec3 vDir;
            void main() {
                vDir = normalize(position);
                gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
            }
        `,
        fragmentShader: /* glsl */ `
            uniform vec3 top, horizon, below;
            varying vec3 vDir;
            void main() {
                float y = vDir.y;
                vec3 c = y > 0.0 ? mix(horizon, top, pow(y, 0.55)) : mix(horizon, below, pow(-y, 0.4));
                gl_FragColor = vec4(c, 1.0);
                #include <colorspace_fragment>
            }
        `,
    });
    const dome = new THREE.Mesh(new THREE.SphereGeometry(400, 32, 16), mat);
    const envScene = new THREE.Scene();
    envScene.add(dome.clone());
    const pmrem = new THREE.PMREMGenerator(renderer);
    scene.environment = pmrem.fromScene(envScene, 0).texture;
    scene.add(dome);
    return dome;
}
