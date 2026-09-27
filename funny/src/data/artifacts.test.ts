import { chacha20 } from '@noble/ciphers/chacha.js';
import { sha1 } from '@noble/hashes/legacy.js';
import { ml_dsa65 } from '@noble/post-quantum/ml-dsa.js';
import { ml_kem768 } from '@noble/post-quantum/ml-kem.js';
import { describe, expect, it } from 'vite-plus/test';
import { artifacts, POW, randomart } from './artifacts.ts';
import { chachaTrace } from './chacha.ts';

const utf8 = new TextEncoder();

describe('chacha trace', () => {
    it('matches the RFC 8439 §2.3.2 block test vector', () => {
        const key = Uint8Array.from({ length: 32 }, (_, i) => i);
        const nonce = Uint8Array.from([0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0]);
        const { output } = chachaTrace(key, 1, nonce);
        expect(output[0]).toBe(0xe4e7f110);
        expect(output[15]).toBe(0x4e3c50a2);
    });

    it('agrees with noble chacha20', () => {
        const a = artifacts();
        const ks = chacha20(a.symKey, a.nonce, new Uint8Array(64), undefined, 1);
        expect(a.trace.keystream).toEqual(ks);
    });
});

describe('shyake artifacts', () => {
    const a = artifacts();

    it('has the SPEC key and ciphertext sizes', () => {
        expect(a.kem.pk.length).toBe(1184);
        expect(a.kem.sk.length).toBe(2400);
        expect(a.kem.ct.length).toBe(1088);
        expect(a.dsa.pk.length).toBe(1952);
        expect(a.mailSig.length).toBe(3309);
    });

    it('decapsulates to the same shared secret', () => {
        expect(ml_kem768.decapsulate(a.kem.ct, a.kem.sk)).toEqual(a.kem.ss);
    });

    it('produces verifiable signatures', () => {
        expect(ml_dsa65.verify(a.mailSig, utf8.encode(a.mailSigned), a.dsa.pk)).toBe(true);
        expect(ml_dsa65.verify(a.blockSig, utf8.encode(a.blockSigned), a.dsa.pk)).toBe(true);
    });

    it('carries a valid 20-bit hashcash token', () => {
        const h = sha1(utf8.encode(POW));
        expect(h[0]).toBe(0);
        expect(h[1]).toBe(0);
        expect(h[2] >> 4).toBe(0);
    });

    it('draws an 11-line randomart', () => {
        const art = randomart(a.fp);
        expect(art).toHaveLength(11);
        expect(art[5]).toContain('S');
    });
});
