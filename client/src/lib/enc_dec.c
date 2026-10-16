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
		snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
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
	u8 sym_key[32], ek[32], nonce[12], mac[16];
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
	for (int i = 0; i < 32; i++)
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
	write_u32le(f, 32);
	fwrite(ek, 1, 32, f);
	write_u32le(f, (u32)pt_len);
	fwrite(nonce, 1, 12, f);
	fwrite(ct, 1, pt_len, f);
	fwrite(mac, 1, 16, f);
	fclose(f);

	if (out_used_path)
		*out_used_path = strdup(dest);
	ret = SHYAKE_OK;
done:
	free(ct);
	free(kem_ct);
	free(ss);
	OQS_KEM_free(kem);
	free(pt);
	free(kem_pk);
	return ret;
}

/* the fields of a .enc file, in file order */
struct enc_file {
	u32 kem_ct_len;
	u8 *kem_ct;
	u8 ek[32];
	u32 pt_len;
	u8 nonce[12];
	u8 *ct;
	u8 mac[16];
};

static int read_enc_file(FILE *in, struct enc_file *e)
{
	e->kem_ct_len = read_u32le(in);
	e->kem_ct = malloc(e->kem_ct_len);
	if (fread(e->kem_ct, 1, e->kem_ct_len, in) != e->kem_ct_len)
		return -1;
	if (read_u32le(in) != 32 || fread(e->ek, 1, 32, in) != 32)
		return -1;
	e->pt_len = read_u32le(in);
	if (fread(e->nonce, 1, 12, in) != 12)
		return -1;
	e->ct = malloc(e->pt_len);
	if (fread(e->ct, 1, e->pt_len, in) != e->pt_len)
		return -1;
	if (fread(e->mac, 1, 16, in) != 16)
		return -1;
	return 0;
}

/* plaintext to out_path, or to stdout without one */
static shyake_err write_output(shyake_ctx *ctx, const char *out_path,
			       const u8 *pt, usize len)
{
	if (!out_path) {
		fwrite(pt, 1, len, stdout);
		return SHYAKE_OK;
	}
	FILE *out = fopen(out_path, "wb");
	if (!out) {
		set_error(ctx, "Cannot write %s.", out_path);
		return SHYAKE_ERR;
	}
	fwrite(pt, 1, len, out);
	fclose(out);
	return SHYAKE_OK;
}

shyake_err shyake_dec_file(shyake_ctx *ctx, const char *in_path,
			   const char *out_path)
{
	if (!ctx || !in_path)
		return SHYAKE_ERR;
	clear_error(ctx);

	FILE *in = fopen(in_path, "rb");
	if (!in) {
		set_error(ctx, "Cannot open %s.", in_path);
		return SHYAKE_ERR;
	}
	struct enc_file e = { 0 };
	int bad = read_enc_file(in, &e);
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
	u8 sym_key[32];
	char ksk_path[512];
	usize ksk_len;
	snprintf(ksk_path, sizeof(ksk_path), "%s/kem_sk.bin", ctx->config_dir);
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
	for (int i = 0; i < 32; i++)
		sym_key[i] = e.ek[i] ^ ss[i];

	pt = malloc(e.pt_len);
	if (chacha20_poly1305_decrypt(sym_key, e.nonce, e.ct, e.pt_len, NULL, 0,
				      e.mac, pt) != 0) {
		set_error(ctx, "The file is damaged or was changed.");
		goto done;
	}
	ret = write_output(ctx, out_path, pt, e.pt_len);
done:
	free(pt);
	free(ss);
	free(ksk);
	OQS_KEM_free(kem);
	free(e.kem_ct);
	free(e.ct);
	return ret;
}
