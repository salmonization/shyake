// The name, alone. Then the mark, alone, and the film ends on it.

import type { Hud } from '../engine/hud.ts';
import { H, W } from '../engine/hud.ts';
import { drawLogo, LOGO_H, LOGO_W } from '../engine/logo.ts';
import { smooth } from '../engine/math.ts';
import { C } from '../engine/palette.ts';
import type { Frame, SceneDef } from '../engine/types.ts';

export const LOGO_AT = 1.75;

export const title: SceneDef = {
    id: 'title',
    sections: ['title'],
    create() {
        return {
            update(f: Frame, hud: Hud) {
                hud.rect(0, 0, W, H, C.night);
                if (f.bar < LOGO_AT) {
                    hud.text('Shyake', W / 2, H / 2 + 36, {
                        face: 'serif',
                        weight: 700,
                        size: 132,
                        align: 'center',
                        color: C.paper,
                        alpha: smooth(f.local / 1.4),
                    });
                } else {
                    const h = 150;
                    drawLogo(hud.ctx, W / 2 - ((LOGO_W / LOGO_H) * h) / 2, H / 2, h, C.paper, 1);
                }
                return { post: { grain: 0.3, vignette: 0.4 } };
            },
        };
    },
};
