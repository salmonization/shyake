// Every sentence in the film, with its time. Scenes draw them; the
// soundtrack rings a low bell under the historical ones.
//
// Sources for the history:
//   Crypto AG: Swiss National Museum blog (2020-02), Washington Post
//   "The intelligence coup of the century" (2020-02-11), Wikipedia
//   "Crypto AG" and "Operation Rubicon".
//   PGP: Wikipedia "Phil Zimmermann"; P. Zimmermann, "Why I Wrote
//   PGP" (1991, PGP User's Guide).

import type { Line } from './captions.ts';
import { at, section, type SectionId } from './score.ts';

function lines(id: SectionId, list: [number, number, string, string?][]): Line[] {
    const b = section(id).bar;
    return list.map(([from, to, text, small]) => ({
        from: at(b + from),
        to: at(b + to),
        text,
        small,
    }));
}

export const SEA_LINES = lines('sea', [[2.5, 5.6, 'Before morning, the sea keeps no records.']]);

export const CX52_LINES = lines('cx52', [
    [2.2, 6.2, 'In 1952, a cipher was something you could hold.'],
]);

export const CRYPTOAG_LINES = lines('cryptoag', [
    [0.3, 2.0, 'That year, Boris Hagelin founded Crypto AG in Zug, Switzerland.'],
    [2.3, 4.0, 'Used correctly, the machine was secure. Later versions were made to be read.'],
    [4.3, 6.0, 'In 1970, the company was quietly bought by the CIA and West Germany’s BND.'],
    [6.3, 8.0, 'More than a hundred and twenty countries kept their secrets in its machines.'],
    [
        8.3,
        10.2,
        'In 1992, a salesman named Hans Bühler was arrested in Iran and held for nine months.',
    ],
    [10.5, 11.5, 'He had not known.'],
    [11.8, 13.0, 'The rest of the world learned in 2020.'],
]);

export const PGP_CAPTIONS = lines('pgp', [
    [0.3, 2.0, 'In 1991, Phil Zimmermann gave away a program called Pretty Good Privacy.'],
    [2.3, 4.0, 'The government counted strong encryption as a munition.'],
    [4.3, 6.0, 'For three years, he lived under a criminal investigation.'],
    [6.3, 8.0, 'So the source code was printed as a book, and a book could cross the border.'],
    [8.2, 9.3, 'In 1996, the case was dropped.'],
    [
        9.5,
        11.0,
        '“If privacy is outlawed, only outlaws will have privacy.”',
        'Phil Zimmermann, 1991',
    ],
]);

export const TIDE_LINES = lines('tide', [
    [0.6, 2.6, 'A point, and a little noise. The sea keeps it.'],
    [3.1, 5.1, 'Twenty tides, and the order is gone.'],
    [5.6, 7.6, 'A public key, 1952 bytes long.'],
]);

export const RIVER_LINES = lines('river', [
    [0.5, 2.3, 'Every river has its own mouth.'],
    [2.6, 3.9, 'No sea owns them.'],
    [4.2, 5.8, 'What does not arrive stays with the one who wrote it.'],
]);

// Pages of the printed source turn once a bar
export const PAGE_TURNS: number[] = [4, 5, 6, 7].map((k) => at(section('pgp').bar + k + 0.5));

// Bell times: the start of each historical sentence
export const BELLS: number[] = [...CRYPTOAG_LINES, ...PGP_CAPTIONS].map((l) => l.from);
