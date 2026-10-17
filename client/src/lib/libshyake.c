#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <oqs/oqs.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <curl/curl.h>
#include "vendor/cJSON/cJSON.h"

#include <stdarg.h>

#include "lib_internal.h"
#include "shyake_crypto.h"

/* ------------------------------------------------------------------ */
/* Error reporting                                                    */
/* ------------------------------------------------------------------ */

void set_error(shyake_ctx *ctx, const char *fmt, ...)
{
	if (!ctx)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap);
	va_end(ap);
}

void clear_error(shyake_ctx *ctx)
{
	if (ctx)
		ctx->last_error[0] = '\0';
}

const char *shyake_last_error(shyake_ctx *ctx)
{
	return ctx ? ctx->last_error : "";
}

/* ------------------------------------------------------------------ */
/* File I/O helpers                                                   */
/* ------------------------------------------------------------------ */

/* path of a file in the config directory */
void config_file(const char *config_dir, const char *name, char *buf, usize len)
{
	snprintf(buf, len, "%s/%s", config_dir, name);
}

int save_file(const char *path, const u8 *data, usize len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	usize written = fwrite(data, 1, len, f);
	fclose(f);
	return written == len ? 0 : -1;
}

u8 *load_file(const char *path, usize *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *data = malloc(*len);
	if (data) {
		if (fread(data, 1, *len, f) != *len) {
			free(data);
			data = NULL;
		}
	}
	fclose(f);
	return data;
}

/* len bytes from the system's random source; 0 on failure */
int fill_random(u8 *buf, usize len)
{
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f)
		return 0;
	int ok = (fread(buf, 1, len, f) == len);
	fclose(f);
	return ok;
}

/* lowercase hex of len bytes into out, which holds 2 * len + 1 */
void hex_encode(const u8 *in, usize len, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (usize i = 0; i < len; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 0x0f];
	}
	out[2 * len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Base64                                                             */
/* ------------------------------------------------------------------ */

char *base64_encode(const u8 *data, usize len)
{
	int out_len = 4 * ((len + 2) / 3);
	char *out = malloc(out_len + 1);
	if (!out)
		return NULL;
	EVP_EncodeBlock((unsigned char *)out, data, len);
	return out;
}

/*
 * Standard or URL-safe base64, padded or not; trailing blanks and
 * line ends are ignored. NULL when the text is not base64.
 */
u8 *base64_decode(const char *b64, usize *out_len)
{
	usize len = strlen(b64);
	while (len > 0 && (b64[len - 1] == '\r' || b64[len - 1] == '\n' ||
			   b64[len - 1] == ' '))
		len--;
	if (len % 4 == 1)
		return NULL;

	/* standard alphabet, padded to a whole quantum */
	usize padded = (len + 3) / 4 * 4;
	char *std = malloc(padded + 1);
	u8 *out = malloc(padded / 4 * 3 + 1);
	if (!std || !out) {
		free(std);
		free(out);
		return NULL;
	}
	for (usize i = 0; i < len; i++)
		std[i] = b64[i] == '-' ? '+' : b64[i] == '_' ? '/' : b64[i];
	memset(std + len, '=', padded - len);
	std[padded] = '\0';

	int dec_len =
		EVP_DecodeBlock(out, (const unsigned char *)std, (int)padded);
	if (dec_len < 0) {
		free(std);
		free(out);
		return NULL;
	}
	/* EVP_DecodeBlock counts padding as zero bytes */
	for (usize i = padded; i > 0 && std[i - 1] == '='; i--)
		dec_len--;
	free(std);
	*out_len = (usize)dec_len;
	return out;
}

/* ------------------------------------------------------------------ */
/* Context lifecycle                                                  */
/* ------------------------------------------------------------------ */

shyake_ctx *shyake_init_ctx(const shyake_config *config)
{
	/* initialize context */
	if (!config || !config->config_dir)
		return NULL;

	shyake_ctx *ctx = calloc(1, sizeof(shyake_ctx));
	if (!ctx)
		return NULL;

	ctx->config_dir = strdup(config->config_dir);
	if (config->instance_url)
		ctx->instance_url = strdup(config->instance_url);
	if (config->username)
		ctx->username = strdup(config->username);

	ctx->debug = config->debug;

	curl_global_init(CURL_GLOBAL_ALL);
	return ctx;
}

void shyake_free_ctx(shyake_ctx *ctx)
{
	if (!ctx)
		return;
	free(ctx->config_dir);
	free(ctx->instance_url);
	free(ctx->username);
	volatile u8 *vp = (volatile u8 *)ctx->passphrase;
	for (usize i = 0; i < sizeof(ctx->passphrase); i++)
		vp[i] = 0;
	vp = (volatile u8 *)ctx->new_passphrase;
	for (usize i = 0; i < sizeof(ctx->new_passphrase); i++)
		vp[i] = 0;
	free(ctx);
	curl_global_cleanup();
}

void shyake_set_passphrase(shyake_ctx *ctx, const char *passphrase)
{
	if (!ctx)
		return;
	snprintf(ctx->passphrase, sizeof(ctx->passphrase), "%s",
		 passphrase ? passphrase : "");
}

void shyake_set_new_passphrase(shyake_ctx *ctx, const char *passphrase)
{
	if (!ctx)
		return;
	snprintf(ctx->new_passphrase, sizeof(ctx->new_passphrase), "%s",
		 passphrase ? passphrase : "");
}

/* ------------------------------------------------------------------ */
/* Key generation                                                     */
/* ------------------------------------------------------------------ */

/* write one key pair unless its public key exists */
static shyake_err generate_pair(shyake_ctx *ctx, const char *pk_file,
				const char *sk_file, int is_kem)
{
	char path_pk[512], path_sk[512];
	config_file(ctx->config_dir, pk_file, path_pk, sizeof(path_pk));
	config_file(ctx->config_dir, sk_file, path_sk, sizeof(path_sk));
	struct stat st = { 0 };
	if (stat(path_pk, &st) == 0)
		return SHYAKE_OK;

	OQS_KEM *kem = is_kem ? OQS_KEM_new("ML-KEM-768") : NULL;
	OQS_SIG *sig = is_kem ? NULL : OQS_SIG_new("ML-DSA-65");
	if (!kem && !sig) {
		set_error(ctx, "The crypto library cannot make keys.");
		return SHYAKE_ERR_CRYPTO;
	}
	usize pk_len = kem ? kem->length_public_key : sig->length_public_key;
	usize sk_len = kem ? kem->length_secret_key : sig->length_secret_key;
	u8 *pk = malloc(pk_len);
	u8 *sk = malloc(sk_len);
	OQS_STATUS rc = kem ? OQS_KEM_keypair(kem, pk, sk) :
			      OQS_SIG_keypair(sig, pk, sk);
	shyake_err ret = SHYAKE_OK;
	if (rc != OQS_SUCCESS) {
		set_error(ctx, "The crypto library cannot make keys.");
		ret = SHYAKE_ERR_CRYPTO;
	} else if (save_file(path_pk, pk, pk_len) != 0) {
		set_error(ctx, "Cannot write %s.", path_pk);
		ret = SHYAKE_ERR;
	} else if (save_sk_encrypted(path_sk, ctx->passphrase, sk, sk_len) !=
		   0) {
		set_error(ctx, "Cannot write %s.", path_sk);
		ret = SHYAKE_ERR;
	}
	free(pk);
	free_secret(sk, sk_len);
	OQS_KEM_free(kem);
	OQS_SIG_free(sig);
	return ret;
}

shyake_err shyake_generate_keys(shyake_ctx *ctx)
{
	if (!ctx || !ctx->config_dir)
		return SHYAKE_ERR;
	clear_error(ctx);
	shyake_err ret = generate_pair(ctx, KEM_PK_FILE, KEM_SK_FILE, 1);
	if (ret == SHYAKE_OK)
		ret = generate_pair(ctx, SIG_PK_FILE, SIG_SK_FILE, 0);
	return ret;
}

/* ------------------------------------------------------------------ */
/* PoW                                                                */
/* ------------------------------------------------------------------ */

char *shyake_mint_pow(const char *resource, int bits)
{
	time_t now = time(NULL);
	struct tm *tm_info = gmtime(&now);
	char date[7];
	strftime(date, 7, "%y%m%d", tm_info);

	char rand_str[13];
	const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	u8 rnd[12];
	FILE *ur = fopen("/dev/urandom", "rb");
	int have_rnd = 0;
	if (ur) {
		have_rnd = (fread(rnd, 1, 12, ur) == 12);
		fclose(ur);
	}
	if (!have_rnd) {
		/* fallback: seed once from time and stack address */
		srand((unsigned)(time(NULL) ^ (uptr)rnd));
		for (int i = 0; i < 12; i++)
			rnd[i] = (u8)rand();
	}
	for (int i = 0; i < 12; i++)
		rand_str[i] = charset[rnd[i] % 36];
	rand_str[12] = '\0';

	usize header_sz = 256;
	char *header = malloc(header_sz);
	unsigned long counter = 0;
	unsigned char hash[SHA_DIGEST_LENGTH];
	int bytes_to_check = bits / 8;
	int remaining_bits = bits % 8;

	while (1) {
		snprintf(header, header_sz, "1:%d:%s:%s::%s:%lx", bits, date,
			 resource, rand_str, counter);
		SHA1((unsigned char *)header, strlen(header), hash);
		int match = 1;
		for (int i = 0; i < bytes_to_check; i++) {
			if (hash[i] != 0) {
				match = 0;
				break;
			}
		}
		if (match && remaining_bits > 0) {
			if ((hash[bytes_to_check] >> (8 - remaining_bits)) != 0)
				match = 0;
		}
		if (match)
			return header;
		counter++;
		if (counter == 0xFFFFFFFF)
			break;
	}
	free(header);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

/* signed POST /api/register body (SPEC 4.1) */
static char *register_payload(shyake_ctx *ctx, const char *username)
{
	usize kpk_len, spk_len, ssk_len = 0;
	char path[512];
	config_file(ctx->config_dir, KEM_PK_FILE, path, sizeof(path));
	u8 *kpk = load_file(path, &kpk_len);
	config_file(ctx->config_dir, SIG_PK_FILE, path, sizeof(path));
	u8 *spk = load_file(path, &spk_len);
	config_file(ctx->config_dir, SIG_SK_FILE, path, sizeof(path));
	u8 *ssk = load_sk_decrypted(ctx, path, &ssk_len);
	if (!kpk || !spk || !ssk) {
		free(kpk);
		free(spk);
		free_secret(ssk, ssk_len);
		return NULL;
	}

	char *kpk_b64 = base64_encode(kpk, kpk_len);
	char *spk_b64 = base64_encode(spk, spk_len);
	char timestamp[32];
	snprintf(timestamp, sizeof(timestamp), "%ld", time(NULL));

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "username", username);
	cJSON_AddStringToObject(root, "kem_pubkey", kpk_b64);
	cJSON_AddStringToObject(root, "sig_pubkey", spk_b64);
	cJSON_AddStringToObject(root, "timestamp", timestamp);

	char *json_raw = cJSON_PrintUnformatted(root);
	OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
	u8 *signature = malloc(sig->length_signature);
	usize sig_len;
	OQS_SIG_sign(sig, signature, &sig_len, (u8 *)json_raw, strlen(json_raw),
		     ssk);
	char *sig_b64 = base64_encode(signature, sig_len);
	char *pow = shyake_mint_pow(username, POW_BITS);

	cJSON_AddStringToObject(root, "signature", sig_b64);
	cJSON_AddStringToObject(root, "pow", pow);
	char *payload = cJSON_PrintUnformatted(root);

	free(json_raw);
	free(signature);
	free(sig_b64);
	free(pow);
	free(kpk_b64);
	free(spk_b64);
	free(kpk);
	free(spk);
	free_secret(ssk, ssk_len);
	cJSON_Delete(root);
	OQS_SIG_free(sig);
	return payload;
}

shyake_err shyake_register(shyake_ctx *ctx, const char *username)
{
	if (!ctx || !username)
		return SHYAKE_ERR;
	clear_error(ctx);

	char *payload = register_payload(ctx, username);
	if (!payload)
		return SHYAKE_ERR_CRYPTO;
	if (!ctx->instance_url) {
		free(payload);
		return SHYAKE_ERR_NO_INSTANCE;
	}

	struct http_reply r;
	shyake_err ret =
		http_send(ctx, "POST", "/api/register", NULL, payload, &r);
	free(payload);
	if (ret != SHYAKE_OK)
		return ret;
	if (r.code != 200 && r.code != 201) {
		set_server_error(ctx, r.code, r.body);
		ret = SHYAKE_ERR_HTTP;
	}
	http_reply_free(&r);
	return ret;
}
