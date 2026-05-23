#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <oqs/oqs.h>
#include <openssl/evp.h>
#include "lib_internal.h"
#include "shyake_crypto.h"

char*
encrypt_to_b64(const uint8_t *key, const uint8_t *pt, size_t pt_len)
{
    if (!pt || pt_len == 0) return NULL;

    uint8_t nonce[12];
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        fread(nonce, 1, 12, urandom);
        fclose(urandom);
    } else return NULL;

    uint8_t *ct = malloc(pt_len);
    uint8_t mac[16];

    if (chacha20_poly1305_encrypt(key, nonce, pt, pt_len, NULL, 0,
                                  ct, mac) != 0) {
        free(ct);
        return NULL;
    }

    size_t packed_len = 12 + pt_len + 16;
    uint8_t *packed = malloc(packed_len);
    memcpy(packed, nonce, 12);
    memcpy(packed + 12, ct, pt_len);
    memcpy(packed + 12 + pt_len, mac, 16);

    char *b64 = base64_encode(packed, packed_len);
    free(ct); free(packed);
    return b64;
}

char*
decrypt_from_b64(const uint8_t *key, const char *b64)
{
    if (!b64 || strlen(b64) == 0) return NULL;
    size_t packed_len;
    uint8_t *packed = base64_decode(b64, &packed_len);
    if (!packed || packed_len <= 28) { free(packed); return NULL; }

    size_t ct_len = packed_len - 28;
    uint8_t nonce[12];
    uint8_t mac[16];
    uint8_t *ct = packed + 12;
    memcpy(nonce, packed, 12);
    memcpy(mac, packed + 12 + ct_len, 16);

    char *pt = malloc(ct_len + 1);
    if (chacha20_poly1305_decrypt(key, nonce, ct, ct_len, NULL, 0, mac,
                                  (uint8_t*)pt) != 0) {
        free(pt); free(packed); return NULL;
    }
    pt[ct_len] = '\0';
    free(packed);
    return pt;
}

char*
kem_encapsulate_key(const uint8_t *kem_pk, size_t kem_pk_len,
                    const uint8_t *sym_key)
{
    OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
    if (!kem) return NULL;

    if (kem_pk_len != kem->length_public_key) {
        OQS_KEM_free(kem);
        return NULL;
    }

    uint8_t *ct = malloc(kem->length_ciphertext);
    uint8_t *ss = malloc(kem->length_shared_secret);

    if (OQS_KEM_encaps(kem, ct, ss, kem_pk) != OQS_SUCCESS) {
        free(ct); free(ss); OQS_KEM_free(kem);
        return NULL;
    }

    uint8_t ek[32];
    for (int i = 0; i < 32; i++)
        ek[i] = sym_key[i] ^ ss[i];

    size_t packed_len = kem->length_ciphertext + 32;
    uint8_t *packed = malloc(packed_len);
    memcpy(packed, ct, kem->length_ciphertext);
    memcpy(packed + kem->length_ciphertext, ek, 32);

    char *b64 = base64_encode(packed, packed_len);

    free(ct); free(ss); free(packed); OQS_KEM_free(kem);
    return b64;
}

uint8_t*
kem_decapsulate_key(const char *enc_key_b64, const uint8_t *my_sk)
{
    size_t packed_len;
    uint8_t *packed = base64_decode(enc_key_b64, &packed_len);
    if (!packed) return NULL;

    OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
    if (!kem) { free(packed); return NULL; }

    if (packed_len != kem->length_ciphertext + 32) {
        free(packed); OQS_KEM_free(kem); return NULL;
    }

    uint8_t *ss = malloc(kem->length_shared_secret);
    if (OQS_KEM_decaps(kem, ss, packed, my_sk) != OQS_SUCCESS) {
        free(ss); free(packed); OQS_KEM_free(kem); return NULL;
    }

    uint8_t *sym_key = malloc(32);
    uint8_t *ek = packed + kem->length_ciphertext;
    for (int i = 0; i < 32; i++)
        sym_key[i] = ek[i] ^ ss[i];

    free(ss); free(packed); OQS_KEM_free(kem);
    return sym_key;
}
