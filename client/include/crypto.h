#ifndef SHYAKE_CRYPTO_H
#define SHYAKE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define CHACHA20_KEY_SIZE 32
#define CHACHA20_NONCE_SIZE 12
#define POLY1305_MAC_SIZE 16

int chacha20_poly1305_encrypt(
    const uint8_t *key, const uint8_t *nonce,
    const uint8_t *plaintext, size_t plaintext_len,
    const uint8_t *aad, size_t aad_len,
    uint8_t *ciphertext, uint8_t *mac);

int chacha20_poly1305_decrypt(
    const uint8_t *key, const uint8_t *nonce,
    const uint8_t *ciphertext, size_t ciphertext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t *mac,
    uint8_t *plaintext);

#endif // SHYAKE_CRYPTO_H
