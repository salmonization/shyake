import type { SceneDef } from '../engine/types.ts';
import { boot } from './boot.ts';
import { cx52 } from './cx52/index.ts';
import { pgp } from './pgp.ts';
import { river } from './river.ts';
import { sea } from './sea.ts';
import { terminal } from './terminal.ts';
import { tide } from './tide.ts';
import { title } from './title.ts';

// In playing order; cx52 also covers the Crypto AG section
export const SCENES: SceneDef[] = [sea, boot, cx52, pgp, tide, river, terminal, title];
