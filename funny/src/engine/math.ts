// Small, pure helpers for shaping time

export const clamp = (x: number, a = 0, b = 1) => Math.min(b, Math.max(a, x));
export const lerp = (a: number, b: number, t: number) => a + (b - a) * t;
export const fract = (x: number) => x - Math.floor(x);

// Normalized position of t within [a, b], clamped
export const span = (t: number, a: number, b: number) => clamp((t - a) / (b - a));

export const smooth = (x: number) => {
    const t = clamp(x);
    return t * t * (3 - 2 * t);
};

export const easeOut = (x: number, p = 3) => 1 - Math.pow(1 - clamp(x), p);
export const easeIn = (x: number, p = 3) => Math.pow(clamp(x), p);
export const easeInOut = (x: number) => {
    const t = clamp(x);
    return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
};

// Rise over [a, a+fadeIn], hold, fall over [b-fadeOut, b]
export const hold = (t: number, a: number, b: number, fadeIn = 0.4, fadeOut = 0.4) =>
    Math.min(smooth((t - a) / fadeIn), smooth((b - t) / fadeOut));

// Decaying pulse after the most recent of a sorted list of times
export function pulse(times: number[], t: number, decay: number): number {
    let lo = 0;
    let hi = times.length - 1;
    if (hi < 0 || t < times[0]) return 0;
    while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if (times[mid] <= t) lo = mid;
        else hi = mid - 1;
    }
    return Math.exp(-(t - times[lo]) / decay);
}

// Number of times in a sorted list that are <= t
export function countUpTo(times: number[], t: number): number {
    let lo = 0;
    let hi = times.length;
    while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (times[mid] <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
