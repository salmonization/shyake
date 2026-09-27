// What gets typed and printed, and when. Keystrokes here also drive
// the key clicks in the soundtrack.

import { at, BEAT, section, STEP, typeOn, type Keystroke } from './score.ts';

const boot = section('boot').bar;
const term = section('terminal').bar;
const FAST = STEP / 2;

/* ------------------------------------------------------------------ */
/* Boot PROM                                                          */
/* ------------------------------------------------------------------ */

export interface PrintLine {
    t: number;
    text: string;
    typed?: Keystroke[];
    dim?: boolean;
}

function lines(t0: number, every: number, texts: string[], dim = false): PrintLine[] {
    return texts.map((text, i) => ({ t: t0 + i * every, text, dim }));
}

const PROMPT_CMD = 'boot net:shyake';

export const BOOT_TYPED = typeOn(PROMPT_CMD, at(boot + 2, 0, 2));

export const BOOT_LINES: PrintLine[] = [
    ...lines(at(boot, 1), BEAT / 2, [
        'Shyake Workstation 52, No Keyboard',
        'PROM Rev 3.52, 3 x 256 coefficients mod 3329',
        'Ethernet address 0:19:52:c:x:52, Host ID 19520052.',
    ]),
    { t: at(boot + 2), text: 'ok ', typed: BOOT_TYPED },
    ...lines(
        at(boot + 3, 1),
        BEAT / 2,
        [
            'Boot device: /net   File and args: shyake',
            'libshyake: liboqs (static), libcurl, libcrypto, cJSON',
            'kem0:   ML-KEM-768         pk 1184   sk 2400   ct 1088',
            'sig0:   ML-DSA-65          pk 1952   sig 3309',
            'aead0:  ChaCha20-Poly1305  key 32   nonce 12   tag 16',
            'kdf0:   scrypt             N 65536   r 8   p 1',
            'pow0:   hashcash           20 bits   sha-1',
            'net0:   relays synchronous, queue none',
            'store0: ciphertext and public keys only',
        ],
        false,
    ),
    { t: at(boot + 5, 3), text: 'shyake: plaintext never leaves this machine.' },
];

export const BOOT_CLEAR = at(boot + 6);

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

function command(pane: Pane, key: Command['key'], cmd: string, t0: number, outAt: number): Command {
    return { pane, key, cmd, typed: typeOn(cmd, t0, FAST), out: outAt };
}

export function terminalScript(mailId: string): Command[] {
    return [
        command('left', 'whoami', 'shyake whoami', at(term, 0, 1), at(term, 2)),
        command(
            'left',
            'send',
            'shyake send -t flat_white -s "Where the wheels turn" < note.txt',
            at(term + 1, 0, 0),
            at(term + 3),
        ),
        command('right', 'inbox', 'shyake check inbox', at(term + 3, 2), at(term + 4)),
        command('right', 'fetch', `shyake fetch ${mailId}`, at(term + 4, 2), at(term + 5, 2)),
        command('left', 'fingerprint', 'shyake fingerprint flat_white', at(term + 5), at(term + 6)),
    ];
}

// Keystroke times only, for the soundtrack (the id length is fixed)
export const TERMINAL_KEYS: Keystroke[] = terminalScript('XXXXXXXXXX').flatMap((c) => c.typed);
