#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <oqs/oqs.h>
#include "lib_internal.h"
#include "shyake_crypto.h"

/*
 * File format written by shyake_enc_file:
 *
 *   [4 bytes LE] kem_ct_len
 *   [kem_ct_len] KEM ciphertext
 *   [4 bytes LE] ek_len  (always 32)
 *   [32 bytes]   encrypted symmetric key (XOR with KEM shared secret)
 *   [4 bytes LE] pt_len
 *   [12 bytes]   ChaCha20-Poly1305 nonce
 *   [pt_len]     ciphertext
 *   [16 bytes]   Poly1305 MAC
 */

static void write_u32le(FILE *f, uint32_t v)
{
	uint8_t buf[4];
	buf[0] = v & 0xff;
	buf[1] = (v >> 8) & 0xff;
	buf[2] = (v >> 16) & 0xff;
	buf[3] = (v >> 24) & 0xff;
	fwrite(buf, 1, 4, f);
}

static uint32_t read_u32le(FILE *f)
{
	uint8_t buf[4];
	if (fread(buf, 1, 4, f) != 4)
		return 0;
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

shyake_err shyake_enc_file(shyake_ctx *ctx, const char *in_path,
			   const char *out_path, const char *recipient,
			   char **out_used_path)
{
	if (!ctx || !in_path)
		return SHYAKE_ERR;

	/* load plaintext */
	FILE *in = fopen(in_path, "rb");
	if (!in) {
		set_error(ctx, "Cannot open input file: %s", in_path);
		return SHYAKE_ERR;
	}
	fseek(in, 0, SEEK_END);
	long fsz = ftell(in);
	fseek(in, 0, SEEK_SET);
	if (fsz < 0) {
		fclose(in);
		return SHYAKE_ERR;
	}
	size_t pt_len = (size_t)fsz;
	uint8_t *pt = malloc(pt_len);
	if (!pt || fread(pt, 1, pt_len, in) != pt_len) {
		free(pt);
		fclose(in);
		return SHYAKE_ERR;
	}
	fclose(in);

	/* resolve KEM public key */
	uint8_t *kem_pk = NULL;
	size_t kem_pk_len = 0;

	if (recipient) {
		char *pk_b64 = fetch_recipient_pubkey(ctx, recipient);
		if (!pk_b64) {
			set_error(ctx, "Failed to fetch pubkey for %s.",
				  recipient);
			free(pt);
			return SHYAKE_ERR_NETWORK;
		}
		kem_pk = base64_decode(pk_b64, &kem_pk_len);
		free(pk_b64);
	} else {
		char path[512];
		snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
		kem_pk = load_file(path, &kem_pk_len);
	}

	if (!kem_pk) {
		free(pt);
		return SHYAKE_ERR_CRYPTO;
	}

	/* generate random symmetric key */
	uint8_t sym_key[32];
	FILE *ur = fopen("/dev/urandom", "rb");
	if (!ur || fread(sym_key, 1, 32, ur) != 32) {
		if (ur)
			fclose(ur);
		free(pt);
		free(kem_pk);
		return SHYAKE_ERR_CRYPTO;
	}
	fclose(ur);

	/* KEM encapsulate */
	OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
	if (!kem || kem_pk_len != kem->length_public_key) {
		if (kem)
			OQS_KEM_free(kem);
		free(pt);
		free(kem_pk);
		return SHYAKE_ERR_CRYPTO;
	}

	uint8_t *kem_ct = malloc(kem->length_ciphertext);
	uint8_t *ss = malloc(kem->length_shared_secret);
	if (OQS_KEM_encaps(kem, kem_ct, ss, kem_pk) != OQS_SUCCESS) {
		free(kem_ct);
		free(ss);
		OQS_KEM_free(kem);
		free(pt);
		free(kem_pk);
		return SHYAKE_ERR_CRYPTO;
	}

	uint8_t ek[32];
	for (int i = 0; i < 32; i++)
		ek[i] = sym_key[i] ^ ss[i];

	/* generate nonce */
	uint8_t nonce[12];
	ur = fopen("/dev/urandom", "rb");
	if (!ur || fread(nonce, 1, 12, ur) != 12) {
		if (ur)
			fclose(ur);
		free(kem_ct);
		free(ss);
		OQS_KEM_free(kem);
		free(pt);
		free(kem_pk);
		return SHYAKE_ERR_CRYPTO;
	}
	fclose(ur);

	/* encrypt */
	uint8_t *ct = malloc(pt_len);
	uint8_t mac[16];
	if (chacha20_poly1305_encrypt(sym_key, nonce, pt, pt_len, NULL, 0, ct,
				      mac) != 0) {
		free(ct);
		free(kem_ct);
		free(ss);
		OQS_KEM_free(kem);
		free(pt);
		free(kem_pk);
		return SHYAKE_ERR_CRYPTO;
	}

	/* determine output path */
	char derived_path[640];
	const char *dest = out_path;
	if (!dest) {
		snprintf(derived_path, sizeof(derived_path), "%s.enc", in_path);
		dest = derived_path;
	}

	FILE *out = fopen(dest, "wb");
	if (!out) {
		set_error(ctx, "Cannot open output file: %s", dest);
		free(ct);
		free(kem_ct);
		free(ss);
		OQS_KEM_free(kem);
		free(pt);
		free(kem_pk);
		return SHYAKE_ERR;
	}

	write_u32le(out, (uint32_t)kem->length_ciphertext);
	fwrite(kem_ct, 1, kem->length_ciphertext, out);
	write_u32le(out, 32);
	fwrite(ek, 1, 32, out);
	write_u32le(out, (uint32_t)pt_len);
	fwrite(nonce, 1, 12, out);
	fwrite(ct, 1, pt_len, out);
	fwrite(mac, 1, 16, out);
	fclose(out);

	free(ct);
	free(kem_ct);
	free(ss);
	OQS_KEM_free(kem);
	free(pt);
	free(kem_pk);

	if (out_used_path)
		*out_used_path = strdup(dest);
	return SHYAKE_OK;
}

shyake_err shyake_dec_file(shyake_ctx *ctx, const char *in_path,
			   const char *out_path)
{
	if (!ctx || !in_path)
		return SHYAKE_ERR;

	FILE *in = fopen(in_path, "rb");
	if (!in) {
		set_error(ctx, "Cannot open input file: %s", in_path);
		return SHYAKE_ERR;
	}

	uint32_t kem_ct_len = read_u32le(in);
	uint8_t *kem_ct = malloc(kem_ct_len);
	if (fread(kem_ct, 1, kem_ct_len, in) != kem_ct_len)
		goto bad_format;

	uint32_t ek_len = read_u32le(in);
	if (ek_len != 32)
		goto bad_format;
	uint8_t ek[32];
	if (fread(ek, 1, 32, in) != 32)
		goto bad_format;

	uint32_t pt_len = read_u32le(in);
	uint8_t nonce[12];
	if (fread(nonce, 1, 12, in) != 12)
		goto bad_format;
	uint8_t *ct = malloc(pt_len);
	if (fread(ct, 1, pt_len, in) != pt_len) {
		free(ct);
		goto bad_format;
	}
	uint8_t mac[16];
	if (fread(mac, 1, 16, in) != 16) {
		free(ct);
		goto bad_format;
	}
	fclose(in);

	/* load own KEM secret key */
	char ksk_path[512];
	size_t ksk_len;
	snprintf(ksk_path, sizeof(ksk_path), "%s/kem_sk.bin", ctx->config_dir);
	uint8_t *ksk = load_sk_decrypted(ctx, ksk_path, &ksk_len);
	if (!ksk) {
		free(kem_ct);
		free(ct);
		return SHYAKE_ERR_CRYPTO;
	}

	OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
	if (!kem) {
		free(kem_ct);
		free(ct);
		free(ksk);
		return SHYAKE_ERR_CRYPTO;
	}

	uint8_t *ss = malloc(kem->length_shared_secret);
	if (OQS_KEM_decaps(kem, ss, kem_ct, ksk) != OQS_SUCCESS) {
		free(ss);
		free(kem_ct);
		free(ct);
		free(ksk);
		OQS_KEM_free(kem);
		set_error(ctx, "Decryption failed: wrong key.");
		return SHYAKE_ERR_CRYPTO;
	}

	uint8_t sym_key[32];
	for (int i = 0; i < 32; i++)
		sym_key[i] = ek[i] ^ ss[i];

	free(ss);
	free(kem_ct);
	free(ksk);
	OQS_KEM_free(kem);

	uint8_t *pt = malloc(pt_len);
	if (chacha20_poly1305_decrypt(sym_key, nonce, ct, pt_len, NULL, 0, mac,
				      pt) != 0) {
		free(pt);
		free(ct);
		set_error(ctx, "Decryption failed: authentication error.");
		return SHYAKE_ERR_CRYPTO;
	}
	free(ct);

	FILE *out;
	if (out_path) {
		out = fopen(out_path, "wb");
		if (!out) {
			set_error(ctx, "Cannot open output file: %s", out_path);
			free(pt);
			return SHYAKE_ERR;
		}
	} else {
		out = stdout;
	}

	fwrite(pt, 1, pt_len, out);
	if (out_path)
		fclose(out);

	free(pt);
	return SHYAKE_OK;

bad_format:
	fclose(in);
	free(kem_ct);
	set_error(ctx, "Invalid or corrupted .enc file.");
	return SHYAKE_ERR;
}
