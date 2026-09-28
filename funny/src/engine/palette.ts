// Low-saturation cold palette: the sea before morning, and the grey
// greens of a northern forest. Nothing warm, nothing vivid.

export const C = {
    ink: '#11171b',
    night: '#1a2227',
    deep: '#243039',
    slate: '#34424b',
    sea: '#4a5a63',
    steel: '#6b7a82',
    mist: '#95a3a8',
    fog: '#bcc6c7',
    paper: '#e2e6e3',
    white: '#f1f3f0',
    pine: '#2f3a34',
    moss: '#4c5a51',
    lichen: '#7d8c83',
    sage: '#a8b4ac',
} as const;

// Workstation window chrome, cooled to grey blue
export const MOTIF = {
    face: '#4a5862',
    light: '#7f8e96',
    dark: '#1f282e',
    title: '#5a6f7c',
    titleIdle: '#3d4a53',
    text: '#e3e8e7',
    well: '#161d22',
} as const;

// The two tones of the archival dither
export const DUO = { dark: '#1b2328', light: '#c7cfcd' } as const;

export function hex(c: string): number {
    return parseInt(c.slice(1), 16);
}
