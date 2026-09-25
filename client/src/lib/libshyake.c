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

u8 *base64_decode(const char *b64, usize *out_len)
{
	usize len = strlen(b64);
	while (len > 0 && (b64[len - 1] == '\r' || b64[len - 1] == '\n' ||
			   b64[len - 1] == ' '))
		len--;

	u8 *out = malloc(len);
	if (!out)
		return NULL;

	char *std_b64 = strdup(b64);
	for (usize i = 0; i < len; i++) {
		if (std_b64[i] == '-')
			std_b64[i] = '+';
		if (std_b64[i] == '_')
			std_b64[i] = '/';
	}

	int dec_len = EVP_DecodeBlock(out, (const unsigned char *)std_b64, len);
	free(std_b64);

	if (dec_len < 0) {
		free(out);
		return NULL;
	}

	int padding = 0;
	if (len > 0 && b64[len - 1] == '=') {
		padding++;
		if (len > 1 && b64[len - 2] == '=')
			padding++;
	} else {
		int mod = len % 4;
		if (mod == 2)
			padding = 2;
		else if (mod == 3)
			padding = 1;
	}
	dec_len -= padding;

	*out_len = dec_len;
	return out;
}

/* ------------------------------------------------------------------ */
/* Context lifecycle                                                  */
/* ------------------------------------------------------------------ */

shyake_ctx *shyake_init_ctx(const shyake_config *config)
{
	// initialize context
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

	ctx->plain = config->plain;
	ctx->debug = config->debug;
	ctx->no_color = config->no_color;

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

int shyake_generate_keys(shyake_ctx *ctx)
{
	if (!ctx || !ctx->config_dir)
		return -1;

	char path_pk[512], path_sk[512];
	struct stat st = { 0 };
	int ret = 0;

	snprintf(path_pk, sizeof(path_pk), "%s/kem_pk.bin", ctx->config_dir);
	if (stat(path_pk, &st) == -1) {
		OQS_KEM *kem = OQS_KEM_new("ML-KEM-768");
		if (kem) {
			u8 *pk = malloc(kem->length_public_key);
			u8 *sk = malloc(kem->length_secret_key);
			if (OQS_KEM_keypair(kem, pk, sk) == OQS_SUCCESS) {
				snprintf(path_sk, sizeof(path_sk),
					 "%s/kem_sk.bin", ctx->config_dir);
				save_file(path_pk, pk, kem->length_public_key);
				save_sk_encrypted(path_sk, ctx->passphrase, sk,
						  kem->length_secret_key);
			} else
				ret = -1;
			free(pk);
			free(sk);
			OQS_KEM_free(kem);
		} else
			ret = -1;
	}

	snprintf(path_pk, sizeof(path_pk), "%s/sig_pk.bin", ctx->config_dir);
	if (stat(path_pk, &st) == -1) {
		OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
		if (sig) {
			u8 *pk = malloc(sig->length_public_key);
			u8 *sk = malloc(sig->length_secret_key);
			if (OQS_SIG_keypair(sig, pk, sk) == OQS_SUCCESS) {
				snprintf(path_sk, sizeof(path_sk),
					 "%s/sig_sk.bin", ctx->config_dir);
				save_file(path_pk, pk, sig->length_public_key);
				save_sk_encrypted(path_sk, ctx->passphrase, sk,
						  sig->length_secret_key);
			} else
				ret = -1;
			free(pk);
			free(sk);
			OQS_SIG_free(sig);
		} else
			ret = -1;
	}
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

shyake_err shyake_register(shyake_ctx *ctx, const char *username)
{
	if (!ctx || !username)
		return SHYAKE_ERR;
	clear_error(ctx);

	usize kpk_len, spk_len, ssk_len;
	char path[512];

	snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
	u8 *kpk = load_file(path, &kpk_len);
	snprintf(path, sizeof(path), "%s/sig_pk.bin", ctx->config_dir);
	u8 *spk = load_file(path, &spk_len);
	snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
	u8 *ssk = load_sk_decrypted(ctx, path, &ssk_len);

	if (!kpk || !spk || !ssk) {
		free(kpk);
		free(spk);
		free(ssk);
		return SHYAKE_ERR_CRYPTO;
	}

	char *kpk_b64 = base64_encode(kpk, kpk_len);
	char *spk_b64 = base64_encode(spk, spk_len);

	time_t now = time(NULL);
	char timestamp[32];
	snprintf(timestamp, sizeof(timestamp), "%ld", now);

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

	char *pow = shyake_mint_pow(username, 20);

	cJSON_AddStringToObject(root, "signature", sig_b64);
	cJSON_AddStringToObject(root, "pow", pow);
	char *payload = cJSON_PrintUnformatted(root);

	shyake_err ret = SHYAKE_OK;
	if (ctx->instance_url) {
		CURL *curl = curl_easy_init();
		if (curl) {
			if (ctx->debug)
				curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

			char url[512];
			snprintf(url, sizeof(url), "%s/api/register",
				 ctx->instance_url);

			struct curl_slist *headers = NULL;
			headers = curl_slist_append(
				headers, "Content-Type: application/json");

			struct curl_response resp = { .data = malloc(1),
						      .size = 0 };
			resp.data[0] = '\0';

			curl_easy_setopt(curl, CURLOPT_URL, url);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
					 curl_write_cb);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA,
					 (void *)&resp);

			CURLcode res = curl_easy_perform(curl);
			if (res != CURLE_OK) {
				set_network_error(ctx, res);
				ret = SHYAKE_ERR_NETWORK;
			} else {
				long http_code = 0;
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
						  &http_code);
				if (http_code == 200 || http_code == 201) {
					ret = SHYAKE_OK;
				} else {
					set_server_error(ctx, http_code,
							 resp.data);
					ret = SHYAKE_ERR_HTTP;
				}
			}

			free(resp.data);
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		}
	} else {
		ret = SHYAKE_ERR_NO_INSTANCE;
	}

	free(json_raw);
	free(payload);
	free(signature);
	free(sig_b64);
	free(pow);
	free(kpk_b64);
	free(spk_b64);
	free(kpk);
	free(spk);
	free(ssk);
	cJSON_Delete(root);
	OQS_SIG_free(sig);
	return ret;
}
