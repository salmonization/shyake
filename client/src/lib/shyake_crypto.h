#ifndef SHYAKE_CRYPTO_H
#define SHYAKE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include "internal.h"

#define CHACHA20_KEY_SIZE 32
#define CHACHA20_NONCE_SIZE 12
#define POLY1305_MAC_SIZE 16

int chacha20_poly1305_encrypt(const u8 *key, const u8 *nonce,
			      const u8 *plaintext, usize plaintext_len,
			      const u8 *aad, usize aad_len, u8 *ciphertext,
			      u8 *mac);

int chacha20_poly1305_decrypt(const u8 *key, const u8 *nonce,
			      const u8 *ciphertext, usize ciphertext_len,
			      const u8 *aad, usize aad_len, const u8 *mac,
			      u8 *plaintext);

#endif // SHYAKE_CRYPTO_H
