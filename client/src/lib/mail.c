#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <oqs/oqs.h>
#include <openssl/sha.h>
#include "vendor/cJSON/cJSON.h"
#include "lib_internal.h"
#include "shyake_crypto.h"

/* ------------------------------------------------------------------ */

void shyake_free_mail_list(shyake_mail_list *list)
{
	if (!list)
		return;
	for (int i = 0; i < list->count; i++) {
		free(list->entries[i].mail_id);
		free(list->entries[i].party);
		free(list->entries[i].subject);
		free(list->entries[i].ref);
		free(list->entries[i].reply_ref);
	}
	free(list->entries);
	free(list);
}

void shyake_free_mail_detail(shyake_mail_detail *d)
{
	if (!d)
		return;
	free(d->mail_id);
	free(d->sender);
	free(d->recipient);
	free(d->subject);
	free(d->body);
	free(d->ref);
	free(d->reply_ref);
	free(d);
}

/* ------------------------------------------------------------------ */
/* Replies                                                            */
/* ------------------------------------------------------------------ */

#define REF_LEN (SHA256_DIGEST_LENGTH * 2)

/* a reference is 64 lowercase hex digits */
static int is_ref(const char *s)
{
	if (!s || strlen(s) != REF_LEN)
		return 0;
	for (const char *p = s; *p; p++)
		if (!strchr("0123456789abcdef", *p))
			return 0;
	return 1;
}

/*
 * A mail ID as the client accepts it: 1 to 64 letters, digits, '-'
 * or '_' (servers assign 10-character base58, SPEC 4). It goes into
 * URLs and file names, so nothing else gets through.
 */
static int is_mail_id(const char *s)
{
	usize n = strlen(s);
	if (n == 0 || n > 64)
		return 0;
	for (const char *p = s; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
			return 0;
	return 1;
}

/* checks a mail ID, recording why it cannot be one */
static int check_mail_id(shyake_ctx *ctx, const char *mail_id)
{
	if (is_mail_id(mail_id))
		return 1;
	set_error(ctx, "The mail ID is not valid.");
	return 0;
}

/* reference of a mail: hex SHA-256 of its signature string */
static char *sig_ref(const char *signature)
{
	if (!signature)
		return NULL;
	u8 sum[SHA256_DIGEST_LENGTH];
	SHA256((const u8 *)signature, strlen(signature), sum);
	char *hex = malloc(REF_LEN + 1);
	if (!hex)
		return NULL;
	hex_encode(sum, sizeof(sum), hex);
	return hex;
}

/* decrypt enc_reply_to; NULL when absent or not a reference */
static char *open_reply_ref(const u8 *sym, cJSON *json)
{
	cJSON *e = cJSON_GetObjectItem(json, "enc_reply_to");
	if (!cJSON_IsString(e) || !e->valuestring[0])
		return NULL;
	char *ref = decrypt_from_b64(sym, e->valuestring);
	if (ref && !is_ref(ref)) {
		free(ref);
		ref = NULL;
	}
	return ref;
}

/* reference of a mail from its full JSON */
static char *json_ref(cJSON *json)
{
	cJSON *sig = cJSON_GetObjectItem(json, "signature");
	return cJSON_IsString(sig) ? sig_ref(sig->valuestring) : NULL;
}

const char *shyake_reply_recipient(shyake_ctx *ctx, const shyake_mail_detail *d)
{
	if (!ctx || !d || !d->sender || !d->recipient)
		return NULL;
	return strcmp(d->sender, ctx->username) == 0 ? d->recipient : d->sender;
}

/* ------------------------------------------------------------------ */
/* Opening a mail                                                     */
/* ------------------------------------------------------------------ */

/* the account's KEM secret key, decrypted */
static u8 *load_kem_sk(shyake_ctx *ctx, usize *len)
{
	char path[512];
	*len = 0;
	config_file(ctx->config_dir, KEM_SK_FILE, path, sizeof(path));
	return load_sk_decrypted(ctx, path, len);
}

/*
 * One mail's JSON, from the instance or a saved file, checked once
 * here: every field below is set unless marked otherwise. Strings
 * point into json.
 */
struct wire_mail {
	cJSON *json;
	const char *mail_id; /* list rows only */
	const char *sender;
	const char *recipient;
	const char *enc_key_sender;
	const char *enc_key_recipient;
	const char *enc_subject;
	const char *enc_body; /* full mails only */
	i64 timestamp;
	int size;
};

static const char *str_field(cJSON *json, const char *name)
{
	cJSON *v = cJSON_GetObjectItem(json, name);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* -1 when a field every mail carries is missing or mistyped */
static int parse_wire_mail(cJSON *json, struct wire_mail *m)
{
	cJSON *ts = cJSON_GetObjectItem(json, "timestamp");
	cJSON *sz = cJSON_GetObjectItem(json, "size");
	m->json = json;
	m->mail_id = str_field(json, "mail_id");
	m->sender = str_field(json, "sender");
	m->recipient = str_field(json, "recipient");
	m->enc_key_sender = str_field(json, "enc_key_sender");
	m->enc_key_recipient = str_field(json, "enc_key_recipient");
	m->enc_subject = str_field(json, "enc_subject");
	m->enc_body = str_field(json, "enc_body");
	if (!m->sender || !m->recipient || !m->enc_key_sender ||
	    !m->enc_key_recipient || !m->enc_subject || !cJSON_IsNumber(ts))
		return -1;
	m->timestamp = (i64)ts->valuedouble;
	m->size = cJSON_IsNumber(sz) ? sz->valueint : 0;
	return 0;
}

/* the wrapped mail key meant for us: as its sender or recipient */
static const char *our_key(shyake_ctx *ctx, const struct wire_mail *m)
{
	return strcmp(ctx->username, m->sender) == 0 ? m->enc_key_sender :
						       m->enc_key_recipient;
}

/* decrypted parts of a mail; each NULL when it cannot be read */
struct mail_text {
	char *subject;
	char *body;
	char *reply_ref;
};

/*
 * Unwrap the mail key enc_key with ksk, then decrypt the subject,
 * the body when with_body, and the reply reference.
 */
static struct mail_text open_mail(const u8 *ksk, const struct wire_mail *m,
				  const char *enc_key, int with_body)
{
	struct mail_text t = { 0 };
	if (!ksk)
		return t;
	u8 *sym = kem_decapsulate_key(enc_key, ksk);
	if (!sym)
		return t;
	t.subject = decrypt_from_b64(sym, m->enc_subject);
	if (with_body && m->enc_body)
		t.body = decrypt_from_b64(sym, m->enc_body);
	t.reply_ref = open_reply_ref(sym, m->json);
	free_secret(sym, CHACHA20_KEY_SIZE);
	return t;
}

/* a mail detail from the full JSON of one mail */
static shyake_mail_detail *detail_from_wire(shyake_ctx *ctx, const u8 *ksk,
					    const char *mail_id,
					    const struct wire_mail *m,
					    int with_body)
{
	struct mail_text t = open_mail(ksk, m, our_key(ctx, m), with_body);
	shyake_mail_detail *d = calloc(1, sizeof(shyake_mail_detail));
	d->mail_id = strdup(mail_id);
	d->sender = strdup(m->sender);
	d->recipient = strdup(m->recipient);
	d->subject = t.subject;
	d->body = t.body;
	d->timestamp = m->timestamp;
	d->size = m->size;
	d->ref = json_ref(m->json);
	d->reply_ref = t.reply_ref;
	return d;
}

/* ------------------------------------------------------------------ */
/* Sending                                                            */
/* ------------------------------------------------------------------ */

/*
 * The recipient's KEM public key, checked against known_hosts: a
 * new name is trusted on first use, a changed key is refused.
 * fp_hex receives the key's fingerprint.
 */
static shyake_err recipient_key(shyake_ctx *ctx, const char *recipient, u8 **pk,
				usize *pk_len, char *fp_hex)
{
	shyake_err lookup;
	char *pk_b64 = fetch_recipient_pubkey(ctx, recipient, &lookup);
	if (!pk_b64)
		return lookup;
	*pk = base64_decode(pk_b64, pk_len);
	if (!*pk) {
		free(pk_b64);
		return SHYAKE_ERR_CRYPTO;
	}
	u8 fp[SHA256_DIGEST_LENGTH];
	SHA256(*pk, *pk_len, fp);
	hex_encode(fp, sizeof(fp), fp_hex);

	char *known_b64 = get_known_host(ctx->config_dir, recipient);
	if (!known_b64)
		add_known_host(ctx->config_dir, recipient, fp_hex, pk_b64);
	free(pk_b64);
	if (!known_b64)
		return SHYAKE_OK;

	usize known_len;
	u8 *known = base64_decode(known_b64, &known_len);
	free(known_b64);
	if (!known)
		return SHYAKE_OK;
	u8 known_fp[SHA256_DIGEST_LENGTH];
	SHA256(known, known_len, known_fp);
	free(known);
	if (memcmp(known_fp, fp, sizeof(fp)) == 0)
		return SHYAKE_OK;

	set_error(ctx, "The public key of %s has changed.", recipient);
	free(*pk);
	*pk = NULL;
	return SHYAKE_ERR_KEY_MISMATCH;
}

/* sender as the recipient sees it: user@domain across instances */
static void sender_name(shyake_ctx *ctx, const char *recipient, char *out,
			usize out_len)
{
	if (!strchr(recipient, '@')) {
		snprintf(out, out_len, "%s", ctx->username);
		return;
	}
	const char *d = strstr(ctx->instance_url, "://");
	d = d ? d + 3 : ctx->instance_url;
	const char *sl = strchr(d, '/');
	int ln = sl ? (int)(sl - d) : (int)strlen(d);
	if (ln > 127)
		ln = 127;
	snprintf(out, out_len, "%s@%.*s", ctx->username, ln, d);
}

/* what goes into a mail, encrypted and signed */
struct outgoing {
	const char *recipient;
	const char *reply_ref;
	const char *subject;
	const u8 *body;
	usize body_len;
	const char *fp_hex; /* recipient key fingerprint */
};

/*
 * The POST /api/mail payload: the signed fields (SPEC 5), the mail
 * key wrapped for both parties, the signature and a proof of work.
 */
static char *mail_payload(shyake_ctx *ctx, const struct outgoing *m,
			  const u8 *recip_pk, usize recip_pk_len,
			  const u8 *my_kpk, usize my_kpk_len, const u8 *my_ssk)
{
	u8 sym_key[CHACHA20_KEY_SIZE];
	if (!fill_random(sym_key, sizeof(sym_key)))
		return NULL;

	char *enc_subject = encrypt_to_b64(sym_key, (const u8 *)m->subject,
					   m->subject ? strlen(m->subject) : 0);
	char *enc_body = encrypt_to_b64(sym_key, m->body, m->body_len);
	char *enc_reply_to =
		m->reply_ref ? encrypt_to_b64(sym_key, (const u8 *)m->reply_ref,
					      REF_LEN) :
			       NULL;
	char *enc_key_recipient =
		kem_encapsulate_key(recip_pk, recip_pk_len, sym_key);
	char *enc_key_sender = kem_encapsulate_key(my_kpk, my_kpk_len, sym_key);

	char *payload = NULL;
	if (!enc_key_recipient || !enc_key_sender ||
	    (m->reply_ref && !enc_reply_to))
		goto out;

	char sender[256];
	sender_name(ctx, m->recipient, sender, sizeof(sender));
	char timestamp[32];
	snprintf(timestamp, sizeof(timestamp), "%ld", time(NULL));

	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "sender", sender);
	cJSON_AddStringToObject(obj, "recipient", m->recipient);
	cJSON_AddStringToObject(obj, "recipient_kem_fingerprint", m->fp_hex);
	cJSON_AddStringToObject(obj, "enc_subject",
				enc_subject ? enc_subject : "");
	cJSON_AddStringToObject(obj, "enc_body", enc_body);
	if (enc_reply_to)
		cJSON_AddStringToObject(obj, "enc_reply_to", enc_reply_to);
	cJSON_AddStringToObject(obj, "timestamp", timestamp);
	cJSON_AddNumberToObject(obj, "size", m->body_len);

	char *signed_raw = cJSON_PrintUnformatted(obj);
	OQS_SIG *sig = OQS_SIG_new("ML-DSA-65");
	u8 *signature = malloc(sig->length_signature);
	usize sig_len;
	OQS_SIG_sign(sig, signature, &sig_len, (u8 *)signed_raw,
		     strlen(signed_raw), my_ssk);
	char *sig_b64 = base64_encode(signature, sig_len);
	char *pow = mint_pow(sender);

	cJSON_AddStringToObject(obj, "enc_key_sender", enc_key_sender);
	cJSON_AddStringToObject(obj, "enc_key_recipient", enc_key_recipient);
	cJSON_AddStringToObject(obj, "signature", sig_b64);
	cJSON_AddStringToObject(obj, "pow", pow);
	payload = cJSON_PrintUnformatted(obj);

	free(signed_raw);
	free(signature);
	free(sig_b64);
	free(pow);
	OQS_SIG_free(sig);
	cJSON_Delete(obj);
out:
	zero_memory(sym_key, sizeof(sym_key));
	free(enc_subject);
	free(enc_body);
	free(enc_reply_to);
	free(enc_key_recipient);
	free(enc_key_sender);
	return payload;
}

/* why the instance refused a mail */
static shyake_err send_refused(shyake_ctx *ctx, const struct outgoing *m,
			       long code, const char *body)
{
	const char *at = strchr(m->recipient, '@');
	if (code == 409) {
		set_error(ctx, "The public key of %s has changed.",
			  m->recipient);
		return SHYAKE_ERR_KEY_MISMATCH;
	}
	if (code == 410) {
		set_error(ctx, "%s no longer exists.", m->recipient);
		return SHYAKE_ERR_GONE;
	}
	if (code == 403 &&
	    http_error_is(body, "Recipient has blocked this sender")) {
		set_error(ctx, "You are blocked by %s.", m->recipient);
		return SHYAKE_ERR_BLOCKED;
	}
	if (code == 502 && at)
		set_error(ctx, "Cannot reach %s.", at + 1);
	else if (code == 422 && at &&
		 http_error_is(body,
			       "Recipient instance does not support replies"))
		set_error(ctx,
			  "%s does not accept replies yet. "
			  "It must run server v0.4.0 or later.",
			  at + 1);
	else if (m->reply_ref && code == 401 && instance_protocol(ctx) < 3)
		set_error(ctx, "Your instance must run server "
			       "v0.4.0 or later to send replies.");
	else
		set_server_error(ctx, code, body);
	return SHYAKE_ERR_HTTP;
}

static shyake_err send_mail(shyake_ctx *ctx, const char *recipient,
			    const char *reply_ref, const char *subject,
			    const u8 *body, usize body_len)
{
	if (!ctx || !recipient || !body)
		return SHYAKE_ERR;
	clear_error(ctx);
	if (reply_ref && !is_ref(reply_ref)) {
		set_error(ctx, "The reply reference is not valid.");
		return SHYAKE_ERR;
	}

	char fp_hex[REF_LEN + 1];
	u8 *recip_pk = NULL;
	usize recip_pk_len;
	shyake_err ret =
		recipient_key(ctx, recipient, &recip_pk, &recip_pk_len, fp_hex);
	if (ret != SHYAKE_OK)
		return ret;

	usize my_kpk_len, my_ssk_len = 0;
	char path[512];
	config_file(ctx->config_dir, KEM_PK_FILE, path, sizeof(path));
	u8 *my_kpk = load_file(path, &my_kpk_len);
	config_file(ctx->config_dir, SIG_SK_FILE, path, sizeof(path));
	u8 *my_ssk = load_sk_decrypted(ctx, path, &my_ssk_len);

	struct outgoing m = { recipient, reply_ref, subject,
			      body,	 body_len,  fp_hex };
	char *payload = my_kpk && my_ssk ?
				mail_payload(ctx, &m, recip_pk, recip_pk_len,
					     my_kpk, my_kpk_len, my_ssk) :
				NULL;
	free(recip_pk);
	free(my_kpk);
	free_secret(my_ssk, my_ssk_len);
	if (!payload)
		return SHYAKE_ERR_CRYPTO;

	struct http_reply r;
	ret = http_send(ctx, "POST", "/api/mail", NULL, payload, &r);
	free(payload);
	if (ret != SHYAKE_OK)
		return ret;
	if (r.code != 200 && r.code != 201)
		ret = send_refused(ctx, &m, r.code, r.body);
	http_reply_free(&r);
	return ret;
}

shyake_err shyake_send(shyake_ctx *ctx, const char *recipient,
		       const char *subject, const u8 *body, usize body_len)
{
	return send_mail(ctx, recipient, NULL, subject, body, body_len);
}

shyake_err shyake_send_reply(shyake_ctx *ctx, const char *recipient,
			     const char *reply_ref, const char *subject,
			     const u8 *body, usize body_len)
{
	if (!reply_ref)
		return SHYAKE_ERR;
	return send_mail(ctx, recipient, reply_ref, subject, body, body_len);
}

/* ------------------------------------------------------------------ */
/* Mail on the instance                                               */
/* ------------------------------------------------------------------ */

/* one row of GET /api/mail; -1 when the row is malformed */
static int fill_entry(shyake_mail_entry *e, cJSON *item, const u8 *ksk,
		      int is_sent)
{
	struct wire_mail m;
	if (parse_wire_mail(item, &m) != 0 || !m.mail_id)
		return -1;
	struct mail_text t = open_mail(
		ksk, &m, is_sent ? m.enc_key_sender : m.enc_key_recipient, 0);
	e->mail_id = strdup(m.mail_id);
	e->party = strdup(is_sent ? m.recipient : m.sender);
	e->subject = t.subject ? t.subject : strdup("(decryption failed)");
	e->reply_ref = t.reply_ref;
	e->size = m.size;
	e->timestamp = m.timestamp;
	e->is_sent = is_sent;
	const char *sh = str_field(item, "sig_hash");
	if (sh && is_ref(sh))
		e->ref = strdup(sh);
	return 0;
}

/* mailbox list from the answer to GET /api/mail */
static shyake_mail_list *parse_mail_list(shyake_ctx *ctx, const char *body,
					 int is_sent)
{
	cJSON *json = cJSON_Parse(body);
	cJSON *arr = json ? cJSON_GetObjectItem(json, "mail") : NULL;
	if (!cJSON_IsArray(arr)) {
		cJSON_Delete(json);
		return NULL;
	}

	int count = cJSON_GetArraySize(arr);
	shyake_mail_list *list = calloc(1, sizeof(shyake_mail_list));
	list->entries = calloc(count, sizeof(shyake_mail_entry));
	usize ksk_len;
	u8 *ksk = load_kem_sk(ctx, &ksk_len);
	/* a malformed row is left out rather than shown half-read */
	int n = 0;
	for (int i = 0; i < count; i++)
		if (fill_entry(&list->entries[n], cJSON_GetArrayItem(arr, i),
			       ksk, is_sent) == 0)
			n++;
	list->count = n;
	free_secret(ksk, ksk_len);
	cJSON_Delete(json);
	return list;
}

shyake_mail_list *shyake_check(shyake_ctx *ctx, const char *type)
{
	if (!ctx || !type)
		return NULL;
	clear_error(ctx);
	if (strcmp(type, "inbox") != 0 && strcmp(type, "sent") != 0) {
		set_error(ctx, "There is no mailbox %s.", type);
		return NULL;
	}

	char endpoint[128];
	snprintf(endpoint, sizeof(endpoint), "/api/mail?type=%s", type);
	struct http_reply r;
	if (http_send_signed(ctx, "GET", endpoint, NULL, &r) != SHYAKE_OK)
		return NULL;
	shyake_mail_list *list = NULL;
	if (r.code == 200)
		list = parse_mail_list(ctx, r.body, strcmp(type, "sent") == 0);
	else
		set_server_error(ctx, r.code, r.body);
	http_reply_free(&r);
	return list;
}

/* one mail from the instance, its body decrypted when with_body */
static shyake_mail_detail *get_mail(shyake_ctx *ctx, const char *mail_id,
				    int with_body)
{
	if (!ctx || !mail_id)
		return NULL;
	clear_error(ctx);
	if (!check_mail_id(ctx, mail_id))
		return NULL;

	char endpoint[128];
	snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
	struct http_reply r;
	if (http_send_signed(ctx, "GET", endpoint, NULL, &r) != SHYAKE_OK)
		return NULL;
	shyake_mail_detail *d = NULL;
	cJSON *json = NULL;
	if (r.code != 200)
		set_server_error(ctx, r.code, r.body);
	else
		json = cJSON_Parse(r.body);
	struct wire_mail m;
	if (json && parse_wire_mail(json, &m) == 0) {
		usize ksk_len;
		u8 *ksk = load_kem_sk(ctx, &ksk_len);
		d = detail_from_wire(ctx, ksk, mail_id, &m, with_body);
		free_secret(ksk, ksk_len);
	} else if (r.code == 200) {
		set_error(ctx, "The instance sent a malformed mail.");
	}
	cJSON_Delete(json);
	http_reply_free(&r);
	return d;
}

shyake_mail_detail *shyake_fetch(shyake_ctx *ctx, const char *mail_id)
{
	shyake_mail_detail *d = get_mail(ctx, mail_id, 1);
	/* fetch has never reported a size */
	if (d)
		d->size = 0;
	return d;
}

shyake_mail_detail *shyake_check_one(shyake_ctx *ctx, const char *mail_id)
{
	return get_mail(ctx, mail_id, 0);
}

shyake_err shyake_burn(shyake_ctx *ctx, const char *mail_id)
{
	if (!ctx || !mail_id)
		return SHYAKE_ERR;
	clear_error(ctx);
	if (!check_mail_id(ctx, mail_id))
		return SHYAKE_ERR;

	char endpoint[128];
	snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
	struct http_reply r;
	shyake_err ret = http_send_signed(ctx, "DELETE", endpoint, NULL, &r);
	if (ret != SHYAKE_OK)
		return ret;
	if (r.code != 200) {
		set_server_error(ctx, r.code, r.body);
		ret = r.code == 404 ? SHYAKE_ERR_NOT_FOUND :
		      r.code == 403 ? SHYAKE_ERR_FORBIDDEN :
				      SHYAKE_ERR_HTTP;
	}
	http_reply_free(&r);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Local saved mail                                                   */
/* ------------------------------------------------------------------ */

/* ensure saved/ directory exists */
static int ensure_saved_dir(const char *config_dir)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/saved", config_dir);
	struct stat st = { 0 };
	if (stat(path, &st) == -1) {
		if (mkdir(path, 0700) == -1)
			return -1;
	}
	return 0;
}

/* length of the mail ID in a saved file name, 0 if not <id>.json */
static usize saved_id_len(const char *name)
{
	usize nl = strlen(name);
	if (nl <= 5 || strcmp(name + nl - 5, ".json") != 0)
		return 0;
	return nl - 5;
}

/* a saved file, parsed; NULL when it cannot be read */
static cJSON *load_json(const char *path)
{
	usize len;
	u8 *raw = load_file(path, &len);
	if (!raw)
		return NULL;
	cJSON *json = cJSON_ParseWithLength((const char *)raw, len);
	free(raw);
	return json;
}

void shyake_free_saved_list(shyake_saved_list *list)
{
	if (!list)
		return;
	for (int i = 0; i < list->count; i++) {
		free(list->entries[i].mail_id);
		free(list->entries[i].sender);
		free(list->entries[i].recipient);
		free(list->entries[i].subject);
		free(list->entries[i].ref);
		free(list->entries[i].reply_ref);
	}
	free(list->entries);
	free(list);
}

/* fetch raw JSON for a mail_id from server, save to saved/<id>.json */
shyake_err shyake_save_mail(shyake_ctx *ctx, const char *mail_id)
{
	if (!ctx || !mail_id)
		return SHYAKE_ERR;
	clear_error(ctx);
	if (!check_mail_id(ctx, mail_id))
		return SHYAKE_ERR;

	if (ensure_saved_dir(ctx->config_dir) != 0) {
		set_error(ctx, "Cannot create the saved directory.");
		return SHYAKE_ERR;
	}

	char endpoint[128];
	snprintf(endpoint, sizeof(endpoint), "/api/mail/%s", mail_id);
	struct http_reply r;
	shyake_err ret = http_send_signed(ctx, "GET", endpoint, NULL, &r);
	if (ret != SHYAKE_OK)
		return ret;
	if (r.code != 200) {
		set_server_error(ctx, r.code, r.body);
		ret = r.code == 404 ? SHYAKE_ERR_NOT_FOUND : SHYAKE_ERR_HTTP;
		http_reply_free(&r);
		return ret;
	}

	/* write raw JSON to saved/<id>.json */
	char save_path[640];
	snprintf(save_path, sizeof(save_path), "%s/saved/%s.json",
		 ctx->config_dir, mail_id);
	FILE *f = fopen(save_path, "w");
	if (f) {
		fputs(r.body, f);
		fclose(f);
	} else {
		set_error(ctx, "Cannot write %s.", save_path);
		ret = SHYAKE_ERR;
	}
	http_reply_free(&r);
	return ret;
}

/* a saved mail, its body decrypted when with_body */
static shyake_mail_detail *read_saved(shyake_ctx *ctx, const char *mail_id,
				      int with_body)
{
	if (!ctx || !mail_id)
		return NULL;
	clear_error(ctx);
	if (!check_mail_id(ctx, mail_id))
		return NULL;

	char path[640];
	snprintf(path, sizeof(path), "%s/saved/%s.json", ctx->config_dir,
		 mail_id);
	if (access(path, R_OK) != 0) {
		set_error(ctx, "Saved mail %s not found.", mail_id);
		return NULL;
	}
	cJSON *json = load_json(path);
	struct wire_mail m;
	if (!json || parse_wire_mail(json, &m) != 0) {
		set_error(ctx, "Saved mail %s is damaged.", mail_id);
		cJSON_Delete(json);
		return NULL;
	}
	usize ksk_len;
	u8 *ksk = load_kem_sk(ctx, &ksk_len);
	shyake_mail_detail *d =
		detail_from_wire(ctx, ksk, mail_id, &m, with_body);
	free_secret(ksk, ksk_len);
	cJSON_Delete(json);
	return d;
}

shyake_mail_detail *shyake_read_saved(shyake_ctx *ctx, const char *mail_id)
{
	return read_saved(ctx, mail_id, 1);
}

shyake_mail_detail *shyake_check_saved_one(shyake_ctx *ctx, const char *mail_id)
{
	return read_saved(ctx, mail_id, 0);
}

/* one row of the saved list, from saved/<name> */
static int fill_saved_entry(shyake_ctx *ctx, shyake_saved_entry *e,
			    const char *dir, const char *name, usize id_len,
			    const u8 *ksk)
{
	char path[768];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	cJSON *json = load_json(path);
	struct wire_mail m;
	if (!json || parse_wire_mail(json, &m) != 0) {
		cJSON_Delete(json);
		return -1;
	}

	struct mail_text t = open_mail(ksk, &m, our_key(ctx, &m), 0);
	e->ref = json_ref(json);
	e->reply_ref = t.reply_ref;
	e->mail_id = strndup(name, id_len < 255 ? id_len : 255);
	e->sender = strdup(m.sender);
	e->recipient = strdup(m.recipient);
	e->subject = t.subject ? t.subject : strdup("(decryption failed)");
	e->timestamp = m.timestamp;
	e->size = m.size;
	cJSON_Delete(json);
	return 0;
}

shyake_saved_list *shyake_list_saved(shyake_ctx *ctx)
{
	if (!ctx)
		return NULL;
	clear_error(ctx);

	char saved_dir[512];
	snprintf(saved_dir, sizeof(saved_dir), "%s/saved", ctx->config_dir);
	DIR *d = opendir(saved_dir);
	if (!d)
		return NULL;

	/* count .json files */
	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL)
		if (saved_id_len(ent->d_name))
			count++;
	rewinddir(d);

	shyake_saved_list *list = calloc(1, sizeof(shyake_saved_list));
	if (count == 0) {
		closedir(d);
		return list;
	}
	list->entries = calloc(count, sizeof(shyake_saved_entry));

	/* load KEM secret key once */
	usize ksk_len;
	u8 *ksk = load_kem_sk(ctx, &ksk_len);
	int idx = 0;
	while ((ent = readdir(d)) != NULL && idx < count) {
		usize id_len = saved_id_len(ent->d_name);
		if (id_len &&
		    fill_saved_entry(ctx, &list->entries[idx], saved_dir,
				     ent->d_name, id_len, ksk) == 0)
			idx++;
	}
	list->count = idx;
	free_secret(ksk, ksk_len);
	closedir(d);
	return list;
}

/* ------------------------------------------------------------------ */
/* Finding a referenced mail                                          */
/* ------------------------------------------------------------------ */

/* mail ID in a listed mailbox with this reference, or NULL */
static char *find_ref_in_box(shyake_ctx *ctx, const char *type, const char *ref)
{
	shyake_mail_list *list = shyake_check(ctx, type);
	if (!list)
		return NULL;
	char *id = NULL;
	for (int i = 0; i < list->count && !id; i++) {
		const char *r = list->entries[i].ref;
		if (r && strcmp(r, ref) == 0)
			id = strdup(list->entries[i].mail_id);
	}
	shyake_free_mail_list(list);
	return id;
}

/* mail ID of a saved copy with this reference, or NULL */
static char *find_ref_in_saved(shyake_ctx *ctx, const char *ref)
{
	char dir_path[512];
	snprintf(dir_path, sizeof(dir_path), "%s/saved", ctx->config_dir);
	DIR *d = opendir(dir_path);
	if (!d)
		return NULL;

	char *id = NULL;
	struct dirent *ent;
	while (!id && (ent = readdir(d)) != NULL) {
		usize id_len = saved_id_len(ent->d_name);
		if (!id_len)
			continue;
		char path[768];
		snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
		cJSON *json = load_json(path);
		char *r = json ? json_ref(json) : NULL;
		if (r && strcmp(r, ref) == 0)
			id = strndup(ent->d_name, id_len);
		free(r);
		cJSON_Delete(json);
	}
	closedir(d);
	return id;
}

char *shyake_find_ref(shyake_ctx *ctx, const char *ref, int *saved)
{
	if (saved)
		*saved = 0;
	if (!ctx || !is_ref(ref))
		return NULL;

	char *id = NULL;
	if (ctx->instance_url && ctx->instance_url[0]) {
		id = find_ref_in_box(ctx, "inbox", ref);
		if (!id)
			id = find_ref_in_box(ctx, "sent", ref);
	}
	if (!id) {
		id = find_ref_in_saved(ctx, ref);
		if (id && saved)
			*saved = 1;
	}
	clear_error(ctx);
	return id;
}
