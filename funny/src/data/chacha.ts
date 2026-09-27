// ChaCha20 block function (RFC 8439) that records the state after
// every round, so the film can show diffusion happening for real.

export const SIGMA = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574];

// Quarter-round index sets: four columns, then four diagonals
export const COLUMNS = [
    [0, 4, 8, 12],
    [1, 5, 9, 13],
    [2, 6, 10, 14],
    [3, 7, 11, 15],
];
export const DIAGONALS = [
    [0, 5, 10, 15],
    [1, 6, 11, 12],
    [2, 7, 8, 13],
    [3, 4, 9, 14],
];

const rotl = (x: number, n: number) => ((x << n) | (x >>> (32 - n))) >>> 0;

function quarter(s: Uint32Array, a: number, b: number, c: number, d: number) {
    s[a] = (s[a] + s[b]) >>> 0;
    s[d] = rotl(s[d] ^ s[a], 16);
    s[c] = (s[c] + s[d]) >>> 0;
    s[b] = rotl(s[b] ^ s[c], 12);
    s[a] = (s[a] + s[b]) >>> 0;
    s[d] = rotl(s[d] ^ s[a], 8);
    s[c] = (s[c] + s[d]) >>> 0;
    s[b] = rotl(s[b] ^ s[c], 7);
}

function le32(b: Uint8Array, o: number): number {
    return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
}

export function initialState(key: Uint8Array, counter: number, nonce: Uint8Array): Uint32Array {
    const s = new Uint32Array(16);
    s.set(SIGMA, 0);
    for (let i = 0; i < 8; i++) s[4 + i] = le32(key, i * 4);
    s[12] = counter >>> 0;
    for (let i = 0; i < 3; i++) s[13 + i] = le32(nonce, i * 4);
    return s;
}

export interface ChachaTrace {
    // rounds[0] is the initial state, rounds[20] the state after 20 rounds
    rounds: Uint32Array[];
    // final block: rounds[20] + initial state
    output: Uint32Array;
    keystream: Uint8Array;
}

export function chachaTrace(key: Uint8Array, counter: number, nonce: Uint8Array): ChachaTrace {
    const init = initialState(key, counter, nonce);
    const s = init.slice();
    const rounds = [init.slice()];
    for (let r = 0; r < 20; r++) {
        for (const [a, b, c, d] of r % 2 === 0 ? COLUMNS : DIAGONALS) quarter(s, a, b, c, d);
        rounds.push(s.slice());
    }
    const output = new Uint32Array(16);
    for (let i = 0; i < 16; i++) output[i] = (s[i] + init[i]) >>> 0;
    const keystream = new Uint8Array(64);
    for (let i = 0; i < 16; i++) {
        keystream[i * 4] = output[i] & 0xff;
        keystream[i * 4 + 1] = (output[i] >>> 8) & 0xff;
        keystream[i * 4 + 2] = (output[i] >>> 16) & 0xff;
        keystream[i * 4 + 3] = output[i] >>> 24;
    }
    return { rounds, output, keystream };
}
