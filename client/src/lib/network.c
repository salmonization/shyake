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

/*
 * Fetch the KEM public key of a recipient through our own instance.
 * On failure, returns NULL, records the reason, and sets *err:
 * NETWORK (no answer), NOT_FOUND (no such user), GONE (destroyed),
 * or HTTP (any other refusal).
 */
char *fetch_recipient_pubkey(shyake_ctx *ctx, const char *recipient,
			     shyake_err *err)
{
	char url[512];
	snprintf(url, sizeof(url), "%s/api/pubkey/%s", ctx->instance_url,
		 recipient);
	*err = SHYAKE_ERR;

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

	char *kem_pk = NULL;
	if (res != CURLE_OK) {
		set_network_error(ctx, res);
		*err = SHYAKE_ERR_NETWORK;
	} else if (http_code == 200) {
		cJSON *json = cJSON_Parse(resp.data);
		cJSON *item = json ? cJSON_GetObjectItem(json, "kem_pubkey") :
				     NULL;
		if (cJSON_IsString(item) && item->valuestring[0]) {
			kem_pk = strdup(item->valuestring);
			*err = SHYAKE_OK;
		} else if (cJSON_IsString(item)) {
			/* a destroyed account keeps its name, not its keys */
			set_error(ctx, "%s no longer exists.", recipient);
			*err = SHYAKE_ERR_GONE;
		} else {
			set_error(ctx, "The instance sent an invalid key.");
		}
		cJSON_Delete(json);
	} else if (http_code == 404) {
		set_error(ctx, "There is no user %s.", recipient);
		*err = SHYAKE_ERR_NOT_FOUND;
	} else if (http_code == 502) {
		const char *at = strchr(recipient, '@');
		set_error(ctx, "Cannot reach %s.", at ? at + 1 : recipient);
		*err = SHYAKE_ERR_HTTP;
	} else {
		set_server_error(ctx, http_code, resp.data);
		*err = SHYAKE_ERR_HTTP;
	}

	free(resp.data);
	return kem_pk;
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

/* server error texts and the reason the client gives for each */
static const struct {
	const char *text;
	const char *reason;
} server_reasons[] = {
	{ "Timestamp out of window",
	  "Your clock is off by more than 5 minutes." },
	{ "Invalid Proof of Work", "The instance rejected the proof of work." },
	{ "Invalid signature", "The instance rejected your signature." },
	{ "Signature verification failed",
	  "The instance rejected your signature." },
	{ "User not found", "Your account does not exist on this instance." },
	{ "User not found or destroyed",
	  "Your account does not exist on this instance." },
	{ "Sender not registered",
	  "Your account does not exist on this instance." },
	{ "Replayed request",
	  "The instance already saw this request. Try again." },
	{ "Registration is disabled",
	  "This instance does not accept new accounts." },
	{ "Username already taken", "The username is taken." },
	{ "Username is reserved", "The username is reserved." },
	{ "Invalid username format", "The username is not valid." },
	{ "Mail not found", "There is no such mail." },
	{ "Payload too large", "The mail is too large." },
	{ "Recipient instance unreachable",
	  "Cannot reach the recipient's instance." },
	{ "Federation disabled", "This instance does not federate." },
	{ "Invalid target", "The block target is not valid." },
	{ "Database error", "The instance had an internal error." },
};

/*
 * Record why the server refused a request, as a sentence that
 * completes "Error: <action> failed.". Known texts get a plain reason;
 * others pass through, made printable.
 */
void set_server_error(shyake_ctx *ctx, long code, const char *body)
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

	for (usize i = 0; i < sizeof(server_reasons) / sizeof(*server_reasons);
	     i++) {
		if (strcmp(msg, server_reasons[i].text) == 0) {
			set_error(ctx, "%s", server_reasons[i].reason);
			return;
		}
	}
	if (code == 429) {
		set_error(ctx, "Too many requests. Try again later.");
	} else if (msg[0]) {
		int stop = strchr(".!?", msg[strlen(msg) - 1]) != NULL;
		set_error(ctx, "%s%s", msg, stop ? "" : ".");
	} else if (code >= 500) {
		set_error(ctx, "The instance had an internal error.");
	} else {
		set_error(ctx, "The instance answered with HTTP %ld.", code);
	}
}

/* record that the instance did not answer */
void set_network_error(shyake_ctx *ctx, CURLcode res)
{
	const char *host = strstr(ctx->instance_url, "://");
	host = host ? host + 3 : ctx->instance_url;
	set_error(ctx, "Cannot reach %s (%s).", host, curl_easy_strerror(res));
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

/* failure reason of a body-signed request; a server older than
 * protocol 2 rejects the signature, so say that instead */
void set_signed_body_error(shyake_ctx *ctx, long code, const char *body)
{
	if (code == 401 && instance_protocol(ctx) == 1) {
		set_error(ctx, "The instance must run server v0.3.0 or later.");
		return;
	}
	set_server_error(ctx, code, body);
}
