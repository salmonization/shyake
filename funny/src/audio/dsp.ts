// DSP building blocks. Plain sample loops, no Web Audio, so the same
// code renders the soundtrack in the browser and in Node.

export class Bus {
    l: Float32Array;
    r: Float32Array;
    constructor(n: number) {
        this.l = new Float32Array(n);
        this.r = new Float32Array(n);
    }
}

// Equal-power pan, p in [-1, 1]
export function panGains(p: number): [number, number] {
    const a = ((p + 1) * Math.PI) / 4;
    return [Math.cos(a), Math.sin(a)];
}

export class Noise {
    s: number;
    constructor(seed: number) {
        this.s = seed >>> 0 || 1;
    }
    next(): number {
        let x = this.s;
        x ^= x << 13;
        x ^= x >>> 17;
        x ^= x << 5;
        this.s = x >>> 0;
        return (this.s / 4294967296) * 2 - 1;
    }
}

// Topology-preserving state variable filter (Zavalishin)
export class SVF {
    ic1 = 0;
    ic2 = 0;
    a1 = 0;
    a2 = 0;
    a3 = 0;
    k = 1;
    low = 0;
    band = 0;
    high = 0;
    set(fc: number, q: number, sr: number) {
        const g = Math.tan((Math.PI * Math.min(fc, sr * 0.45)) / sr);
        this.k = 1 / q;
        this.a1 = 1 / (1 + g * (g + this.k));
        this.a2 = g * this.a1;
        this.a3 = g * this.a2;
    }
    tick(v0: number): number {
        const v3 = v0 - this.ic2;
        const v1 = this.a1 * this.ic1 + this.a2 * v3;
        const v2 = this.ic2 + this.a2 * this.ic1 + this.a3 * v3;
        this.ic1 = 2 * v1 - this.ic1;
        this.ic2 = 2 * v2 - this.ic2;
        this.low = v2;
        this.band = v1;
        this.high = v0 - this.k * v1 - v2;
        return v2;
    }
}

// Band-limited sawtooth step correction
export function polyBlep(t: number, dt: number): number {
    if (t < dt) {
        const x = t / dt;
        return x + x - x * x - 1;
    }
    if (t > 1 - dt) {
        const x = (t - 1) / dt;
        return x * x + x + x + 1;
    }
    return 0;
}

// Freeverb-style stereo reverb, processed in place into `out`
export function reverb(
    src: Bus,
    out: Bus,
    sr: number,
    opts: { room: number; damp: number; wet: number },
) {
    const scale = sr / 44100;
    const combT = [1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617];
    const apT = [556, 441, 341, 225];
    const spread = 23;
    for (const [side, input, dest] of [
        [0, src.l, out.l],
        [1, src.r, out.r],
    ] as const) {
        const combs = combT.map((c) => {
            const len = Math.round((c + side * spread) * scale);
            return { buf: new Float32Array(len), i: 0, store: 0 };
        });
        const aps = apT.map((a) => {
            const len = Math.round((a + side * spread) * scale);
            return { buf: new Float32Array(len), i: 0 };
        });
        const fb = opts.room;
        const d1 = opts.damp;
        const d2 = 1 - d1;
        const n = input.length;
        for (let s = 0; s < n; s++) {
            const x = input[s] * 0.015;
            let acc = 0;
            for (const c of combs) {
                const y = c.buf[c.i];
                c.store = y * d2 + c.store * d1;
                c.buf[c.i] = x + c.store * fb;
                if (++c.i >= c.buf.length) c.i = 0;
                acc += y;
            }
            for (const a of aps) {
                const y = a.buf[a.i];
                a.buf[a.i] = acc + y * 0.5;
                acc = y - acc;
                if (++a.i >= a.buf.length) a.i = 0;
            }
            dest[s] += acc * opts.wet;
        }
    }
}

// Ping-pong delay with a darkening feedback path
export function pingPong(
    src: Bus,
    out: Bus,
    sr: number,
    opts: { time: number; feedback: number; tone: number; wet: number },
) {
    const len = Math.round(opts.time * sr);
    const bl = new Float32Array(len);
    const br = new Float32Array(len);
    let i = 0;
    let lpL = 0;
    let lpR = 0;
    const a = opts.tone;
    const n = src.l.length;
    for (let s = 0; s < n; s++) {
        const yl = bl[i];
        const yr = br[i];
        lpL += a * (yl - lpL);
        lpR += a * (yr - lpR);
        bl[i] = (src.l[s] + src.r[s]) * 0.5 + lpR * opts.feedback;
        br[i] = lpL * opts.feedback;
        if (++i >= len) i = 0;
        out.l[s] += yl * opts.wet;
        out.r[s] += yr * opts.wet;
    }
}
