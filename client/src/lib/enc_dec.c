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

static void write_u32le(FILE *f, u32 v)
{
	u8 buf[4];
	buf[0] = v & 0xff;
	buf[1] = (v >> 8) & 0xff;
	buf[2] = (v >> 16) & 0xff;
	buf[3] = (v >> 24) & 0xff;
	fwrite(buf, 1, 4, f);
}

static u32 read_u32le(FILE *f)
{
	u8 buf[4];
	if (fread(buf, 1, 4, f) != 4)
		return 0;
	return (u32)buf[0] | ((u32)buf[1] << 8) | ((u32)buf[2] << 16) |
	       ((u32)buf[3] << 24);
}

/* whole file into memory; NULL when it cannot be read */
static u8 *read_input(shyake_ctx *ctx, const char *path, usize *len)
{
	FILE *in = fopen(path, "rb");
	if (!in) {
		set_error(ctx, "Cannot open %s.", path);
		return NULL;
	}
	fseek(in, 0, SEEK_END);
	long fsz = ftell(in);
	fseek(in, 0, SEEK_SET);
	u8 *data = NULL;
	if (fsz >= 0) {
		*len = (usize)fsz;
		data = malloc(*len);
		if (data && fread(data, 1, *len, in) != *len) {
			free(data);
			data = NULL;
		}
	}
	fclose(in);
	return data;
}

/* KEM public key to encrypt to: the recipient's, else our own */
static u8 *target_kem_pk(shyake_ctx *ctx, const char *recipient, usize *len,
			 shyake_err *err)
{
	*err = SHYAKE_ERR_CRYPTO;
	if (!recipient) {
		char path[512];
		config_file(ctx->config_dir, KEM_PK_FILE, path, sizeof(path));
		return load_file(path, len);
	}
	char *pk_b64 = fetch_recipient_pubkey(ctx, recipient, err);
	if (!pk_b64)
		return NULL;
	*err = SHYAKE_ERR_CRYPTO;
	u8 *pk = base64_decode(pk_b64, len);
	free(pk_b64);
	return pk;
}

shyake_err shyake_enc_file(shyake_ctx *ctx, const char *in_path,
			   const char *out_path, const char *recipient,
			   char **out_used_path)
{
	if (!ctx || !in_path)
		return SHYAKE_ERR;
	clear_error(ctx);

	usize pt_len;
	u8 *pt = read_input(ctx, in_path, &pt_len);
	if (!pt)
		return SHYAKE_ERR;
	shyake_err ret;
	usize kem_pk_len = 0;
	u8 *kem_pk = target_kem_pk(ctx, recipient, &kem_pk_len, &ret);
	if (!kem_pk) {
		free(pt);
		return ret;
	}

	ret = SHYAKE_ERR_CRYPTO;
	OQS_KEM *kem = NULL;
	u8 *kem_ct = NULL, *ss = NULL, *ct = NULL;
	u8 sym_key[CHACHA20_KEY_SIZE], ek[CHACHA20_KEY_SIZE],
		nonce[CHACHA20_NONCE_SIZE], mac[POLY1305_MAC_SIZE];
	if (!fill_random(sym_key, sizeof(sym_key)))
		goto done;

	/* KEM encapsulate; the shared secret masks the file key */
	kem = OQS_KEM_new("ML-KEM-768");
	if (!kem || kem_pk_len != kem->length_public_key)
		goto done;
	kem_ct = malloc(kem->length_ciphertext);
	ss = malloc(kem->length_shared_secret);
	if (OQS_KEM_encaps(kem, kem_ct, ss, kem_pk) != OQS_SUCCESS)
		goto done;
	for (int i = 0; i < CHACHA20_KEY_SIZE; i++)
		ek[i] = sym_key[i] ^ ss[i];

	/* encrypt */
	if (!fill_random(nonce, sizeof(nonce)))
		goto done;
	ct = malloc(pt_len);
	if (chacha20_poly1305_encrypt(sym_key, nonce, pt, pt_len, NULL, 0, ct,
				      mac) != 0)
		goto done;

	char derived_path[640];
	const char *dest = out_path;
	if (!dest) {
		snprintf(derived_path, sizeof(derived_path), "%s.enc", in_path);
		dest = derived_path;
	}
	FILE *f = fopen(dest, "wb");
	if (!f) {
		set_error(ctx, "Cannot write %s.", dest);
		ret = SHYAKE_ERR;
		goto done;
	}
	write_u32le(f, (u32)kem->length_ciphertext);
	fwrite(kem_ct, 1, kem->length_ciphertext, f);
	write_u32le(f, sizeof(ek));
	fwrite(ek, 1, sizeof(ek), f);
	write_u32le(f, (u32)pt_len);
	fwrite(nonce, 1, sizeof(nonce), f);
	fwrite(ct, 1, pt_len, f);
	fwrite(mac, 1, sizeof(mac), f);
	fclose(f);

	if (out_used_path)
		*out_used_path = strdup(dest);
	ret = SHYAKE_OK;
done:
	zero_memory(sym_key, sizeof(sym_key));
	zero_memory(ek, sizeof(ek));
	free(ct);
	free(kem_ct);
	free_secret(ss, kem ? kem->length_shared_secret : 0);
	OQS_KEM_free(kem);
	free(pt);
	free(kem_pk);
	return ret;
}

/* the fields of a .enc file, in file order */
struct enc_file {
	u32 kem_ct_len;
	u8 *kem_ct;
	u8 ek[CHACHA20_KEY_SIZE];
	u32 pt_len;
	u8 nonce[CHACHA20_NONCE_SIZE];
	u8 *ct;
	u8 mac[POLY1305_MAC_SIZE];
};

/*
 * Read a .enc file. Lengths come from the file, so each is checked
 * before use: the KEM ciphertext must be kem_ct_len bytes, and the
 * ciphertext must be exactly what remains before the MAC.
 */
static int read_enc_file(FILE *in, usize kem_ct_len, struct enc_file *e)
{
	if (fseek(in, 0, SEEK_END) != 0)
		return -1;
	long size = ftell(in);
	rewind(in);
	if (size < 0)
		return -1;
	usize left = (usize)size;

	e->kem_ct_len = read_u32le(in);
	if (e->kem_ct_len != kem_ct_len || left < 4 + (usize)e->kem_ct_len)
		return -1;
	left -= 4 + e->kem_ct_len;
	e->kem_ct = malloc(e->kem_ct_len);
	if (!e->kem_ct ||
	    fread(e->kem_ct, 1, e->kem_ct_len, in) != e->kem_ct_len)
		return -1;
	if (left < 4 + sizeof(e->ek) + 4 || read_u32le(in) != sizeof(e->ek) ||
	    fread(e->ek, 1, sizeof(e->ek), in) != sizeof(e->ek))
		return -1;
	left -= 4 + sizeof(e->ek) + 4;
	e->pt_len = read_u32le(in);
	if (left != sizeof(e->nonce) + (usize)e->pt_len + sizeof(e->mac))
		return -1;
	if (fread(e->nonce, 1, sizeof(e->nonce), in) != sizeof(e->nonce))
		return -1;
	e->ct = malloc(e->pt_len ? e->pt_len : 1);
	if (!e->ct || fread(e->ct, 1, e->pt_len, in) != e->pt_len)
		return -1;
	if (fread(e->mac, 1, sizeof(e->mac), in) != sizeof(e->mac))
		return -1;
	return 0;
}

shyake_err shyake_dec_file_buf(shyake_ctx *ctx, const char *in_path,
			       uint8_t **out, size_t *out_len)
{
	if (!ctx || !in_path || !out || !out_len)
		return SHYAKE_ERR;
	clear_error(ctx);
	*out = NULL;
	*out_len = 0;

	FILE *in = fopen(in_path, "rb");
	if (!in) {
		set_error(ctx, "Cannot open %s.", in_path);
		return SHYAKE_ERR;
	}
	struct enc_file e = { 0 };
	int bad = read_enc_file(in, OQS_KEM_ml_kem_768_length_ciphertext, &e);
	fclose(in);
	if (bad) {
		free(e.kem_ct);
		free(e.ct);
		set_error(ctx, "The file is not a valid .enc file.");
		return SHYAKE_ERR;
	}

	shyake_err ret = SHYAKE_ERR_CRYPTO;
	OQS_KEM *kem = NULL;
	u8 *ss = NULL, *pt = NULL;
	u8 sym_key[CHACHA20_KEY_SIZE];
	char ksk_path[512];
	usize ksk_len = 0;
	config_file(ctx->config_dir, KEM_SK_FILE, ksk_path, sizeof(ksk_path));
	u8 *ksk = load_sk_decrypted(ctx, ksk_path, &ksk_len);
	if (!ksk)
		goto done;
	kem = OQS_KEM_new("ML-KEM-768");
	if (!kem)
		goto done;
	ss = malloc(kem->length_shared_secret);
	if (OQS_KEM_decaps(kem, ss, e.kem_ct, ksk) != OQS_SUCCESS) {
		set_error(ctx, "The file is not encrypted to your key.");
		goto done;
	}
	for (int i = 0; i < CHACHA20_KEY_SIZE; i++)
		sym_key[i] = e.ek[i] ^ ss[i];

	/* one spare byte so an empty plaintext is still a buffer */
	pt = malloc((usize)e.pt_len + 1);
	if (!pt || chacha20_poly1305_decrypt(sym_key, e.nonce, e.ct, e.pt_len,
					     NULL, 0, e.mac, pt) != 0) {
		set_error(ctx, "The file is damaged or was changed.");
		goto done;
	}
	*out = pt;
	*out_len = e.pt_len;
	pt = NULL;
	ret = SHYAKE_OK;
done:
	zero_memory(sym_key, sizeof(sym_key));
	free(pt);
	free_secret(ss, kem ? kem->length_shared_secret : 0);
	free_secret(ksk, ksk_len);
	OQS_KEM_free(kem);
	free(e.kem_ct);
	free(e.ct);
	return ret;
}

shyake_err shyake_dec_file(shyake_ctx *ctx, const char *in_path,
			   const char *out_path)
{
	if (!ctx || !in_path)
		return SHYAKE_ERR;
	if (!out_path) {
		set_error(ctx, "No output file was given.");
		return SHYAKE_ERR;
	}
	u8 *pt;
	usize len;
	shyake_err ret = shyake_dec_file_buf(ctx, in_path, &pt, &len);
	if (ret != SHYAKE_OK)
		return ret;
	FILE *out = fopen(out_path, "wb");
	if (out) {
		fwrite(pt, 1, len, out);
		fclose(out);
	} else {
		set_error(ctx, "Cannot write %s.", out_path);
		ret = SHYAKE_ERR;
	}
	free(pt);
	return ret;
}
