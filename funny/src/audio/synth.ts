// The soundtrack, composed in code. Every note comes from the score,
// so the picture can be cut to it without any audio analysis.

import {
    at,
    BAR,
    BEAT,
    CHORDS,
    chordAt,
    CLAPS,
    CX_STEPS,
    DURATION,
    HATS,
    KICKS,
    midiHz,
    section,
    sectionStart,
    STEP,
    type SectionId,
} from '../engine/score.ts';
import { BOOT_LINES, BOOT_TYPED, TERMINAL_KEYS } from '../engine/script.ts';
import { Bus, Noise, panGains, pingPong, polyBlep, reverb, SVF } from './dsp.ts';

export const SR = 48000;

export interface Song {
    sampleRate: number;
    left: Float32Array;
    right: Float32Array;
}

interface Ctx {
    sr: number;
    n: number;
    drums: Bus;
    music: Bus;
    pad: Bus;
    verb: Bus;
    echo: Bus;
}

const TAU = Math.PI * 2;

function range(c: Ctx, t: number, dur: number): [number, number] {
    const s0 = Math.max(0, Math.round(t * c.sr));
    const s1 = Math.min(c.n, Math.round((t + dur) * c.sr));
    return [s0, s1];
}

// Write a mono voice into a bus with a pan and optional sends
function place(
    c: Ctx,
    bus: Bus,
    t: number,
    dur: number,
    pan: number,
    voice: (tau: number, i: number) => number,
    sends: { verb?: number; echo?: number } = {},
) {
    const [s0, s1] = range(c, t, dur);
    const [gl, gr] = panGains(pan);
    const rv = sends.verb ?? 0;
    const ec = sends.echo ?? 0;
    for (let s = s0; s < s1; s++) {
        const x = voice((s - s0) / c.sr, s - s0);
        bus.l[s] += x * gl;
        bus.r[s] += x * gr;
        if (rv) {
            c.verb.l[s] += x * gl * rv;
            c.verb.r[s] += x * gr * rv;
        }
        if (ec) {
            c.echo.l[s] += x * gl * ec;
            c.echo.r[s] += x * gr * ec;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Voices                                                             */
/* ------------------------------------------------------------------ */

function kick(c: Ctx, t: number, vel: number) {
    let ph = 0;
    const nz = new Noise(0x4b1c);
    place(c, c.drums, t, 0.55, 0, (tau) => {
        const f = 44 + 110 * Math.exp(-tau / 0.03);
        ph += (TAU * f) / c.sr;
        const body = Math.sin(ph) * Math.exp(-tau / 0.24) * Math.min(1, tau / 0.002);
        const click = nz.next() * Math.exp(-tau / 0.002) * 0.3;
        return Math.tanh((body * 1.4 + click) * vel) * 0.95;
    });
}

// Six detuned squares, the classic metallic hat, then high-passed
const HAT_F = [263, 400, 421, 474, 587, 845].map((f) => f * 2.6);

function hat(c: Ctx, t: number, open: boolean, vel: number, seed: number) {
    const hp = new SVF();
    hp.set(7800, 0.9, c.sr);
    const nz = new Noise(seed);
    const decay = open ? 0.11 : 0.028;
    place(
        c,
        c.drums,
        t,
        decay * 6,
        open ? 0.25 : -0.2,
        (tau) => {
            let m = 0;
            for (const f of HAT_F) m += Math.sign(Math.sin(TAU * f * tau));
            hp.tick(m * 0.12 + nz.next() * 0.35);
            return hp.high * Math.exp(-tau / decay) * vel * 0.32;
        },
        { verb: 0.08 },
    );
}

function clap(c: Ctx, t: number, seed: number) {
    const bp = new SVF();
    bp.set(1300, 1.4, c.sr);
    const nz = new Noise(seed);
    place(
        c,
        c.drums,
        t - 0.02,
        0.4,
        0.05,
        (tau) => {
            let env = 0;
            for (const o of [0, 0.011, 0.022]) if (tau >= o) env += Math.exp(-(tau - o) / 0.006);
            if (tau >= 0.03) env += 0.6 * Math.exp(-(tau - 0.03) / 0.09);
            bp.tick(nz.next());
            return bp.band * env * 0.55;
        },
        { verb: 0.45 },
    );
}

// CX-52: a pin ratchet, and a heavier lug-cage thunk per letter
function pinClick(c: Ctx, t: number, letter: boolean, i: number) {
    const nz = new Noise(0x52 + i * 7919);
    const hp = new SVF();
    hp.set(2500, 0.7, c.sr);
    const detune = 1 + ((i * 37) % 11) / 90;
    const fs = [2870, 4630, 6310].map((f) => f * detune);
    place(
        c,
        c.drums,
        t,
        0.12,
        ((i % 6) - 2.5) / 5,
        (tau) => {
            hp.tick(nz.next());
            let ping = 0;
            for (let k = 0; k < 3; k++)
                ping += Math.sin(TAU * fs[k] * tau) * Math.exp(-tau / (0.012 - k * 0.003));
            const tick = hp.high * Math.exp(-tau / 0.0015);
            const thunk = letter
                ? Math.sin(TAU * 170 * tau) * Math.exp(-tau / 0.03) * 0.8 +
                  Math.sin(TAU * 1210 * tau) * Math.exp(-tau / 0.02) * 0.25
                : 0;
            return (tick * 0.7 + ping * 0.2 + thunk) * (letter ? 0.55 : 0.32);
        },
        { verb: 0.12 },
    );
}

function keyClick(c: Ctx, t: number, i: number, soft: boolean) {
    const nz = new Noise(0x7e7 + i * 131);
    const bp = new SVF();
    bp.set(3200 + ((i * 53) % 900), 2.2, c.sr);
    place(
        c,
        c.drums,
        t,
        0.05,
        -0.35 + ((i * 17) % 7) / 10,
        (tau) => {
            bp.tick(nz.next());
            const ping = Math.sin(TAU * 1850 * tau) * Math.exp(-tau / 0.006);
            return (bp.band * 1.4 + ping * 0.3) * Math.exp(-tau / 0.008) * (soft ? 0.18 : 0.3);
        },
        { verb: 0.05 },
    );
}

// Workstation power-on beep
function beep(c: Ctx, t: number) {
    const lp = new SVF();
    lp.set(3000, 0.7, c.sr);
    place(
        c,
        c.music,
        t,
        0.9,
        0,
        (tau) => {
            const sq = Math.sin(TAU * 1046.5 * tau) > 0 ? 1 : -1;
            const env =
                Math.min(1, tau / 0.004) * (tau < 0.55 ? 1 : Math.exp(-(tau - 0.55) / 0.04));
            return lp.tick(sq) * env * 0.12;
        },
        { verb: 0.3 },
    );
}

// Bell 202 FSK chatter under each PROM line
function modem(c: Ctx, t: number, seed: number) {
    const nz = new Noise(seed);
    let ph = 0;
    let bit = 0;
    const baud = 1200;
    const lp = new SVF();
    lp.set(2600, 0.8, c.sr);
    place(
        c,
        c.music,
        t,
        0.09,
        0.3,
        (tau, i) => {
            if (i % Math.round(c.sr / baud) === 0) bit = nz.next() > 0 ? 1 : 0;
            ph += (TAU * (bit ? 1200 : 2200)) / c.sr;
            const env = Math.min(1, tau / 0.004) * Math.min(1, (0.09 - tau) / 0.01);
            return lp.tick(Math.sin(ph)) * env * 0.045;
        },
        { echo: 0.3 },
    );
}

function bass(c: Ctx, t: number, note: number, dur: number, vel: number) {
    const f = midiHz(note);
    const dt = f / c.sr;
    let ph = 0;
    const lp = new SVF();
    place(c, c.music, t, dur + 0.05, 0, (tau, i) => {
        if (i % 16 === 0) lp.set(140 + 1300 * Math.exp(-tau / 0.07) * vel, 1.3, c.sr);
        ph += dt;
        if (ph >= 1) ph -= 1;
        const saw = 2 * ph - 1 - polyBlep(ph, dt);
        const sub = Math.sin(TAU * ph);
        const env =
            Math.min(1, tau / 0.003) * Math.exp(-tau / 0.16) * Math.min(1, (dur - tau) / 0.02 + 1);
        return (lp.tick(saw) * 0.55 + sub * 0.45) * Math.max(0, env) * 0.42;
    });
}

// FM tone: carrier plus one modulator, index decays
function fm(
    c: Ctx,
    t: number,
    note: number,
    opts: {
        ratio: number;
        index: number;
        decay: number;
        dur: number;
        gain: number;
        pan: number;
        verb?: number;
        echo?: number;
        vibrato?: number;
    },
) {
    const f = midiHz(note);
    let pc = 0;
    let pm = 0;
    place(
        c,
        c.music,
        t,
        opts.dur,
        opts.pan,
        (tau) => {
            const vib = opts.vibrato
                ? 1 + opts.vibrato * Math.sin(TAU * 5.2 * tau) * Math.min(1, tau / 0.4)
                : 1;
            pm += (TAU * f * opts.ratio) / c.sr;
            const idx = opts.index * Math.exp(-tau / (opts.decay * 0.6));
            pc += (TAU * f * vib) / c.sr;
            const env =
                Math.min(1, tau / 0.004) *
                Math.exp(-tau / opts.decay) *
                Math.min(1, (opts.dur - tau) / 0.05);
            return Math.sin(pc + idx * Math.sin(pm)) * env * opts.gain;
        },
        { verb: opts.verb ?? 0.3, echo: opts.echo ?? 0 },
    );
}

// Pad: two detuned saws and a triangle per chord tone, low-passed
function padChord(
    c: Ctx,
    t: number,
    dur: number,
    tones: number[],
    cutoff: (t: number) => number,
    gain: number,
) {
    tones.forEach((note, k) => {
        const f = midiHz(note + 12);
        const lp = new SVF();
        let p1 = (k * 0.137) % 1;
        let p2 = (k * 0.311) % 1;
        let p3 = 0;
        const d1 = (f * Math.pow(2, 7 / 1200)) / c.sr;
        const d2 = (f * Math.pow(2, -7 / 1200)) / c.sr;
        const d3 = f / 2 / c.sr;
        const pan = k % 2 === 0 ? -0.45 + k * 0.05 : 0.45 - k * 0.05;
        const attack = 0.9;
        const release = 1.4;
        place(
            c,
            c.pad,
            t,
            dur + release,
            pan,
            (tau, i) => {
                if (i % 32 === 0) lp.set(cutoff(t + tau), 0.8, c.sr);
                p1 += d1;
                if (p1 >= 1) p1 -= 1;
                p2 += d2;
                if (p2 >= 1) p2 -= 1;
                p3 += d3;
                if (p3 >= 1) p3 -= 1;
                const s1 = 2 * p1 - 1 - polyBlep(p1, d1);
                const s2 = 2 * p2 - 1 - polyBlep(p2, d2);
                const tri = 1 - 4 * Math.abs(p3 - 0.5);
                const env =
                    Math.min(1, tau / attack) *
                    (tau < dur ? 1 : Math.exp(-(tau - dur) / (release / 3)));
                return lp.tick((s1 + s2) * 0.5 + tri * 0.4) * env * gain;
            },
            { verb: 0.55 },
        );
    });
}

// Filtered noise rising into a downbeat
function riser(c: Ctx, tEnd: number, dur: number, seed: number) {
    const nz = new Noise(seed);
    const bp = new SVF();
    place(
        c,
        c.music,
        tEnd - dur,
        dur + 0.05,
        0,
        (tau, i) => {
            const x = tau / dur;
            if (i % 32 === 0) bp.set(300 + 7000 * x * x, 2.5, c.sr);
            bp.tick(nz.next());
            return bp.band * x * x * 0.3 * Math.min(1, (dur + 0.05 - tau) / 0.03);
        },
        { verb: 0.5 },
    );
}

// Low boom at a section start
function impact(c: Ctx, t: number) {
    let ph = 0;
    place(
        c,
        c.drums,
        t,
        2.5,
        0,
        (tau) => {
            ph += (TAU * (30 + 40 * Math.exp(-tau / 0.15))) / c.sr;
            return Math.sin(ph) * Math.exp(-tau / 0.9) * 0.55;
        },
        { verb: 0.4 },
    );
}

/* ------------------------------------------------------------------ */
/* Arrangement                                                        */
/* ------------------------------------------------------------------ */

const s = (id: SectionId) => section(id);

// Pad filter per section, with the boot opening up and the outro closing
function padCutoff(t: number): number {
    const bar = t / BAR;
    const pts: [number, number][] = [
        [s('boot').bar + 1, 220],
        [s('boot').bar + 8, 1300],
        [s('cx52').bar + 10, 1500],
        [s('rubicon').bar, 650],
        [s('rubicon').bar + 4, 900],
        [s('lattice').bar, 1900],
        [s('chacha').bar, 1600],
        [s('sign').bar, 1800],
        [s('federation').bar, 2800],
        [s('terminal').bar, 1300],
        [s('outro').bar, 1700],
        [s('outro').bar + 6, 300],
    ];
    if (bar <= pts[0][0]) return pts[0][1];
    for (let i = 1; i < pts.length; i++) {
        const [b1, v1] = pts[i];
        const [b0, v0] = pts[i - 1];
        if (bar <= b1) {
            const x = (bar - b0) / (b1 - b0);
            return v0 * Math.pow(v1 / v0, x);
        }
    }
    return pts[pts.length - 1][1];
}

function arrange(c: Ctx) {
    // Drums
    KICKS.forEach((t) => kick(c, t, 1));
    HATS.forEach((h, i) => hat(c, h.t, h.open, h.vel, 0x1a7 + i));
    CLAPS.forEach((t, i) => clap(c, t, 0xc1a9 + i));
    CX_STEPS.forEach((st, i) => pinClick(c, st.t, st.letter, i));

    // Boot: power-on beep, modem chatter, typed command
    beep(c, 0.3);
    BOOT_LINES.forEach((l, i) => modem(c, l.t, 0x3d + i));
    BOOT_TYPED.forEach((k, i) => keyClick(c, k.t, i, k.ch === ' '));
    TERMINAL_KEYS.forEach((k, i) => keyClick(c, k.t, i + 100, k.ch === ' '));

    // Pad: one chord per bar, from bar 1 to the end of the outro
    const last = s('outro').bar + s('outro').bars;
    for (let b = 1; b < last; b++) {
        const inRubicon = b >= s('rubicon').bar && b < s('lattice').bar;
        const gain = b < s('boot').bar + 3 ? 0.05 : inRubicon ? 0.075 : 0.06;
        const tones = inRubicon ? CHORDS[b % 2 === 0 ? 0 : 2].tones : chordAt(b).tones;
        padChord(c, at(b), BAR, tones, padCutoff, gain);
    }

    // Bass: offbeat sixteenths on the root, octave on the last one
    const bassBars: [SectionId, number][] = [
        ['cx52', 4],
        ['lattice', 0],
        ['chacha', 0],
        ['sign', 0],
        ['server', 0],
        ['federation', 0],
        ['terminal', 2],
    ];
    for (const [id, head] of bassBars) {
        const sec = s(id);
        for (let b = sec.bar + head; b < sec.bar + sec.bars; b++) {
            const root = chordAt(b).root;
            for (let beat = 0; beat < 4; beat++)
                for (const st of [1, 2, 3]) {
                    const note = st === 3 && beat % 2 === 1 ? root + 12 : root;
                    bass(c, at(b, beat, st), note, STEP * 0.8, st === 2 ? 1 : 0.6);
                }
        }
    }

    // Arpeggio: FM glass, sixteenths over chord tones
    const ARP = [0, 2, 4, 1, 3, 5, 2, 4, 0, 3, 5, 1, 4, 2, 5, 3];
    const arpBars: [SectionId, number, number][] = [
        ['lattice', 2, 0.055],
        ['sign', 0, 0.04],
        ['federation', 0, 0.06],
    ];
    for (const [id, head, gain] of arpBars) {
        const sec = s(id);
        for (let b = sec.bar + head; b < sec.bar + sec.bars; b++) {
            const tones = chordAt(b).tones;
            for (let st = 0; st < 16; st++) {
                const k = ARP[(st + b * 3) % 16];
                const note = k < 5 ? tones[k] + 12 : tones[0] + 24;
                fm(c, at(b, 0, st), note, {
                    ratio: 3.5,
                    index: 1.6,
                    decay: 0.22,
                    dur: 0.5,
                    gain: gain * (st % 4 === 0 ? 1 : 0.7),
                    pan: st % 2 === 0 ? -0.5 : 0.5,
                    verb: 0.25,
                    echo: 0.35,
                });
            }
        }
    }

    // ChaCha20: one rising tick per round, twenty rounds
    const scale = [62, 64, 65, 67, 69, 70, 72, 74, 76, 77];
    for (let r = 0; r < 20; r++) {
        const t = at(s('chacha').bar, r);
        fm(c, t, scale[r % 10] + (r >= 10 ? 12 : 0), {
            ratio: 1.5,
            index: 2.2,
            decay: 0.18,
            dur: 0.35,
            gain: 0.09,
            pan: r % 2 === 0 ? -0.3 : 0.3,
            echo: 0.25,
        });
    }

    // Rubicon: a low bell on each downbeat, a line of text each
    for (let b = 0; b < 4; b++)
        fm(c, at(s('rubicon').bar + b), [38, 41, 43, 45][b], {
            ratio: 1.41,
            index: 3,
            decay: 1.6,
            dur: 3,
            gain: 0.16,
            pan: 0,
            verb: 0.7,
        });

    // Signature: a stamp every two bars
    for (let b = 2; b < 8; b += 2) {
        const t = at(s('sign').bar + b);
        fm(c, t, 38, { ratio: 2.76, index: 4, decay: 0.3, dur: 0.8, gain: 0.2, pan: 0, verb: 0.5 });
    }

    // Federation lead: slow melody over the climax
    const LEAD: [number, number, number][] = [
        [0, 74, 1.5],
        [1.5, 77, 0.5],
        [2, 76, 2],
        [4, 74, 1.5],
        [5.5, 72, 0.5],
        [6, 69, 2],
        [8, 70, 1.5],
        [9.5, 74, 0.5],
        [10, 72, 2],
        [12, 69, 3],
        [15, 64, 1],
        [16, 74, 1.5],
        [17.5, 77, 0.5],
        [18, 79, 2],
        [20, 77, 1.5],
        [21.5, 76, 0.5],
        [22, 74, 2],
        [24, 70, 1.5],
        [25.5, 72, 0.5],
        [26, 74, 2],
        [28, 69, 4],
    ];
    for (const [beat, note, len] of LEAD)
        fm(c, sectionStart('federation') + beat * BEAT, note, {
            ratio: 1,
            index: 1.1,
            decay: len * BEAT * 0.9,
            dur: len * BEAT + 0.4,
            gain: 0.11,
            pan: 0,
            verb: 0.4,
            echo: 0.3,
            vibrato: 0.004,
        });

    // Outro: sparse bells over the last chords, and a final beep
    const o = s('outro').bar;
    [74, 69, 72, 65, 69, 62].forEach((n, i) =>
        fm(c, at(o + i), n, {
            ratio: 3.5,
            index: 1.2,
            decay: 1.2,
            dur: 2.4,
            gain: 0.07,
            pan: i % 2 === 0 ? -0.4 : 0.4,
            verb: 0.5,
            echo: 0.4,
        }),
    );

    // Transitions
    riser(c, sectionStart('cx52'), BAR * 2, 0x11);
    riser(c, sectionStart('lattice'), BAR * 2, 0x22);
    riser(c, sectionStart('federation'), BAR, 0x33);
    impact(c, sectionStart('lattice'));
    impact(c, sectionStart('federation'));
    impact(c, sectionStart('cx52'));
}

// Master low-pass: the server's view is heard through a wall
function masterCutoff(t: number): number {
    const a = sectionStart('server');
    const b = sectionStart('federation');
    const edge = BEAT;
    if (t < a - edge || t > b) return 20000;
    if (t < a) return 20000 * Math.pow(500 / 20000, (t - (a - edge)) / edge);
    if (t > b - edge * 2) return 500 * Math.pow(20000 / 500, (t - (b - edge * 2)) / (edge * 2));
    return 500;
}

export function renderSong(sr = SR): Song {
    const n = Math.ceil(DURATION * sr);
    const c: Ctx = {
        sr,
        n,
        drums: new Bus(n),
        music: new Bus(n),
        pad: new Bus(n),
        verb: new Bus(n),
        echo: new Bus(n),
    };
    arrange(c);

    // Sidechain the pad against the kick
    const duck = new Float32Array(n).fill(1);
    for (const t of KICKS) {
        const [s0, s1] = range(c, t, 0.45);
        for (let i = s0; i < s1; i++) {
            const tau = (i - s0) / sr;
            duck[i] = Math.min(
                duck[i],
                1 - 0.65 * Math.exp(-tau / 0.12) * Math.min(1, tau / 0.004 + 0.3),
            );
        }
    }

    const out = new Bus(n);
    for (let i = 0; i < n; i++) {
        out.l[i] = c.drums.l[i] + c.music.l[i] + c.pad.l[i] * duck[i];
        out.r[i] = c.drums.r[i] + c.music.r[i] + c.pad.r[i] * duck[i];
    }
    reverb(c.verb, out, sr, { room: 0.86, damp: 0.35, wet: 0.9 });
    pingPong(c.echo, out, sr, { time: BEAT * 0.75, feedback: 0.45, tone: 0.25, wet: 0.5 });

    // Master bus: low-pass automation, soft clip, fade, normalize
    const lpL = new SVF();
    const lpR = new SVF();
    let peak = 0;
    for (let i = 0; i < n; i++) {
        const t = i / sr;
        if (i % 32 === 0) {
            const fc = masterCutoff(t);
            lpL.set(fc, 0.9, sr);
            lpR.set(fc, 0.9, sr);
        }
        const fade = Math.min(1, (DURATION - t) / 1.5);
        const l = Math.tanh(lpL.tick(out.l[i]) * 1.1) * fade;
        const r = Math.tanh(lpR.tick(out.r[i]) * 1.1) * fade;
        out.l[i] = l;
        out.r[i] = r;
        peak = Math.max(peak, Math.abs(l), Math.abs(r));
    }
    const g = peak > 0 ? 0.9 / peak : 1;
    for (let i = 0; i < n; i++) {
        out.l[i] *= g;
        out.r[i] *= g;
    }
    return { sampleRate: sr, left: out.l, right: out.r };
}
