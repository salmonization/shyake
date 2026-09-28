// What gets printed and typed, and when. Keystrokes here also drive
// the key clicks in the soundtrack.

import { at, BEAT, section, sectionStart, STEP, typeOn, type Keystroke } from './score.ts';

/* ------------------------------------------------------------------ */
/* Boot PROM, after the old white-screen consoles                     */
/* ------------------------------------------------------------------ */

export interface PrintLine {
    t: number;
    text: string;
    // indented beside the vendor mark
    banner?: boolean;
    // dots that fill in before "Completed."
    progress?: [number, number];
}

const b = section('boot').bar;

export const BOOT_LINES: PrintLine[] = [
    { t: at(b, 2), text: 'Selftest Completed.' },
    { t: at(b + 1), text: 'Shyake Workstation, Model 52.', banner: true },
    { t: at(b + 1, 0, 2), text: 'No Keyboard.', banner: true },
    {
        t: at(b + 1, 1),
        text: 'ROM Rev 3.52, 3329 KB memory installed, Serial #1952.',
        banner: true,
    },
    {
        t: at(b + 1, 1, 2),
        text: 'Ethernet address 8:0:19:52:c:52, Host ID 19520052.',
        banner: true,
    },
    {
        t: at(b + 1, 3),
        text: 'Testing 3329 Kilobytes of Memory ... Completed.',
        progress: [at(b + 1, 3), at(b + 2, 2)],
    },
    { t: at(b + 2, 3), text: 'Auto-boot in progress...' },
    { t: at(b + 3, 1), text: 'Boot device: le(0,0,0)   File and args: shyake' },
    { t: at(b + 3, 2), text: 'kem0 at mainbus0: ML-KEM-768, pk 1184, ct 1088' },
    { t: at(b + 3, 3), text: 'sig0 at mainbus0: ML-DSA-65, pk 1952, sig 3309' },
    { t: at(b + 4), text: 'aead0 at mainbus0: ChaCha20-Poly1305' },
    { t: at(b + 4, 1), text: 'store0: ciphertext and public keys only' },
    { t: at(b + 4, 2, 2), text: 'shyake: plaintext never leaves this machine.' },
];

/* ------------------------------------------------------------------ */
/* PGP in a terminal, 1994                                            */
/* ------------------------------------------------------------------ */

const p = sectionStart('pgp');

export const PGP_TYPED = typeOn('pgp -eat letter.txt', p + BEAT * 2, STEP);

export const PGP_LINES: { t: number; text: string }[] = [
    {
        t: p + BEAT * 8,
        text: 'Pretty Good Privacy(tm) 2.6.2 - Public-key encryption for the masses.',
    },
    { t: p + BEAT * 8.5, text: "(c) 1990-1994 Philip Zimmermann, Phil's Pretty Good Software." },
    {
        t: p + BEAT * 9.5,
        text: 'Export of this software may be restricted by the U.S. government.',
    },
];

/* ------------------------------------------------------------------ */
/* Terminal session                                                   */
/* ------------------------------------------------------------------ */

export type Pane = 'left' | 'right';

export interface Command {
    pane: Pane;
    cmd: string;
    typed: Keystroke[];
    // output appears at this time
    out: number;
    // output lines are filled in by the scene, which has the crypto data
    key: 'whoami' | 'send' | 'inbox' | 'fetch' | 'fingerprint';
}

const d = sectionStart('desk');
const TYPE = STEP / 3;

function command(pane: Pane, key: Command['key'], cmd: string, t0: number, outAt: number): Command {
    return { pane, key, cmd, typed: typeOn(cmd, d + t0, TYPE), out: d + outAt };
}

export function terminalScript(mailId: string): Command[] {
    return [
        command('left', 'whoami', 'shyake whoami', 0.4, 1.9),
        command(
            'left',
            'send',
            'shyake send -t flat_white -s "Where the wheels turn" < note.txt',
            2.6,
            7.3,
        ),
        command('right', 'inbox', 'shyake check inbox', 7.7, 9.2),
        command('right', 'fetch', `shyake fetch ${mailId}`, 9.7, 11.5),
        command('left', 'fingerprint', 'shyake fingerprint flat_white', 12.0, 14.3),
    ];
}

// Keystroke times only, for the soundtrack (the id length is fixed)
export const TERMINAL_KEYS: Keystroke[] = terminalScript('XXXXXXXXXX').flatMap((c) => c.typed);
