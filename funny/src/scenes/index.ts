import type { SceneDef } from '../engine/types.ts';
import { boot } from './boot.ts';
import { cx52 } from './cx52/index.ts';
import { lattice } from './lattice.ts';
import { chacha } from './chacha.ts';
import { sign } from './sign.ts';
import { placeholder } from './placeholder.ts';

export const SCENES: SceneDef[] = [
    boot,
    cx52,
    lattice,
    chacha,
    sign,
    placeholder('server'),
    placeholder('federation'),
    placeholder('terminal'),
    placeholder('outro'),
];
