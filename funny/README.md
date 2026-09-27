# 1952

A music video for Shyake. About 2:40, cold colours, a workstation
aesthetic: a PROM boots, a CX-52 turns its pin wheels, and the story
runs from a cipher machine you could hold to post-quantum mail you
can host yourself.

Every frame is a pure function of song time, so the live preview and
the offline export look the same. The soundtrack is composed in code
from the same score as the picture, so no audio analysis is needed.

## What is real

The film uses real data, not decoration. `src/data/artifacts.ts` runs
the constructions from [SPEC.md](../docs/SPEC.md) with fixed seeds:

- ML-KEM-768 keys, encapsulation and shared secret. The coefficient
  rings in the lattice scene are decoded from the public key.
- ChaCha20-Poly1305 mail fields. The ChaCha scene shows the real
  block state after each of the 20 rounds, and the keystream, XOR
  and Poly1305 tag of one line of the letter.
- An ML-DSA-65 signature over a header-signed request, including the
  SHA-256 of the body (protocol level 2). The punched tape is its
  3309 bytes.
- A 20-bit Hashcash token, the key fingerprint and its randomart
  (the same walk as `client/src/cli/display.c`).

`pnpm test` verifies all of the above.

## Sections

| Bars  | Section    | Picture                                            |
| ----- | ---------- | -------------------------------------------------- |
| 0–8   | boot       | PROM console prints the Shyake parameters          |
| 8–18  | cx52       | Procedural CX-52: pin wheels, lug cage, dial, tape |
| 18–22 | rubicon    | The machine as an x-ray; Crypto AG and Rubicon     |
| 22–32 | lattice    | ML-KEM-768: three rings of 256, LWE, shared secret |
| 32–40 | chacha     | ChaCha20 state round by round, then the tag        |
| 40–48 | sign       | ML-DSA-65 request signing; 1952 bytes              |
| 48–52 | server     | What the server holds: ciphertext only             |
| 52–60 | federation | Peers on a globe, one synchronous relay per beat   |
| 60–68 | terminal   | Two terminals: send, inbox, fetch, fingerprint     |
| 68–74 | outro      | Title, links, Salmonization                        |

112 BPM, D minor. Section lengths live in `src/engine/score.ts`.

## Commands

Toolchain: [Vite+](https://viteplus.dev) (`vp`) with pnpm; lint is
oxlint, format is oxfmt. Node 22.18 or later runs the TypeScript
scripts directly.

```sh
pnpm install
pnpm dev            # preview on http://127.0.0.1:5173
pnpm check          # format, lint, type check
pnpm test           # crypto artifacts against noble and RFC vectors
pnpm audio          # soundtrack only, to out/shyake.wav
```

Preview keys: space plays and pauses, ← → move one bar, 0–9 jump to
a section. `?t=42` starts at 42 seconds.

### Export

The export needs ffmpeg on `PATH` and a Chromium. It uses
`/opt/pw-browsers/chromium` if it exists; otherwise set `CHROMIUM`.

```sh
pnpm render video                        # 1080p60, out/shyake-1080p60.mp4
pnpm render video --scale 2              # 2160p60
pnpm render video --fps 30 --from 18 --to 40
pnpm render video --jobs 4               # 4 browsers, 4 segments
pnpm render stills 21.5 64 130           # PNGs to out/stills/
pnpm render sheet --every 2              # one frame every 2 bars
```

With a hardware GPU, a frame takes tens of milliseconds. Under
software rendering (SwiftShader), expect about 1 to 5 seconds per
1080p frame.

## Layout

```
src/
  engine/     score, script, director, HUD, post-processing, camera
  audio/      DSP, the arrangement, WAV encoder, preview worker
  data/       real Shyake artifacts and a traced ChaCha20 block
  scenes/     one module per scene
  assets/     the Salmonization mark
scripts/      soundtrack and video export
public/fonts/ EB Garamond, GNU FreeSans, GNU FreeMono
```

## Credits

- Fonts: EB Garamond (SIL OFL 1.1); GNU FreeFont (GPLv3+ with the
  font exception). See `public/fonts/README.txt`.
- The Salmonization mark is U+1F41F from Noto Sans Symbols 2 (SIL
  OFL 1.1), cut to an SVG path.
- The CX-52 is built from primitives, and its proportions follow
  photographs. It is an interpretation of the machine, not a
  replica. The workstation look pays homage to that era of
  computing. It uses no vendor names or logos.
- Tooling approach after
  [mexicat/pdoom-video](https://github.com/mexicat/pdoom-video).
