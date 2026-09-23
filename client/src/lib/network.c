#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <oqs/oqs.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include "vendor/cJSON/cJSON.h"
#include "lib_internal.h"

usize curl_write_cb(void *contents, usize size, usize nmemb, void *userp)
{
	usize realsize = size * nmemb;
	struct curl_response *mem = (struct curl_response *)userp;
	char *ptr = realloc(mem->data, mem->size + realsize + 1);
	if (!ptr)
		return 0;
	mem->data = ptr;
	memcpy(&(mem->data[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->data[mem->size] = 0;
	return realsize;
}

/*
 * Signed string of a header-authenticated request:
 *
 *   METHOD:endpoint:username:timestamp               no request body
 *   METHOD:endpoint:username:timestamp:sha256hex     with a body
 *
 * The digest binds the exact body bytes (protocol 2, SPEC 3.3).
 */
struct curl_slist *create_signed_headers_body(shyake_ctx *ctx,
					      const char *method,
					      const char *endpoint,
					      const char *username,
					      const u8 *body, usize body_len)
{
	// sign request with given method and mint PoW
	time_t now = time(NULL);
	char timestamp[32];
	snprintf(timestamp, sizeof(timestamp), "%ld", now);

	char path[512];
	usize ssk_len;
	snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
	u8 *ssk = load_sk_decrypted(ctx, path, &ssk_len);
	if (!ssk)
		return NULL;

	char digest[2 * SHA256_DIGEST_LENGTH + 2] = "";
	if (body) {
		u8 md[SHA256_DIGEST_LENGTH];
		SHA256(body, body_len, md);
		digest[0] = ':';
		for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
			snprintf(digest + 1 + 2 * i, 3, "%02x", md[i]);
	}

	char message[640];
	snprintf(message, sizeof(message), "%s:%s:%s:%s%s", method, endpoint,
		 username, timestamp, digest);

	OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
	if (!sig) {
		free(ssk);
		return NULL;
	}
	u8 *signature = malloc(sig->length_signature);
	usize sig_len;
	OQS_SIG_sign(sig, signature, &sig_len, (u8 *)message, strlen(message),
		     ssk);
	char *sig_b64 = base64_encode(signature, sig_len);

	char *pow = shyake_mint_pow(username, 20);

	struct curl_slist *headers = NULL;
	char header_buf[8192];
	snprintf(header_buf, sizeof(header_buf), "X-Shyake-Username: %s",
		 username);
	headers = curl_slist_append(headers, header_buf);
	snprintf(header_buf, sizeof(header_buf), "X-Shyake-Timestamp: %s",
		 timestamp);
	headers = curl_slist_append(headers, header_buf);
	snprintf(header_buf, sizeof(header_buf), "X-Shyake-Signature: %s",
		 sig_b64);
	headers = curl_slist_append(headers, header_buf);
	snprintf(header_buf, sizeof(header_buf), "X-Shyake-Pow: %s", pow);
	headers = curl_slist_append(headers, header_buf);

	free(ssk);
	free(signature);
	free(sig_b64);
	free(pow);
	OQS_SIG_free(sig);
	return headers;
}

struct curl_slist *create_signed_headers(shyake_ctx *ctx, const char *method,
					 const char *endpoint,
					 const char *username)
{
	return create_signed_headers_body(ctx, method, endpoint, username, NULL,
					  0);
}

struct curl_slist *create_auth_headers(shyake_ctx *ctx, const char *endpoint,
				       const char *username)
{
	return create_signed_headers(ctx, "GET", endpoint, username);
}

char *fetch_recipient_pubkey(shyake_ctx *ctx, const char *recipient)
{
	// fetch public key for recipient from server
	char url[512];
	snprintf(url, sizeof(url), "%s/api/pubkey/%s", ctx->instance_url,
		 recipient);

	CURL *curl = curl_easy_init();
	if (!curl)
		return NULL;

	if (ctx->debug)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	struct curl_response resp = { .data = malloc(1), .size = 0 };
	resp.data[0] = '\0';

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);

	if (res == CURLE_OK && http_code == 200) {
		cJSON *json = cJSON_Parse(resp.data);
		free(resp.data);
		if (json) {
			cJSON *kem_pk_item =
				cJSON_GetObjectItem(json, "kem_pubkey");
			char *kem_pk = NULL;
			if (cJSON_IsString(kem_pk_item))
				kem_pk = strdup(kem_pk_item->valuestring);
			cJSON_Delete(json);
			return kem_pk;
		}
		return NULL;
	} else if (res != CURLE_OK) {
		set_error(ctx, "Network error: %s", curl_easy_strerror(res));
	}

	free(resp.data);
	return NULL;
}

/* whether a server reply carries exactly this error text */
int http_error_is(const char *body, const char *text)
{
	cJSON *json = cJSON_Parse(body);
	cJSON *e = json ? cJSON_GetObjectItem(json, "error") : NULL;
	int match = cJSON_IsString(e) && strcmp(e->valuestring, text) == 0;
	cJSON_Delete(json);
	return match;
}

/* record "<what> (HTTP <code>): <server's error text>" */
void set_http_error(shyake_ctx *ctx, const char *what, long code,
		    const char *body)
{
	char msg[256] = "";
	cJSON *json = cJSON_Parse(body);
	cJSON *e = json ? cJSON_GetObjectItem(json, "error") : NULL;
	if (cJSON_IsString(e)) {
		snprintf(msg, sizeof(msg), "%s", e->valuestring);
		for (char *p = msg; *p; p++)
			if ((u8)*p < 0x20 || *p == 0x7f)
				*p = '?';
	}
	cJSON_Delete(json);
	if (msg[0])
		set_error(ctx, "%s (HTTP %ld): %s", what, code, msg);
	else
		set_error(ctx, "%s (HTTP %ld).", what, code);
}

/* protocol level of the instance: 1 if it has no /api/version, 0 if
 * it cannot be reached */
int instance_protocol(shyake_ctx *ctx)
{
	char url[512];
	snprintf(url, sizeof(url), "%s/api/version", ctx->instance_url);
	CURL *curl = curl_easy_init();
	if (!curl)
		return 0;
	struct curl_response resp = { .data = malloc(1), .size = 0 };
	resp.data[0] = '\0';
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);

	int level = 0;
	if (curl_easy_perform(curl) == CURLE_OK) {
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		level = 1;
		cJSON *json = http_code == 200 ? cJSON_Parse(resp.data) : NULL;
		cJSON *p = json ? cJSON_GetObjectItem(json, "protocol") : NULL;
		if (cJSON_IsNumber(p) && p->valueint > 1)
			level = p->valueint;
		cJSON_Delete(json);
	}
	free(resp.data);
	curl_easy_cleanup(curl);
	return level;
}

/* failure detail of a body-signed request; a server older than
 * protocol 2 rejects the signature, so say that instead of 401 */
void set_signed_body_error(shyake_ctx *ctx, const char *what, long code,
			   const char *body)
{
	if (code == 401 && instance_protocol(ctx) == 1) {
		set_error(ctx,
			  "%s: this instance does not accept signed "
			  "request bodies. Its server must be v0.3.0 or "
			  "later.",
			  what);
		return;
	}
	set_http_error(ctx, what, code, body);
}
