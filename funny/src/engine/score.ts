// The score: tempo, form, and every timed event the picture and the
// sound share. Both sides read from here, so a kick in the audio and
// a flash on screen can never drift apart.

export const BPM = 112;
export const BEAT = 60 / BPM;
export const BAR = BEAT * 4;
export const STEP = BEAT / 4;

export type SectionId =
    | 'boot'
    | 'cx52'
    | 'rubicon'
    | 'lattice'
    | 'chacha'
    | 'sign'
    | 'server'
    | 'federation'
    | 'terminal'
    | 'outro';

export interface Section {
    id: SectionId;
    bar: number;
    bars: number;
}

// Form, in bars
const FORM: [SectionId, number][] = [
    ['boot', 8],
    ['cx52', 10],
    ['rubicon', 4],
    ['lattice', 10],
    ['chacha', 8],
    ['sign', 8],
    ['server', 4],
    ['federation', 8],
    ['terminal', 8],
    ['outro', 6],
];

export const SECTIONS: Section[] = (() => {
    let bar = 0;
    return FORM.map(([id, bars]) => {
        const s = { id, bar, bars };
        bar += bars;
        return s;
    });
})();

export const TOTAL_BARS = SECTIONS.reduce((n, s) => n + s.bars, 0);
export const TAIL = 2.5;
export const DURATION = TOTAL_BARS * BAR + TAIL;

export function section(id: SectionId): Section {
    const s = SECTIONS.find((x) => x.id === id);
    if (!s) throw new Error(`no section ${id}`);
    return s;
}

export function sectionAt(t: number): Section {
    const bar = t / BAR;
    for (const s of SECTIONS) if (bar < s.bar + s.bars) return s;
    return SECTIONS[SECTIONS.length - 1];
}

// Absolute time of a position given in bars, beats and steps
export function at(bar: number, beat = 0, step = 0): number {
    return bar * BAR + beat * BEAT + step * STEP;
}

export function sectionStart(id: SectionId): number {
    return section(id).bar * BAR;
}

export function sectionEnd(id: SectionId): number {
    const s = section(id);
    return (s.bar + s.bars) * BAR;
}

/* ------------------------------------------------------------------ */
/* Harmony                                                            */
/* ------------------------------------------------------------------ */

// D minor, i - bVI - iv - v, one chord per bar
export const CHORDS: { root: number; tones: number[] }[] = [
    { root: 38, tones: [50, 53, 57, 60, 64] }, // Dm9
    { root: 34, tones: [46, 50, 53, 57, 60] }, // Bbmaj9
    { root: 31, tones: [43, 46, 50, 53, 57] }, // Gm9
    { root: 33, tones: [45, 48, 52, 55, 62] }, // Am7(11)
];

export function chordAt(bar: number) {
    return CHORDS[((Math.floor(bar) % 4) + 4) % 4];
}

export function midiHz(n: number): number {
    return 440 * Math.pow(2, (n - 69) / 12);
}

/* ------------------------------------------------------------------ */
/* Drums                                                              */
/* ------------------------------------------------------------------ */

function bars(from: SectionId, skipHead = 0, skipTail = 0): number[] {
    const s = section(from);
    const out: number[] = [];
    for (let b = s.bar + skipHead; b < s.bar + s.bars - skipTail; b++) out.push(b);
    return out;
}

// Four on the floor wherever the groove runs
export const KICKS: number[] = (() => {
    const out: number[] = [];
    const groove: [SectionId, number, number][] = [
        ['cx52', 2, 0],
        ['lattice', 0, 0],
        ['chacha', 0, 0],
        ['sign', 0, 0],
        ['server', 0, 0],
        ['federation', 0, 0],
        ['terminal', 0, 0],
        ['outro', 0, 4],
    ];
    for (const [id, head, tail] of groove)
        for (const b of bars(id, head, tail))
            for (let beat = 0; beat < 4; beat++) {
                // drop the last beat before a section change
                const s = section(id);
                const lastBar = b === s.bar + s.bars - 1 - tail;
                if (lastBar && beat === 3 && id !== 'server') continue;
                out.push(at(b, beat));
            }
    return out;
})();

export const HATS: { t: number; open: boolean; vel: number }[] = (() => {
    const out: { t: number; open: boolean; vel: number }[] = [];
    const add = (id: SectionId, sixteenths: boolean, head = 0) => {
        for (const b of bars(id, head))
            for (let step = 0; step < 16; step++) {
                const off = step % 4 === 2;
                if (!sixteenths && !off) continue;
                if (sixteenths && step % 4 === 0) continue;
                out.push({ t: at(b, 0, step), open: off, vel: off ? 0.8 : 0.35 });
            }
    };
    add('cx52', false, 6);
    add('lattice', false);
    add('chacha', true);
    add('sign', false);
    add('federation', true);
    add('terminal', false, 2);
    return out;
})();

// Claps on 2 and 4 in the busier sections
export const CLAPS: number[] = (() => {
    const out: number[] = [];
    for (const id of ['chacha', 'federation'] as SectionId[])
        for (const b of bars(id, 2)) {
            out.push(at(b, 1));
            out.push(at(b, 3));
        }
    return out;
})();

// Sidechain envelope: 1 just after a kick, decaying to 0
export function kickEnv(t: number, decay = 0.28): number {
    let lo = 0;
    let hi = KICKS.length - 1;
    if (hi < 0 || t < KICKS[0]) return 0;
    while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if (KICKS[mid] <= t) lo = mid;
        else hi = mid - 1;
    }
    const dt = t - KICKS[lo];
    return Math.exp(-dt / decay) * (dt < 0.6 ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* CX-52: pin wheel steps                                             */
/* ------------------------------------------------------------------ */

// One encipherment per beat, sixteenth ratchets in between. The
// machine starts slowly and gets up to speed over two bars.
export const CX_STEPS: { t: number; letter: boolean }[] = (() => {
    const s = section('cx52');
    const out: { t: number; letter: boolean }[] = [];
    for (let b = s.bar; b < s.bar + s.bars; b++)
        for (let step = 0; step < 16; step++) {
            const rel = b - s.bar;
            if (rel < 1 && step % 4 !== 0) continue;
            if (rel < 2 && step % 2 !== 0) continue;
            out.push({ t: at(b, 0, step), letter: step % 4 === 0 });
        }
    return out;
})();

/* ------------------------------------------------------------------ */
/* Typing, for the boot PROM and the terminal                         */
/* ------------------------------------------------------------------ */

export interface Keystroke {
    t: number;
    ch: string;
}

// Lay out a string one character per step from a start time
export function typeOn(text: string, t0: number, step = STEP): Keystroke[] {
    const out: Keystroke[] = [];
    for (let i = 0; i < text.length; i++) out.push({ t: t0 + i * step, ch: text[i] });
    return out;
}
