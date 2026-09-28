# 1952

A short film for Shyake, about 3:45. Grey blue and grey green, the
sea before morning, a quiet ambient score. It moves from a cipher
machine you could hold, through two stories of trust and of the
right to privacy, to mail anyone can host and only its reader can
open.

Every frame is a pure function of song time, so the live preview and
the offline export look the same. The soundtrack is composed in code
from the same score as the picture, so no audio analysis is needed.

## What is real

`src/data/artifacts.ts` runs the constructions from
[SPEC.md](../docs/SPEC.md) with fixed seeds, and the film shows their
output:

- The buoys in the tide scene carry the coefficients of a real
  ML-KEM-768 public key, then the bits of a real ChaCha20 block,
  round by round.
- The CX-52's tape prints letters taken from a real ChaCha20-Poly1305
  mail body.
- The terminal shows real fingerprints and randomart (the same walk
  as `client/src/cli/display.c`).
- The printed book's pages are Shyake's own C source.

`pnpm test` verifies the artifacts. The history in the Crypto AG and
PGP sections is sourced in `src/engine/lines.ts`.

## Sections

| Bars  | Section  | Picture                                                 |
| ----- | -------- | ------------------------------------------------------- |
| 0–6   | sea      | Grey sea under mist; a fish passes under the surface    |
| 6–12  | boot     | A white-screen PROM in the mist boots Shyake            |
| 12–19 | cx52     | The CX-52 in an empty museum, enciphering               |
| 19–33 | cryptoag | Crypto AG, as a dithered archive, one sentence a shot    |
| 33–44 | pgp      | PGP 2.6.2, a book of source, a sentence from 1991       |
| 44–52 | tide     | 768 buoys: a public key, then twenty ChaCha20 rounds    |
| 52–58 | river    | A coast of rivers; lights swim upstream to their source |
| 58–63 | desk     | Two terminals: send, inbox, fetch, fingerprint          |
| 63–67 | title    | The name, then the mark                                 |

72 BPM, D minor. Section lengths live in `src/engine/score.ts`, and
every sentence with its time in `src/engine/lines.ts`.

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
pnpm render video --fps 30 --from 60 --to 110
pnpm render video --jobs 4               # 4 browsers, 4 segments
pnpm render stills 42 95 160             # PNGs to out/stills/
pnpm render sheet --every 2              # one frame every 2 bars
```

With a hardware GPU, a frame takes tens of milliseconds. Under
software rendering (SwiftShader), expect about 1 to 5 seconds per
1080p frame.

## Layout

```
src/
  engine/     score, lines, script, director, HUD, captions,
              post-processing (Bayer duotone), sea, noise
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
- The CX-52 is built from primitives after photographs from the
  Crypto Museum. It is an interpretation of the machine, not a
  replica. The workstation look pays homage to that era of
  computing. It uses no vendor names or logos.
- Tooling approach after
  [mexicat/pdoom-video](https://github.com/mexicat/pdoom-video).
