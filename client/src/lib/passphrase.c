#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <openssl/evp.h>

#include "lib_internal.h"
#include "shyake_crypto.h"

/*
 * Encrypted secret key file format:
 *
 *   [4  B]  magic   = "SHYK"
 *   [1  B]  version = 0x01
 *   [1  B]  kdf_id  = 0x01  (scrypt)
 *   [32 B]  salt    (random)
 *   [4  B]  N  (LE u32, default 131072)
 *   [4  B]  r  (LE u32, default 8)
 *   [4  B]  p  (LE u32, default 1)
 *   [12 B]  nonce   (ChaCha20-Poly1305, random)
 *   [?  B]  ciphertext  (same length as plaintext key)
 *   [16 B]  mac     (Poly1305 tag; header bytes 0..61 are AAD)
 */

#define HEADER_LEN 62 /* 4+1+1+32+4+4+4+12 */
#define SK_MAGIC "SHYK"
#define SK_MAGIC_LEN 4

/* Prevent the compiler from optimizing away the memory wipe */
void zero_memory(void *buf, usize len)
{
	volatile u8 *p = (volatile u8 *)buf;
	while (len--)
		*p++ = 0;
}

static void write_u32le(u8 *buf, u32 v)
{
	buf[0] = (u8)(v);
	buf[1] = (u8)(v >> 8);
	buf[2] = (u8)(v >> 16);
	buf[3] = (u8)(v >> 24);
}

static u32 read_u32le(const u8 *buf)
{
	return (u32)buf[0] | ((u32)buf[1] << 8) | ((u32)buf[2] << 16) |
	       ((u32)buf[3] << 24);
}

static int fill_random(u8 *buf, usize len)
{
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f)
		return 0;
	int ok = (fread(buf, 1, len, f) == len);
	fclose(f);
	return ok;
}

static int derive_key(const char *passphrase, const u8 *salt, u32 N, u32 r,
		      u32 p, u8 *out)
{
	/* 128 * N * r bytes of working memory, plus 50% headroom */
	u64 maxmem = (u64)128 * N * r * 3 / 2;
	if (maxmem < 64ULL * 1024 * 1024)
		maxmem = 64ULL * 1024 * 1024;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	int ret = EVP_PBE_scrypt(passphrase, strlen(passphrase), salt, 32,
				 (u64)N, (u64)r, (u64)p, maxmem, out,
				 CHACHA20_KEY_SIZE);
#pragma GCC diagnostic pop
	return ret == 1 ? 0 : -1;
}

int save_sk_encrypted(const char *path, const char *passphrase, const u8 *sk,
		      usize sk_len)
{
	if (!passphrase || passphrase[0] == '\0')
		return save_file(path, sk, sk_len);

	u8 header[HEADER_LEN];
	memcpy(header, SK_MAGIC, SK_MAGIC_LEN);
	header[4] = 0x01; /* version */
	header[5] = 0x01; /* kdf = scrypt */

	if (!fill_random(header + 6, 32)) /* salt */
		return -1;
	if (!fill_random(header + 50, 12)) /* nonce */
		return -1;

	u32 N = 65536, r = 8, p = 1;
	write_u32le(header + 38, N);
	write_u32le(header + 42, r);
	write_u32le(header + 46, p);

	u8 derived[CHACHA20_KEY_SIZE];
	if (derive_key(passphrase, header + 6, N, r, p, derived) != 0)
		return -1;

	u8 *ct = malloc(sk_len);
	if (!ct) {
		zero_memory(derived, sizeof(derived));
		return -1;
	}
	u8 mac[POLY1305_MAC_SIZE];

	int enc_ret = chacha20_poly1305_encrypt(
		derived, header + 50, /* nonce at offset 50 */
		sk, sk_len, header, HEADER_LEN, /* full header as AAD */
		ct, mac);
	zero_memory(derived, sizeof(derived));

	if (enc_ret != 0) {
		free(ct);
		return -1;
	}

	FILE *f = fopen(path, "wb");
	if (!f) {
		free(ct);
		return -1;
	}
	fwrite(header, 1, HEADER_LEN, f);
	fwrite(ct, 1, sk_len, f);
	fwrite(mac, 1, POLY1305_MAC_SIZE, f);
	fclose(f);
	free(ct);
	return 0;
}

u8 *load_sk_decrypted(shyake_ctx *ctx, const char *path, usize *out_len)
{
	const char *passphrase = ctx->passphrase;
	usize file_len;
	u8 *data = load_file(path, &file_len);
	if (!data) {
		set_error(ctx, "Cannot load key file: %s", path);
		return NULL;
	}

	/* No magic header → legacy raw binary, return as-is */
	if (file_len < SK_MAGIC_LEN ||
	    memcmp(data, SK_MAGIC, SK_MAGIC_LEN) != 0) {
		*out_len = file_len;
		return data;
	}

	if (!passphrase || passphrase[0] == '\0') {
		set_error(ctx, "Key is encrypted; passphrase required.");
		free(data);
		return NULL;
	}

	if (file_len < (usize)(HEADER_LEN + POLY1305_MAC_SIZE)) {
		set_error(ctx, "Key file is corrupted.");
		free(data);
		return NULL;
	}

	if (data[4] != 0x01 || data[5] != 0x01) {
		set_error(ctx, "Unsupported key file format.");
		free(data);
		return NULL;
	}

	u32 N = read_u32le(data + 38);
	u32 r = read_u32le(data + 42);
	u32 p = read_u32le(data + 46);

	usize ct_len = file_len - HEADER_LEN - POLY1305_MAC_SIZE;
	u8 *ct = data + HEADER_LEN;
	u8 *mac = data + HEADER_LEN + ct_len;

	u8 derived[CHACHA20_KEY_SIZE];
	if (derive_key(passphrase, data + 6, N, r, p, derived) != 0) {
		free(data);
		return NULL;
	}

	u8 *sk = malloc(ct_len);
	if (!sk) {
		zero_memory(derived, sizeof(derived));
		free(data);
		return NULL;
	}

	int dec_ret = chacha20_poly1305_decrypt(
		derived, data + 50, /* nonce at offset 50 */
		ct, ct_len, data, HEADER_LEN, /* full header as AAD */
		mac, sk);
	zero_memory(derived, sizeof(derived));

	free(data);

	if (dec_ret != 0) {
		free(sk);
		set_error(ctx, "Incorrect passphrase.");
		return NULL;
	}

	*out_len = ct_len;
	return sk;
}
