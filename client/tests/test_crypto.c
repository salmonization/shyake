#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <oqs/oqs.h>
#include "../src/lib/shyake_crypto.h"
#include "../src/lib/internal.h"

int test_kem(void)
{
    OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
    if (!kem) return 1;

    u8 *pk = malloc(kem->length_public_key);
    u8 *sk = malloc(kem->length_secret_key);
    u8 *ct = malloc(kem->length_ciphertext);
    u8 *ss1 = malloc(kem->length_shared_secret);
    u8 *ss2 = malloc(kem->length_shared_secret);

    int ret = 0;
    if (OQS_KEM_keypair(kem, pk, sk) != OQS_SUCCESS) ret = 1;
    if (OQS_KEM_encaps(kem, ct, ss1, pk) != OQS_SUCCESS) ret = 1;
    if (OQS_KEM_decaps(kem, ss2, ct, sk) != OQS_SUCCESS) ret = 1;

    if (memcmp(ss1, ss2, kem->length_shared_secret) != 0) ret = 1;

    free(pk); free(sk); free(ct); free(ss1); free(ss2);
    OQS_KEM_free(kem);
    return ret;
}

int test_sig(void)
{
    OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
    if (!sig) return 1;

    u8 *pk = malloc(sig->length_public_key);
    u8 *sk = malloc(sig->length_secret_key);
    u8 message[] = "Hello Shyake";
    usize message_len = strlen((char *)message);
    u8 *signature = malloc(sig->length_signature);
    usize signature_len;

    int ret = 0;
    if (OQS_SIG_keypair(sig, pk, sk) != OQS_SUCCESS) ret = 1;
    if (OQS_SIG_sign(sig, signature, &signature_len,
        message, message_len, sk) != OQS_SUCCESS) ret = 1;
    if (OQS_SIG_verify(sig, message, message_len, signature,
        signature_len, pk) != OQS_SUCCESS) ret = 1;

    free(pk); free(sk); free(signature);
    OQS_SIG_free(sig);
    return ret;
}

int test_chacha20_poly1305(void)
{
    u8 key[CHACHA20_KEY_SIZE] = {0x01, 0x02, 0x03};
    u8 nonce[CHACHA20_NONCE_SIZE] = {0x0a, 0x0b, 0x0c};
    u8 plaintext[] = "Hello ChaCha20-Poly1305";
    usize plaintext_len = strlen((char *)plaintext);
    u8 aad[] = "aad data";
    usize aad_len = strlen((char *)aad);

    u8 ciphertext[64];
    u8 mac[POLY1305_MAC_SIZE];
    u8 decrypted[64];

    if (chacha20_poly1305_encrypt(key, nonce, plaintext, plaintext_len,
        aad, aad_len, ciphertext, mac) != 0) {
        return 1;
    }

    if (chacha20_poly1305_decrypt(key, nonce, ciphertext, plaintext_len,
        aad, aad_len, mac, decrypted) != 0) {
        return 1;
    }

    if (memcmp(plaintext, decrypted, plaintext_len) != 0) {
        return 1;
    }

    // Test tamper
    ciphertext[0] ^= 1;
    if (chacha20_poly1305_decrypt(key, nonce, ciphertext, plaintext_len,
        aad, aad_len, mac, decrypted) == 0) {
        return 1; // should fail
    }

    return 0;
}

int main(void)
{
    if (test_kem() == 0) {
        printf("KEM test passed\n");
    } else {
        printf("KEM test failed\n");
        return 1;
    }

    if (test_sig() == 0) {
        printf("SIG test passed\n");
    } else {
        printf("SIG test failed\n");
        return 1;
    }

    if (test_chacha20_poly1305() == 0) {
        printf("ChaCha20-Poly1305 test passed\n");
    } else {
        printf("ChaCha20-Poly1305 test failed\n");
        return 1;
    }

    return 0;
}
