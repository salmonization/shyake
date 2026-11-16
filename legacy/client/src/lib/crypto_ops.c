#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <oqs/oqs.h>
#include <openssl/evp.h>
#include "lib_internal.h"
#include "shyake_crypto.h"

char *encrypt_to_b64(const u8 *key, const u8 *pt, usize pt_len)
{
	if (!pt || pt_len == 0)
		return NULL;

	u8 nonce[CHACHA20_NONCE_SIZE];
	if (!fill_random(nonce, sizeof(nonce)))
		return NULL;

	u8 *ct = malloc(pt_len);
	u8 mac[POLY1305_MAC_SIZE];

	if (chacha20_poly1305_encrypt(key, nonce, pt, pt_len, NULL, 0, ct,
				      mac) != 0) {
		free(ct);
		return NULL;
	}

	usize packed_len = sizeof(nonce) + pt_len + sizeof(mac);
	u8 *packed = malloc(packed_len);
	memcpy(packed, nonce, sizeof(nonce));
	memcpy(packed + sizeof(nonce), ct, pt_len);
	memcpy(packed + sizeof(nonce) + pt_len, mac, sizeof(mac));

	char *b64 = base64_encode(packed, packed_len);
	free(ct);
	free(packed);
	return b64;
}

char *decrypt_from_b64(const u8 *key, const char *b64)
{
	if (!b64 || strlen(b64) == 0)
		return NULL;
	usize packed_len;
	u8 *packed = base64_decode(b64, &packed_len);
	if (!packed || packed_len <= 28) {
		free(packed);
		return NULL;
	}

	usize ct_len = packed_len - 28;
	u8 nonce[CHACHA20_NONCE_SIZE];
	u8 mac[POLY1305_MAC_SIZE];
	u8 *ct = packed + sizeof(nonce);
	memcpy(nonce, packed, sizeof(nonce));
	memcpy(mac, packed + sizeof(nonce) + ct_len, sizeof(mac));

	char *pt = malloc(ct_len + 1);
	if (chacha20_poly1305_decrypt(key, nonce, ct, ct_len, NULL, 0, mac,
				      (u8 *)pt) != 0) {
		free(pt);
		free(packed);
		return NULL;
	}
	pt[ct_len] = '\0';
	free(packed);
	return pt;
}

char *kem_encapsulate_key(const u8 *kem_pk, usize kem_pk_len, const u8 *sym_key)
{
	OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
	if (!kem)
		return NULL;

	if (kem_pk_len != kem->length_public_key) {
		OQS_KEM_free(kem);
		return NULL;
	}

	u8 *ct = malloc(kem->length_ciphertext);
	u8 *ss = malloc(kem->length_shared_secret);

	if (OQS_KEM_encaps(kem, ct, ss, kem_pk) != OQS_SUCCESS) {
		free(ct);
		free_secret(ss, kem->length_shared_secret);
		OQS_KEM_free(kem);
		return NULL;
	}

	u8 ek[CHACHA20_KEY_SIZE];
	for (int i = 0; i < CHACHA20_KEY_SIZE; i++)
		ek[i] = sym_key[i] ^ ss[i];

	usize packed_len = kem->length_ciphertext + sizeof(ek);
	u8 *packed = malloc(packed_len);
	memcpy(packed, ct, kem->length_ciphertext);
	memcpy(packed + kem->length_ciphertext, ek, sizeof(ek));

	char *b64 = base64_encode(packed, packed_len);

	free(ct);
	zero_memory(ek, sizeof(ek));
	free_secret(ss, kem->length_shared_secret);
	free(packed);
	OQS_KEM_free(kem);
	return b64;
}

/* --- public self-encryption primitives --- */

char *shyake_selfenc_begin(shyake_ctx *ctx, u8 sym_key[32])
{
	if (!ctx || !sym_key)
		return NULL;

	char path[512];
	usize kpk_len;
	config_file(ctx->config_dir, KEM_PK_FILE, path, sizeof(path));
	u8 *kpk = load_file(path, &kpk_len);
	if (!kpk)
		return NULL;

	if (!fill_random(sym_key, CHACHA20_KEY_SIZE)) {
		free(kpk);
		return NULL;
	}

	char *enc_key = kem_encapsulate_key(kpk, kpk_len, sym_key);
	free(kpk);
	return enc_key;
}

struct shyake_selfdec {
	u8 *ksk;
	usize ksk_len;
};

shyake_selfdec *shyake_selfdec_new(shyake_ctx *ctx)
{
	if (!ctx)
		return NULL;

	char path[512];
	config_file(ctx->config_dir, KEM_SK_FILE, path, sizeof(path));

	shyake_selfdec *sd = calloc(1, sizeof(shyake_selfdec));
	sd->ksk = load_sk_decrypted(ctx, path, &sd->ksk_len);
	if (!sd->ksk) {
		free(sd);
		return NULL;
	}
	return sd;
}

void shyake_selfdec_free(shyake_selfdec *sd)
{
	if (!sd)
		return;
	free_secret(sd->ksk, sd->ksk_len);
	free(sd);
}

shyake_err shyake_selfdec_key(shyake_selfdec *sd, const char *enc_key_b64,
			      u8 sym_key[32])
{
	if (!sd || !enc_key_b64 || !sym_key)
		return SHYAKE_ERR;
	u8 *sym = kem_decapsulate_key(enc_key_b64, sd->ksk);
	if (!sym)
		return SHYAKE_ERR_CRYPTO;
	memcpy(sym_key, sym, CHACHA20_KEY_SIZE);
	free_secret(sym, CHACHA20_KEY_SIZE);
	return SHYAKE_OK;
}

char *shyake_seal_b64(const u8 sym_key[32], const u8 *pt, usize pt_len)
{
	return encrypt_to_b64(sym_key, pt, pt_len);
}

char *shyake_unseal_b64(const u8 sym_key[32], const char *b64)
{
	return decrypt_from_b64(sym_key, b64);
}

u8 *kem_decapsulate_key(const char *enc_key_b64, const u8 *my_sk)
{
	usize packed_len;
	u8 *packed = base64_decode(enc_key_b64, &packed_len);
	if (!packed)
		return NULL;

	OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
	if (!kem) {
		free(packed);
		return NULL;
	}

	if (packed_len != kem->length_ciphertext + CHACHA20_KEY_SIZE) {
		free(packed);
		OQS_KEM_free(kem);
		return NULL;
	}

	u8 *ss = malloc(kem->length_shared_secret);
	if (OQS_KEM_decaps(kem, ss, packed, my_sk) != OQS_SUCCESS) {
		free_secret(ss, kem->length_shared_secret);
		free(packed);
		OQS_KEM_free(kem);
		return NULL;
	}

	u8 *sym_key = malloc(CHACHA20_KEY_SIZE);
	u8 *ek = packed + kem->length_ciphertext;
	for (int i = 0; i < CHACHA20_KEY_SIZE; i++)
		sym_key[i] = ek[i] ^ ss[i];

	free_secret(ss, kem->length_shared_secret);
	zero_memory(packed, packed_len); /* trailing bytes are ek */
	free(packed);
	OQS_KEM_free(kem);
	return sym_key;
}
