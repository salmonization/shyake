#include "crypto.h"
#include <openssl/evp.h>

int chacha20_poly1305_encrypt(
    const uint8_t *key, const uint8_t *nonce,
    const uint8_t *plaintext, size_t plaintext_len,
    const uint8_t *aad, size_t aad_len,
    uint8_t *ciphertext, uint8_t *mac)
{
    EVP_CIPHER_CTX *ctx;
    int len;
    int ciphertext_len_out;

    if (!(ctx = EVP_CIPHER_CTX_new())) return -1;

    if (1 != EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(),
        NULL, NULL, NULL)) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (1 != EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce)) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (aad && aad_len > 0) {
        if (1 != EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len)) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
    }

    if (plaintext && plaintext_len > 0) {
        if (1 != EVP_EncryptUpdate(ctx, ciphertext, &len, 
            plaintext, plaintext_len)) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
        ciphertext_len_out = len;
    } else {
        ciphertext_len_out = 0;
    }

    if (1 != EVP_EncryptFinal_ex(ctx, 
        ciphertext + ciphertext_len_out, &len)) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 
        POLY1305_MAC_SIZE, mac)) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    EVP_CIPHER_CTX_free(ctx);
    return 0;
}

int chacha20_poly1305_decrypt(
    const uint8_t *key, const uint8_t *nonce,
    const uint8_t *ciphertext, size_t ciphertext_len,
    const uint8_t *aad, size_t aad_len,
    const uint8_t *mac,
    uint8_t *plaintext)
{
    EVP_CIPHER_CTX *ctx;
    int len;
    int plaintext_len_out;

    if (!(ctx = EVP_CIPHER_CTX_new())) return -1;

    if (1 != EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), 
        NULL, NULL, NULL)) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (1 != EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce)) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (aad && aad_len > 0) {
        if (1 != EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len)) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
    }

    if (ciphertext && ciphertext_len > 0) {
        if (1 != EVP_DecryptUpdate(ctx, plaintext, &len, 
            ciphertext, ciphertext_len)) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
        plaintext_len_out = len;
    } else {
        plaintext_len_out = 0;
    }

    if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 
        POLY1305_MAC_SIZE, (void *)mac)) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    int ret = EVP_DecryptFinal_ex(ctx, plaintext + plaintext_len_out, &len);

    EVP_CIPHER_CTX_free(ctx);
    return ret == 1 ? 0 : -1;
}
