// Render the soundtrack to out/shyake.wav

import { mkdirSync, writeFileSync } from 'node:fs';
import { renderSong } from '../src/audio/synth.ts';
import { encodeWav } from '../src/audio/wav.ts';

const t0 = performance.now();
const song = renderSong();
mkdirSync('out', { recursive: true });
writeFileSync('out/shyake.wav', encodeWav(song));
const secs = song.left.length / song.sampleRate;
console.log(
    `out/shyake.wav: ${secs.toFixed(2)} s in ${((performance.now() - t0) / 1000).toFixed(1)} s`,
);
