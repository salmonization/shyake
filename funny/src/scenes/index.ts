import type { SceneDef } from '../engine/types.ts';
import { boot } from './boot.ts';
import { cx52 } from './cx52/index.ts';
import { placeholder } from './placeholder.ts';

export const SCENES: SceneDef[] = [
    boot,
    cx52,
    placeholder('lattice'),
    placeholder('chacha'),
    placeholder('sign'),
    placeholder('server'),
    placeholder('federation'),
    placeholder('terminal'),
    placeholder('outro'),
];
