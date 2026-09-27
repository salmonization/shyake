// Renders the soundtrack off the main thread for the preview

import { renderSong } from './synth.ts';

self.onmessage = () => {
    const song = renderSong();
    (self as unknown as Worker).postMessage(song, [song.left.buffer, song.right.buffer]);
};
