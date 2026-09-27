import type { SceneDef } from '../engine/types.ts';
import { boot } from './boot.ts';
import { chacha } from './chacha.ts';
import { cx52 } from './cx52/index.ts';
import { federation } from './federation.ts';
import { lattice } from './lattice.ts';
import { outro } from './outro.ts';
import { server } from './server.ts';
import { sign } from './sign.ts';
import { terminal } from './terminal.ts';

// In playing order; cx52 also covers the Rubicon section
export const SCENES: SceneDef[] = [
    boot,
    cx52,
    lattice,
    chacha,
    sign,
    server,
    federation,
    terminal,
    outro,
];
