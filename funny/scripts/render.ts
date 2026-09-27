// Offline export: drive the page in headless Chromium, step song time
// frame by frame, and pipe the frames into ffmpeg with the soundtrack.
//
//   node scripts/render.ts video [--fps 60] [--scale 1] [--from s] [--to s]
//   node scripts/render.ts stills <t> [<t> ...]
//   node scripts/render.ts sheet [--every bars]

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import { chromium } from 'playwright-core';
import { createServer } from 'vite-plus';
import { renderSong } from '../src/audio/synth.ts';
import { encodeWav } from '../src/audio/wav.ts';
import { BAR, DURATION, SECTIONS } from '../src/engine/score.ts';

const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
        fps: { type: 'string', default: '60' },
        scale: { type: 'string', default: '1' },
        from: { type: 'string', default: '0' },
        to: { type: 'string', default: String(DURATION) },
        every: { type: 'string', default: '1' },
        out: { type: 'string' },
        crf: { type: 'string', default: '14' },
    },
});

const mode = positionals[0] ?? 'video';
const fps = Number(values.fps);
const scale = Number(values.scale);

function chromePath(): string | undefined {
    if (process.env.CHROMIUM) return process.env.CHROMIUM;
    for (const p of ['/opt/pw-browsers/chromium']) if (existsSync(p)) return p;
    return undefined;
}

async function open() {
    const server = await createServer({ logLevel: 'warn', server: { port: 0 } });
    await server.listen();
    const addr = server.resolvedUrls?.local[0];
    if (!addr) throw new Error('vite did not start');

    const browser = await chromium.launch({
        executablePath: chromePath(),
        args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
    });
    const page = await browser.newPage({
        viewport: { width: 1920 * scale, height: 1080 * scale },
    });
    page.on('console', (m) => {
        if (m.type() === 'error' || m.type() === 'warning') console.error(`[page] ${m.text()}`);
    });
    page.on('pageerror', (e) => console.error(`[page] ${e.message}`));
    await page.goto(`${addr}?render&scale=${scale}`);
    await page.waitForFunction(() => window.__film !== undefined, null, { timeout: 120_000 });

    const grab = async (t: number): Promise<Buffer> => {
        const url = await page.evaluate((time) => {
            window.__film?.frame(time);
            const c = document.getElementById('film') as HTMLCanvasElement;
            return c.toDataURL('image/png');
        }, t);
        return Buffer.from(url.slice(url.indexOf(',') + 1), 'base64');
    };
    const close = async () => {
        await browser.close();
        await server.close();
    };
    return { grab, close };
}

async function stills(times: number[], dir = 'out/stills') {
    mkdirSync(dir, { recursive: true });
    const { grab, close } = await open();
    for (const t of times) {
        const file = `${dir}/t${t.toFixed(2).padStart(7, '0')}.png`;
        writeFileSync(file, await grab(t));
        console.log(file);
    }
    await close();
}

async function video() {
    const from = Number(values.from);
    const to = Math.min(DURATION, Number(values.to));
    const out = values.out ?? `out/shyake-${1080 * scale}p${fps}.mp4`;
    mkdirSync('out', { recursive: true });

    console.log('composing soundtrack…');
    writeFileSync('out/shyake.wav', encodeWav(renderSong()));

    const ff = spawn(
        'ffmpeg',
        [
            '-y',
            '-loglevel',
            'error',
            '-f',
            'image2pipe',
            '-framerate',
            String(fps),
            '-c:v',
            'png',
            '-i',
            '-',
            '-ss',
            String(from),
            '-t',
            String(to - from),
            '-i',
            'out/shyake.wav',
            '-c:v',
            'libx264',
            '-preset',
            'slow',
            '-crf',
            values.crf ?? '14',
            '-pix_fmt',
            'yuv420p',
            '-c:a',
            'aac',
            '-b:a',
            '320k',
            '-movflags',
            '+faststart',
            '-shortest',
            out,
        ],
        { stdio: ['pipe', 'inherit', 'inherit'] },
    );
    const done = new Promise<void>((resolve, reject) =>
        ff.on('close', (code) => (code === 0 ? resolve() : reject(new Error(`ffmpeg ${code}`)))),
    );

    const { grab, close } = await open();
    const frames = Math.round((to - from) * fps);
    const t0 = performance.now();
    for (let i = 0; i < frames; i++) {
        const png = await grab(from + i / fps);
        if (!ff.stdin.write(png)) await new Promise((r) => ff.stdin.once('drain', r));
        if (i % fps === 0) {
            const el = (performance.now() - t0) / 1000;
            const eta = (el / (i + 1)) * (frames - i - 1);
            process.stdout.write(
                `\rframe ${i}/${frames}  ${el.toFixed(0)}s  eta ${eta.toFixed(0)}s   `,
            );
        }
    }
    ff.stdin.end();
    await close();
    await done;
    console.log(`\n${out}`);
}

if (mode === 'stills') {
    await stills(positionals.slice(1).map(Number));
} else if (mode === 'sheet') {
    // one frame every N bars, labelled by section, for a quick look
    const every = Number(values.every);
    const times: number[] = [];
    for (let b = 0; b * BAR < DURATION; b += every) times.push(b * BAR + BAR * 0.5);
    await stills(times, 'out/sheet');
    console.log(SECTIONS.map((s) => `${s.id}@${s.bar}`).join(' '));
} else {
    await video();
}
