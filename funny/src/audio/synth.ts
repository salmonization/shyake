// The soundtrack, composed in code: a slow ambient piece in D minor.
// Pad and sea underneath, a sparse FM piano above, and the few real
// sounds the picture asks for. Every note comes from the score.

import { BELLS, PAGE_TURNS } from '../engine/lines.ts';
import {
    at,
    BAR,
    BEAT,
    chordAt,
    CX_STEPS,
    DURATION,
    midiHz,
    PULSES,
    section,
    sectionStart,
    STEP,
    type SectionId,
} from '../engine/score.ts';
import { BOOT_LINES, PGP_TYPED, TERMINAL_KEYS } from '../engine/script.ts';
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
    dry: Bus;
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

// Soft FM piano: a sine lightly modulated, a hammer, a long decay
function piano(c: Ctx, t: number, note: number, vel: number, pan = 0) {
    const f = midiHz(note);
    let pc = 0;
    let pm = 0;
    let pd = 0;
    const dur = 5;
    place(
        c,
        c.dry,
        t,
        dur,
        pan,
        (tau) => {
            pm += (TAU * f) / c.sr;
            pc += (TAU * f) / c.sr;
            pd += (TAU * f * 1.0016) / c.sr;
            const idx = 1.1 * Math.exp(-tau / 0.35) + 0.15;
            const env =
                Math.min(1, tau / 0.006) *
                (0.55 * Math.exp(-tau / 0.5) + 0.45 * Math.exp(-tau / 2.2));
            const x =
                Math.sin(pc + idx * Math.sin(pm)) +
                0.5 * Math.sin(pd + idx * 0.5 * Math.sin(pm * 2));
            return x * env * vel * 0.13 * Math.min(1, (dur - tau) / 0.2);
        },
        { verb: 0.55, echo: 0.22 },
    );
}

// A low bell under each historical sentence
function bell(c: Ctx, t: number, note: number) {
    const f = midiHz(note);
    let pc = 0;
    let pm = 0;
    place(
        c,
        c.dry,
        t,
        7,
        0,
        (tau) => {
            pm += (TAU * f * 1.41) / c.sr;
            pc += (TAU * f) / c.sr;
            const idx = 2.2 * Math.exp(-tau / 1.2);
            const env = Math.min(1, tau / 0.01) * Math.exp(-tau / 2.4);
            return Math.sin(pc + idx * Math.sin(pm)) * env * 0.11 * Math.min(1, (7 - tau) / 0.3);
        },
        { verb: 0.7 },
    );
}

// Pad: two detuned saws and a triangle per chord tone, kept dark
function padChord(
    c: Ctx,
    t: number,
    dur: number,
    tones: number[],
    cutoff: (t: number) => number,
    gain: number,
) {
    tones.forEach((note, k) => {
        const f = midiHz(note);
        const lp = new SVF();
        let p1 = (k * 0.137) % 1;
        let p2 = (k * 0.311) % 1;
        let p3 = 0;
        const d1 = (f * Math.pow(2, 6 / 1200)) / c.sr;
        const d2 = (f * Math.pow(2, -6 / 1200)) / c.sr;
        const d3 = f / 2 / c.sr;
        const pan = k % 2 === 0 ? -0.5 + k * 0.06 : 0.5 - k * 0.06;
        const attack = 2.2;
        const release = 3;
        place(
            c,
            c.pad,
            t,
            dur + release,
            pan,
            (tau, i) => {
                if (i % 64 === 0) lp.set(cutoff(t + tau), 0.7, c.sr);
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
                return lp.tick((s1 + s2) * 0.4 + tri * 0.6) * env * gain;
            },
            { verb: 0.6 },
        );
    });
}

// The sea: two bands of noise that swell and fall with the waves
function sea(c: Ctx, t0: number, t1: number, level: (t: number) => number, seed: number) {
    const nl = new Noise(seed);
    const nr = new Noise(seed * 7 + 1);
    const lpL = new SVF();
    const lpR = new SVF();
    const [s0, s1] = range(c, t0, t1 - t0);
    for (let s = s0; s < s1; s++) {
        const t = s / c.sr;
        const swellL = 0.5 + 0.5 * Math.sin((t / (BAR * 1.5)) * TAU);
        const swellR = 0.5 + 0.5 * Math.sin((t / (BAR * 1.5)) * TAU + 1.9);
        if (s % 64 === 0) {
            lpL.set(300 + 1400 * swellL * swellL, 0.6, c.sr);
            lpR.set(300 + 1400 * swellR * swellR, 0.6, c.sr);
        }
        const edge = Math.min(1, (t - t0) / 3, (t1 - t) / 3);
        const g = level(t) * Math.max(0, edge) * 0.35;
        const l = lpL.tick(nl.next()) * (0.3 + 0.7 * swellL) * g;
        const r = lpR.tick(nr.next()) * (0.3 + 0.7 * swellR) * g;
        c.dry.l[s] += l;
        c.dry.r[s] += r;
        c.verb.l[s] += l * 0.2;
        c.verb.r[s] += r * 0.2;
    }
}

// Tape hiss for the archive
function hiss(c: Ctx, t0: number, t1: number, seed: number) {
    const n = new Noise(seed);
    const hp = new SVF();
    hp.set(5000, 0.7, c.sr);
    place(c, c.dry, t0, t1 - t0, 0, (tau) => {
        hp.tick(n.next());
        const edge = Math.min(1, tau / 2, (t1 - t0 - tau) / 2);
        return hp.high * 0.012 * Math.max(0, edge);
    });
}

// CX-52: a pin ratchet, and a softer lug-cage thunk per letter
function pinClick(c: Ctx, t: number, letter: boolean, i: number) {
    const nz = new Noise(0x52 + i * 7919);
    const hp = new SVF();
    hp.set(2200, 0.7, c.sr);
    const detune = 1 + ((i * 37) % 11) / 90;
    const fs = [2470, 3930, 5310].map((f) => f * detune);
    place(
        c,
        c.dry,
        t,
        0.15,
        ((i % 6) - 2.5) / 8,
        (tau) => {
            hp.tick(nz.next());
            let ping = 0;
            for (let k = 0; k < 3; k++)
                ping += Math.sin(TAU * fs[k] * tau) * Math.exp(-tau / (0.012 - k * 0.003));
            const tick = hp.high * Math.exp(-tau / 0.0015);
            const thunk = letter
                ? Math.sin(TAU * 150 * tau) * Math.exp(-tau / 0.035) * 0.7 +
                  Math.sin(TAU * 980 * tau) * Math.exp(-tau / 0.02) * 0.2
                : 0;
            return (tick * 0.6 + ping * 0.15 + thunk) * (letter ? 0.28 : 0.16);
        },
        { verb: 0.25 },
    );
}

function keyClick(c: Ctx, t: number, i: number, soft: boolean) {
    const nz = new Noise(0x7e7 + i * 131);
    const bp = new SVF();
    bp.set(2600 + ((i * 53) % 700), 2, c.sr);
    place(
        c,
        c.dry,
        t,
        0.05,
        -0.2 + ((i * 17) % 5) / 10,
        (tau) => {
            bp.tick(nz.next());
            return bp.band * Math.exp(-tau / 0.007) * (soft ? 0.08 : 0.14);
        },
        { verb: 0.1 },
    );
}

// Workstation self-test beep, soft
function beep(c: Ctx, t: number) {
    place(
        c,
        c.dry,
        t,
        0.6,
        0,
        (tau) => {
            const env = Math.min(1, tau / 0.01) * (tau < 0.3 ? 1 : Math.exp(-(tau - 0.3) / 0.05));
            return Math.sin(TAU * 880 * tau) * env * 0.05;
        },
        { verb: 0.4 },
    );
}

// A disk seek, a dull tick
function seek(c: Ctx, t: number, i: number) {
    const nz = new Noise(0xd15c + i);
    const bp = new SVF();
    bp.set(900, 3, c.sr);
    place(c, c.dry, t, 0.06, 0.3, (tau) => {
        bp.tick(nz.next());
        return bp.band * Math.exp(-tau / 0.012) * 0.12;
    });
}

// A page turning: a swell of filtered air
function page(c: Ctx, t: number, i: number) {
    const nz = new Noise(0x9a9e + i);
    const bp = new SVF();
    const dur = 0.9;
    place(
        c,
        c.dry,
        t,
        dur,
        -0.2 + i * 0.1,
        (tau, k) => {
            const x = tau / dur;
            if (k % 32 === 0) bp.set(1200 + 2500 * x, 0.9, c.sr);
            bp.tick(nz.next());
            return bp.band * Math.sin(Math.PI * x) ** 2 * 0.1;
        },
        { verb: 0.3 },
    );
}

// Heartbeat: a soft low thump
function thump(c: Ctx, t: number, strong: boolean) {
    let ph = 0;
    place(c, c.dry, t, 0.5, 0, (tau) => {
        ph += (TAU * (46 + 30 * Math.exp(-tau / 0.03))) / c.sr;
        return (
            Math.sin(ph) * Math.exp(-tau / 0.13) * Math.min(1, tau / 0.004) * (strong ? 0.28 : 0.17)
        );
    });
}

/* ------------------------------------------------------------------ */
/* Arrangement                                                        */
/* ------------------------------------------------------------------ */

const sec = (id: SectionId) => section(id);

// Pad brightness through the film
function padCutoff(t: number): number {
    const bar = t / BAR;
    const pts: [SectionId, number, number][] = [
        ['sea', 0, 380],
        ['boot', 0, 620],
        ['cx52', 0, 760],
        ['cryptoag', 0, 420],
        ['pgp', 8, 480],
        ['tide', 0, 1000],
        ['river', 0, 1300],
        ['desk', 0, 780],
        ['title', 0, 900],
        ['title', 4, 500],
    ];
    const xs = pts.map(([id, off, v]) => [sec(id).bar + off, v] as const);
    if (bar <= xs[0][0]) return xs[0][1];
    for (let i = 1; i < xs.length; i++) {
        const [b1, v1] = xs[i];
        const [b0, v0] = xs[i - 1];
        if (bar <= b1) return v0 * Math.pow(v1 / v0, (bar - b0) / (b1 - b0));
    }
    return xs[xs.length - 1][1];
}

// A short phrase for the piano, in beats and notes
const MOTIF: [number, number][] = [
    [0, 74],
    [1.5, 69],
    [2, 72],
    [4, 65],
    [6, 67],
    [7, 69],
];

function motif(c: Ctx, bar: number, transpose: number, vel: number) {
    MOTIF.forEach(([beat, note], i) =>
        piano(
            c,
            at(bar) + beat * BEAT,
            note + transpose,
            vel * (i === 0 ? 1 : 0.8),
            (i % 2) * 0.4 - 0.2,
        ),
    );
}

function arrange(c: Ctx) {
    const end = sec('title').bar + sec('title').bars;

    // Pad: two bars per chord, all the way through
    for (let b = 0; b < end; b += 2) {
        const quiet = b >= sec('pgp').bar + 9 && b < sec('tide').bar;
        padChord(c, at(b), BAR * 2, chordAt(b).tones, padCutoff, quiet ? 0.03 : 0.045);
    }

    // Sea wherever the sea is in the picture
    sea(c, 0, sectionStart('boot') + BAR, () => 1, 0x5ea);
    sea(
        c,
        at(sec('pgp').bar + 8.5),
        sectionStart('river') + BAR,
        (t) => (t < sectionStart('tide') ? 0.5 : 0.8),
        0x5eb,
    );
    sea(c, sectionStart('river'), sectionStart('desk') + 1, () => 0.6, 0x5ec);
    hiss(c, sectionStart('cryptoag'), at(sec('pgp').bar + 9), 0x415);

    // Piano: the phrase, sparsely, and single notes in between
    motif(c, sec('sea').bar + 2, 0, 0.7);
    motif(c, sec('cx52').bar + 2, -5, 0.6);
    motif(c, sec('tide').bar + 1, 0, 0.8);
    motif(c, sec('tide').bar + 5, 3, 0.7);
    motif(c, sec('river').bar + 1, 0, 0.9);
    motif(c, sec('river').bar + 3, 5, 0.8);
    for (const [b, n] of [
        [sec('boot').bar + 4, 69],
        [sec('pgp').bar + 4, 62],
        [sec('pgp').bar + 6, 65],
        [sec('desk').bar + 1, 74],
        [sec('desk').bar + 3, 72],
    ] as const)
        piano(c, at(b), n, 0.6);

    // Boot: the self-test beep and the disk
    beep(c, at(sec('boot').bar, 2));
    BOOT_LINES.forEach((l, i) => seek(c, l.t, i));

    // CX-52
    CX_STEPS.forEach((s, i) => pinClick(c, s.t, s.letter, i));

    // History: a bell under each sentence, keys and pages for PGP
    const bellNotes = [38, 41, 36, 43, 38, 45, 41];
    BELLS.forEach((t, i) => bell(c, t, bellNotes[i % bellNotes.length]));
    PGP_TYPED.forEach((k, i) => keyClick(c, k.t, i, k.ch === ' '));
    PAGE_TURNS.forEach((t, i) => page(c, t, i));

    // Tide: slow glass, one note an eighth
    const ARP = [0, 2, 4, 1, 3, 2, 4, 0];
    for (let b = sec('tide').bar + 2; b < sec('river').bar; b++) {
        const tones = chordAt(b).tones;
        for (let e = 0; e < 8; e++)
            if ((e + b) % 3 !== 2)
                piano(c, at(b, 0, e * 2), tones[ARP[e]] + 12, 0.28, e % 2 ? 0.5 : -0.5);
    }

    // River: a heartbeat
    PULSES.forEach((t, i) => thump(c, t, i % 2 === 0));

    // Desk: typing
    TERMINAL_KEYS.forEach((k, i) => keyClick(c, k.t, i + 200, k.ch === ' '));

    // Title, and the mark
    bell(c, sectionStart('title'), 38);
    [62, 65, 69, 74].forEach((n, i) =>
        piano(c, sectionStart('title') + i * STEP * 1.5, n, 0.5, i * 0.2 - 0.3),
    );
    piano(c, at(sec('title').bar + 1.75), 81, 0.45);
}

export function renderSong(sr = SR): Song {
    const n = Math.ceil(DURATION * sr);
    const c: Ctx = { sr, n, dry: new Bus(n), pad: new Bus(n), verb: new Bus(n), echo: new Bus(n) };
    arrange(c);

    const out = new Bus(n);
    for (let i = 0; i < n; i++) {
        out.l[i] = c.dry.l[i] + c.pad.l[i];
        out.r[i] = c.dry.r[i] + c.pad.r[i];
    }
    reverb(c.verb, out, sr, { room: 0.9, damp: 0.45, wet: 0.9 });
    pingPong(c.echo, out, sr, { time: BEAT * 0.75, feedback: 0.4, tone: 0.2, wet: 0.35 });

    // Master: soft clip, a short fade at the very end, normalize
    let peak = 0;
    for (let i = 0; i < n; i++) {
        const t = i / sr;
        const fade = Math.min(1, (DURATION - t) / 0.6, t / 0.05);
        out.l[i] = Math.tanh(out.l[i] * 1.2) * fade;
        out.r[i] = Math.tanh(out.r[i] * 1.2) * fade;
        peak = Math.max(peak, Math.abs(out.l[i]), Math.abs(out.r[i]));
    }
    const g = peak > 0 ? 0.8 / peak : 1;
    for (let i = 0; i < n; i++) {
        out.l[i] *= g;
        out.r[i] *= g;
    }
    return { sampleRate: sr, left: out.l, right: out.r };
}
