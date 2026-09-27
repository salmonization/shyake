// Cold palette. No warm colour anywhere in the film.

export const C = {
    ink: '#03060b',
    night: '#07101b',
    navy: '#0c1a2b',
    deep: '#12263c',
    slate: '#253649',
    steel: '#5d7187',
    fog: '#9db0c3',
    ice: '#dcebf7',
    white: '#f2f8fd',
    phosphor: '#8fe9ff',
    cyan: '#43c6e6',
    frost: '#b6d6ff',
    indigo: '#4b5fb4',
    teal: '#2c8c9a',
    olive: '#4a5249',
} as const;

// Motif-style window chrome, cooled down
export const MOTIF = {
    face: '#33445a',
    light: '#6b7f97',
    dark: '#161f2b',
    title: '#3f55a3',
    titleIdle: '#2b3a4f',
    text: '#e3eef8',
    well: '#0a121c',
} as const;

export function hex(c: string): number {
    return parseInt(c.slice(1), 16);
}
