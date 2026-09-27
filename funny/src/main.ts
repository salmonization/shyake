// Preview player, and the frame server the export script drives

import type { Song } from './audio/synth.ts';
import { Director } from './engine/director.ts';
import { loadFonts } from './engine/fonts.ts';
import { DURATION, SECTIONS, sectionAt } from './engine/score.ts';
import { SCENES } from './scenes/index.ts';

declare global {
    interface Window {
        __film?: { duration: number; frame(t: number): void };
    }
}

const params = new URLSearchParams(location.search);
const renderMode = params.has('render');
const scale = Number(params.get('scale') ?? 1);
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

async function main() {
    await loadFonts(import.meta.env.BASE_URL);
    const canvas = $<HTMLCanvasElement>('film');
    const director = new Director(canvas, SCENES, scale);

    if (renderMode) {
        document.body.classList.add('render');
        canvas.style.width = `${1920 * scale}px`;
        canvas.style.height = `${1080 * scale}px`;
        window.__film = { duration: DURATION, frame: (t) => director.frame(t) };
        return;
    }
    player(director);
}

function fmt(t: number): string {
    const m = Math.floor(t / 60);
    const s = t - m * 60;
    return `${m}:${s.toFixed(2).padStart(5, '0')}`;
}

function player(director: Director) {
    const overlay = $('overlay');
    const play = $<HTMLButtonElement>('play');
    const scrub = $<HTMLInputElement>('scrub');
    const clock = $('clock');
    const where = $('where');

    let audio: AudioContext | null = null;
    let buffer: AudioBuffer | null = null;
    let source: AudioBufferSourceNode | null = null;
    let playing = false;
    let offset = Number(params.get('t') ?? 0);
    let started = 0;

    const now = () => {
        if (!playing) return offset;
        const clockNow = audio ? audio.currentTime : performance.now() / 1000;
        return offset + clockNow - started;
    };

    const worker = new Worker(new URL('./audio/worker.ts', import.meta.url), { type: 'module' });
    overlay.textContent = 'composing the soundtrack…';
    worker.onmessage = (e: MessageEvent<Song>) => {
        const song = e.data;
        audio = new AudioContext({ sampleRate: song.sampleRate });
        buffer = audio.createBuffer(2, song.left.length, song.sampleRate);
        buffer.copyToChannel(new Float32Array(song.left), 0);
        buffer.copyToChannel(new Float32Array(song.right), 1);
        overlay.textContent = 'click to play · space · ←/→ bar · 0–9 section';
        worker.terminate();
    };
    worker.postMessage(null);

    const start = () => {
        if (playing) return;
        playing = true;
        play.textContent = 'pause';
        if (audio && buffer) {
            void audio.resume();
            source = audio.createBufferSource();
            source.buffer = buffer;
            source.connect(audio.destination);
            source.start(0, offset);
            started = audio.currentTime;
        } else {
            started = performance.now() / 1000;
        }
    };
    const stop = () => {
        if (!playing) return;
        offset = now();
        playing = false;
        play.textContent = 'play';
        source?.stop();
        source = null;
    };
    const seek = (t: number) => {
        const was = playing;
        stop();
        offset = Math.min(DURATION - 0.01, Math.max(0, t));
        if (was) start();
    };

    overlay.addEventListener('click', () => {
        if (!buffer) return;
        overlay.hidden = true;
        start();
    });
    play.addEventListener('click', () => (playing ? stop() : start()));
    scrub.addEventListener('input', () => seek((Number(scrub.value) / 1000) * DURATION));
    addEventListener('keydown', (e) => {
        const bar = (60 / 112) * 4;
        if (e.key === ' ') {
            e.preventDefault();
            overlay.hidden = true;
            if (playing) stop();
            else start();
        } else if (e.key === 'ArrowRight') seek(now() + bar);
        else if (e.key === 'ArrowLeft') seek(now() - bar);
        else if (/^[0-9]$/.test(e.key)) {
            const s = SECTIONS[Number(e.key)];
            if (s) seek(s.bar * bar);
        }
    });

    const tick = () => {
        let t = now();
        if (t >= DURATION) {
            stop();
            offset = 0;
            t = 0;
        }
        director.frame(t);
        clock.textContent = fmt(t);
        where.textContent = sectionAt(t).id;
        if (document.activeElement !== scrub)
            scrub.value = String(Math.round((t / DURATION) * 1000));
        requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
}

void main();
