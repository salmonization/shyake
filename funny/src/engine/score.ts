// The score: tempo, form, and the timed events the picture and the
// sound share. Slow and sparse: this is weather, not a dance floor.

export const BPM = 72;
export const BEAT = 60 / BPM;
export const BAR = BEAT * 4;
export const STEP = BEAT / 4;

export type SectionId =
    | 'sea'
    | 'boot'
    | 'cx52'
    | 'cryptoag'
    | 'pgp'
    | 'tide'
    | 'river'
    | 'desk'
    | 'title';

export interface Section {
    id: SectionId;
    bar: number;
    bars: number;
}

// Form, in bars
const FORM: [SectionId, number][] = [
    ['sea', 6],
    ['boot', 6],
    ['cx52', 7],
    ['cryptoag', 14],
    ['pgp', 11],
    ['tide', 8],
    ['river', 6],
    ['desk', 5],
    ['title', 4],
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
export const DURATION = TOTAL_BARS * BAR;

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

// D minor, two bars per chord: i - bVI - iv - v
export const CHORDS: { root: number; tones: number[] }[] = [
    { root: 38, tones: [50, 53, 57, 60, 64] }, // Dm9
    { root: 34, tones: [46, 50, 53, 57, 60] }, // Bbmaj9
    { root: 31, tones: [43, 46, 50, 53, 57] }, // Gm9
    { root: 33, tones: [45, 48, 52, 55, 62] }, // Am7(11)
];

export function chordAt(bar: number) {
    return CHORDS[((Math.floor(bar / 2) % 4) + 4) % 4];
}

export function midiHz(n: number): number {
    return 440 * Math.pow(2, (n - 69) / 12);
}

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

// CX-52: one letter per beat, a ratchet on the eighth between
export const CX_STEPS: { t: number; letter: boolean }[] = (() => {
    const s = section('cx52');
    const out: { t: number; letter: boolean }[] = [];
    for (let b = s.bar + 1; b < s.bar + s.bars - 1; b++)
        for (let step = 0; step < 16; step += 2)
            out.push({ t: at(b, 0, step), letter: step % 4 === 0 });
    return out;
})();

// A slow heartbeat under the river: lub-dub on beats one and three
export const PULSES: number[] = (() => {
    const s = section('river');
    const out: number[] = [];
    for (let b = s.bar + 1; b < s.bar + s.bars; b++)
        for (const beat of [0, 2]) out.push(at(b, beat), at(b, beat, 1));
    return out;
})();

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
