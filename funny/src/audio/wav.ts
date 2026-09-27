// 16-bit PCM WAV encoder

import type { Song } from './synth.ts';

export function encodeWav(song: Song): Uint8Array {
    const n = song.left.length;
    const bytes = new Uint8Array(44 + n * 4);
    const v = new DataView(bytes.buffer);
    const str = (o: number, s: string) => {
        for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i));
    };
    str(0, 'RIFF');
    v.setUint32(4, 36 + n * 4, true);
    str(8, 'WAVE');
    str(12, 'fmt ');
    v.setUint32(16, 16, true);
    v.setUint16(20, 1, true);
    v.setUint16(22, 2, true);
    v.setUint32(24, song.sampleRate, true);
    v.setUint32(28, song.sampleRate * 4, true);
    v.setUint16(32, 4, true);
    v.setUint16(34, 16, true);
    str(36, 'data');
    v.setUint32(40, n * 4, true);
    const q = (x: number) => Math.max(-32768, Math.min(32767, Math.round(x * 32767)));
    for (let i = 0; i < n; i++) {
        v.setInt16(44 + i * 4, q(song.left[i]), true);
        v.setInt16(46 + i * 4, q(song.right[i]), true);
    }
    return bytes;
}
