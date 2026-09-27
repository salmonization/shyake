import { H, W } from '../engine/hud.ts';
import { C } from '../engine/palette.ts';
import type { SectionId } from '../engine/score.ts';
import type { SceneDef } from '../engine/types.ts';

export function placeholder(id: SectionId): SceneDef {
    return {
        id,
        sections: [id],
        create: () => ({
            update(f, hud) {
                hud.rect(0, 0, W, H, C.navy);
                hud.text(`${id}  ${f.bar.toFixed(2)}`, W / 2, H / 2, {
                    face: 'italic',
                    size: 80,
                    align: 'center',
                });
                return {};
            },
        }),
    };
}
