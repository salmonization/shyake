#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <oqs/oqs.h>
#include "vendor/cJSON/cJSON.h"
#include "lib_internal.h"

shyake_err shyake_block(shyake_ctx *ctx, const char *target, int unblock)
{
	if (!ctx || !target)
		return SHYAKE_ERR;
	clear_error(ctx);

	cJSON *body_json = cJSON_CreateObject();
	cJSON_AddStringToObject(body_json, "target", target);
	char *body = cJSON_PrintUnformatted(body_json);
	cJSON_Delete(body_json);

	struct http_reply r;
	shyake_err ret = http_send_signed(ctx, unblock ? "DELETE" : "POST",
					  "/api/block", body, &r);
	free(body);
	if (ret != SHYAKE_OK)
		return ret;
	if (r.code != 200 && r.code != 201) {
		set_signed_body_error(ctx, r.code, r.body);
		ret = SHYAKE_ERR_HTTP;
	}
	http_reply_free(&r);
	return ret;
}

void shyake_free_block_list(shyake_block_list *list)
{
	if (!list)
		return;
	for (int i = 0; i < list->count; i++)
		free(list->entries[i].target);
	free(list->entries);
	free(list);
}

/* block list from the answer to GET /api/block */
static shyake_block_list *parse_blocks(const char *body)
{
	cJSON *json = cJSON_Parse(body);
	cJSON *arr = json ? cJSON_GetObjectItem(json, "blocks") : NULL;
	if (!cJSON_IsArray(arr)) {
		cJSON_Delete(json);
		return NULL;
	}

	int count = cJSON_GetArraySize(arr);
	shyake_block_list *list = calloc(1, sizeof(shyake_block_list));
	list->count = count;
	list->entries = calloc(count, sizeof(shyake_block_entry));
	for (int i = 0; i < count; i++) {
		cJSON *item = cJSON_GetArrayItem(arr, i);
		cJSON *tgt = cJSON_GetObjectItem(item, "blocked");
		cJSON *ts = cJSON_GetObjectItem(item, "created_at");
		shyake_block_entry *e = &list->entries[i];
		e->target = strdup(cJSON_IsString(tgt) ? tgt->valuestring : "");
		e->created = cJSON_IsNumber(ts) ? (i64)ts->valuedouble : 0;
	}
	cJSON_Delete(json);
	return list;
}

shyake_block_list *shyake_list_blocks(shyake_ctx *ctx)
{
	if (!ctx)
		return NULL;
	clear_error(ctx);

	struct http_reply r;
	if (http_send_signed(ctx, "GET", "/api/block", NULL, &r) != SHYAKE_OK)
		return NULL;
	shyake_block_list *list = NULL;
	if (r.code == 200)
		list = parse_blocks(r.body);
	else
		set_server_error(ctx, r.code, r.body);
	http_reply_free(&r);
	return list;
}

/* fresh key pairs for rotate */
struct key_pairs {
	OQS_KEM *kem;
	OQS_SIG *sig;
	u8 *kpk, *ksk, *spk, *ssk;
};

static void free_key_pairs(struct key_pairs *k)
{
	free(k->kpk);
	free(k->spk);
	if (k->kem)
		free_secret(k->ksk, k->kem->length_secret_key);
	if (k->sig)
		free_secret(k->ssk, k->sig->length_secret_key);
	OQS_KEM_free(k->kem);
	OQS_SIG_free(k->sig);
}

static int make_key_pairs(struct key_pairs *k)
{
	k->kem = OQS_KEM_new("ML-KEM-768");
	k->sig = OQS_SIG_new("ML-DSA-65");
	if (!k->kem || !k->sig)
		return -1;
	k->kpk = malloc(k->kem->length_public_key);
	k->ksk = malloc(k->kem->length_secret_key);
	k->spk = malloc(k->sig->length_public_key);
	k->ssk = malloc(k->sig->length_secret_key);
	if (OQS_KEM_keypair(k->kem, k->kpk, k->ksk) != OQS_SUCCESS ||
	    OQS_SIG_keypair(k->sig, k->spk, k->ssk) != OQS_SUCCESS)
		return -1;
	return 0;
}

/* POST /api/rotate body: the new public keys */
static char *rotate_body(const struct key_pairs *k)
{
	char *kpk_b64 = base64_encode(k->kpk, k->kem->length_public_key);
	char *spk_b64 = base64_encode(k->spk, k->sig->length_public_key);
	cJSON *json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "new_kem_pubkey", kpk_b64);
	cJSON_AddStringToObject(json, "new_sig_pubkey", spk_b64);
	char *body = cJSON_PrintUnformatted(json);
	cJSON_Delete(json);
	free(kpk_b64);
	free(spk_b64);
	return body;
}

/* replace the key files, secret keys under the new passphrase */
static void save_key_pairs(shyake_ctx *ctx, const struct key_pairs *k)
{
	const char *pp = ctx->new_passphrase;
	char path[512];
	snprintf(path, sizeof(path), "%s/kem_pk.bin", ctx->config_dir);
	save_file(path, k->kpk, k->kem->length_public_key);
	snprintf(path, sizeof(path), "%s/kem_sk.bin", ctx->config_dir);
	save_sk_encrypted(path, pp, k->ksk, k->kem->length_secret_key);
	snprintf(path, sizeof(path), "%s/sig_pk.bin", ctx->config_dir);
	save_file(path, k->spk, k->sig->length_public_key);
	snprintf(path, sizeof(path), "%s/sig_sk.bin", ctx->config_dir);
	save_sk_encrypted(path, pp, k->ssk, k->sig->length_secret_key);
}

shyake_err shyake_rotate(shyake_ctx *ctx)
{
	if (!ctx)
		return SHYAKE_ERR;
	clear_error(ctx);

	struct key_pairs k = { 0 };
	if (make_key_pairs(&k) != 0) {
		free_key_pairs(&k);
		return SHYAKE_ERR_CRYPTO;
	}

	/* signed with the OLD private key currently on disk; the digest
	 * binds the new public keys to that signature */
	char *body = rotate_body(&k);
	struct http_reply r;
	shyake_err ret = http_send_signed(ctx, "POST", "/api/rotate", body, &r);
	free(body);
	if (ret == SHYAKE_OK) {
		/* save new keys locally only after server confirmed */
		if (r.code == 200 || r.code == 201) {
			save_key_pairs(ctx, &k);
		} else {
			set_signed_body_error(ctx, r.code, r.body);
			ret = SHYAKE_ERR_HTTP;
		}
		http_reply_free(&r);
	}
	free_key_pairs(&k);
	return ret;
}

shyake_err shyake_destroy(shyake_ctx *ctx)
{
	if (!ctx)
		return SHYAKE_ERR;
	clear_error(ctx);

	struct http_reply r;
	shyake_err ret =
		http_send_signed(ctx, "DELETE", "/api/destroy", NULL, &r);
	if (ret != SHYAKE_OK)
		return ret;
	if (r.code != 200) {
		set_server_error(ctx, r.code, r.body);
		ret = SHYAKE_ERR_HTTP;
	}
	http_reply_free(&r);
	return ret;
}
