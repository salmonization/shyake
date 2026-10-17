/*
 * Local encrypted drafts (doubles as an encrypted diary).
 *
 * Draft file: <config_dir>/drafts/<id>.json
 * {
 *   "version": 1,
 *   "draft_id": "3",
 *   "created": <unix seconds>,
 *   "modified": <unix seconds>,
 *   "size": <plaintext body bytes>,
 *   "enc_key": "<b64: kem_ct || (sym_key XOR ss)>",
 *   "enc_recipient": "<b64: nonce||ct||mac>" ("" = no recipient),
 *   "enc_subject": "<b64>" ("" = empty subject),
 *   "enc_body": "<b64>",
 *   "enc_reply_to": "<b64>" (a reply only: the mail it answers)
 * }
 *
 * Same hybrid scheme as sent mail, via libshyake's public
 * self-encryption primitives: a random 32-byte symmetric key seals
 * each field, and the key is ML-KEM-768-encapsulated to the user's
 * own kem_pk. Saving a draft therefore needs no passphrase; reading
 * one does.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include "cJSON.h"
#include "drafts.h"

/* zero a symmetric key after use */
/* detail of the last drafts failure, "" if none */
static char drafts_error[256];

const char *cli_drafts_last_error(void)
{
	return drafts_error;
}

static void drafts_set_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(drafts_error, sizeof(drafts_error), fmt, ap);
	va_end(ap);
}

/* ensure drafts/ directory exists */
static int ensure_drafts_dir(const char *config_dir)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/drafts", config_dir);
	struct stat st = { 0 };
	if (stat(path, &st) == -1) {
		if (mkdir(path, 0700) == -1)
			return -1;
	}
	return 0;
}

/* check filename is <digits>.json, return id or -1 */
static long numeric_draft_id(const char *name)
{
	usize nl = strlen(name);
	if (nl <= 5 || strcmp(name + nl - 5, ".json") != 0)
		return -1;
	for (usize i = 0; i < nl - 5; i++) {
		if (name[i] < '0' || name[i] > '9')
			return -1;
	}
	return atol(name);
}

/* scan drafts/ for highest numeric id */
static long max_draft_id(const char *config_dir)
{
	char dir_path[512];
	snprintf(dir_path, sizeof(dir_path), "%s/drafts", config_dir);
	DIR *d = opendir(dir_path);
	if (!d)
		return 0;

	long max_id = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		long id = numeric_draft_id(ent->d_name);
		if (id > max_id)
			max_id = id;
	}
	closedir(d);
	return max_id;
}

/* read whole file into malloc'd NUL-terminated buffer */
static char *read_text_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long flen = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (flen < 0) {
		fclose(f);
		return NULL;
	}
	char *raw = malloc(flen + 1);
	if (!raw) {
		fclose(f);
		return NULL;
	}
	usize rd = fread(raw, 1, (usize)flen, f);
	raw[rd] = '\0';
	fclose(f);
	return raw;
}

/* decrypt optional field; "" maps to empty string */
static char *decrypt_field(const u8 *sym, const char *enc)
{
	if (!enc || enc[0] == '\0')
		return strdup("");
	return shyake_unseal_b64(sym, enc);
}

/*
 * Open the file of draft_id for rewriting, or create the next free
 * id exclusively for a new draft. id_buf receives the id.
 */
static int open_draft_file(const char *config_dir, const char *draft_id,
			   char *id_buf, usize id_len)
{
	char path[640];
	if (draft_id) {
		snprintf(id_buf, id_len, "%s", draft_id);
		snprintf(path, sizeof(path), "%s/drafts/%s.json", config_dir,
			 draft_id);
		return open(path, O_WRONLY | O_TRUNC, 0600);
	}
	int fd = -1;
	long id = max_draft_id(config_dir) + 1;
	for (int tries = 0; tries < 100; tries++, id++) {
		snprintf(id_buf, id_len, "%ld", id);
		snprintf(path, sizeof(path), "%s/drafts/%ld.json", config_dir,
			 id);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd >= 0 || errno != EEXIST)
			break;
	}
	return fd;
}

shyake_err cli_save_draft(shyake_ctx *ctx, const char *config_dir,
			  const char *recipient, const char *subject,
			  const char *reply_ref, const u8 *body, usize body_len,
			  const char *draft_id, char **out_id)
{
	drafts_error[0] = '\0';
	if (!ctx || !config_dir || !body || body_len == 0)
		return SHYAKE_ERR;

	if (ensure_drafts_dir(config_dir) != 0) {
		drafts_set_error("Cannot create the drafts directory.");
		return SHYAKE_ERR;
	}

	/* keep created time when overwriting an existing draft */
	time_t now = time(NULL);
	long created = (long)now;
	if (draft_id) {
		char old_path[640];
		snprintf(old_path, sizeof(old_path), "%s/drafts/%s.json",
			 config_dir, draft_id);
		char *raw = read_text_file(old_path);
		if (!raw)
			return SHYAKE_ERR_NOT_FOUND;
		cJSON *old = cJSON_Parse(raw);
		free(raw);
		if (old) {
			cJSON *c = cJSON_GetObjectItem(old, "created");
			if (c)
				created = (long)c->valuedouble;
			cJSON_Delete(old);
		}
	}

	/* symmetric key encapsulated to own kem_pk */
	u8 sym_key[32];
	char *enc_key = shyake_selfenc_begin(ctx, sym_key);
	if (!enc_key) {
		drafts_set_error("Cannot load kem_pk.bin. Run 'shyake init'.");
		return SHYAKE_ERR_CRYPTO;
	}

	/* encrypt fields; empty ones stored as "" */
	char *enc_rec = NULL, *enc_sub = NULL, *enc_bdy = NULL, *enc_re = NULL;
	if (recipient && recipient[0])
		enc_rec = shyake_seal_b64(sym_key, (const u8 *)recipient,
					  strlen(recipient));
	if (subject && subject[0])
		enc_sub = shyake_seal_b64(sym_key, (const u8 *)subject,
					  strlen(subject));
	if (reply_ref)
		enc_re = shyake_seal_b64(sym_key, (const u8 *)reply_ref,
					 strlen(reply_ref));
	enc_bdy = shyake_seal_b64(sym_key, body, body_len);
	shyake_wipe(sym_key, sizeof(sym_key));

	if (!enc_bdy || (recipient && recipient[0] && !enc_rec) ||
	    (subject && subject[0] && !enc_sub) || (reply_ref && !enc_re)) {
		free(enc_re);
		free(enc_rec);
		free(enc_sub);
		free(enc_bdy);
		free(enc_key);
		return SHYAKE_ERR_CRYPTO;
	}

	char id_buf[32];
	int fd = open_draft_file(config_dir, draft_id, id_buf, sizeof(id_buf));
	if (fd < 0) {
		free(enc_re);
		free(enc_rec);
		free(enc_sub);
		free(enc_bdy);
		free(enc_key);
		return SHYAKE_ERR;
	}

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "version", 1);
	cJSON_AddStringToObject(root, "draft_id", id_buf);
	cJSON_AddNumberToObject(root, "created", (double)created);
	cJSON_AddNumberToObject(root, "modified", (double)now);
	cJSON_AddNumberToObject(root, "size", (double)body_len);
	cJSON_AddStringToObject(root, "enc_key", enc_key);
	cJSON_AddStringToObject(root, "enc_recipient", enc_rec ? enc_rec : "");
	cJSON_AddStringToObject(root, "enc_subject", enc_sub ? enc_sub : "");
	cJSON_AddStringToObject(root, "enc_body", enc_bdy);
	if (enc_re)
		cJSON_AddStringToObject(root, "enc_reply_to", enc_re);
	char *payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	free(enc_re);
	free(enc_rec);
	free(enc_sub);
	free(enc_bdy);
	free(enc_key);

	shyake_err ret = SHYAKE_OK;
	FILE *out = fdopen(fd, "w");
	if (out) {
		fputs(payload, out);
		fclose(out);
	} else {
		close(fd);
		ret = SHYAKE_ERR;
	}
	free(payload);

	if (ret == SHYAKE_OK && out_id)
		*out_id = strdup(id_buf);
	return ret;
}

/* parse and decrypt a draft file */
static shyake_mail_detail *
parse_draft_json(shyake_ctx *ctx, const char *config_dir, const char *username,
		 const char *draft_id, int decrypt_body)
{
	char path[640];
	snprintf(path, sizeof(path), "%s/drafts/%s.json", config_dir, draft_id);

	char *raw = read_text_file(path);
	if (!raw) {
		drafts_set_error("Draft %s not found.", draft_id);
		return NULL;
	}

	cJSON *json = cJSON_Parse(raw);
	free(raw);
	if (!json) {
		drafts_set_error("Draft %s is corrupt.", draft_id);
		return NULL;
	}

	cJSON *jkey = cJSON_GetObjectItem(json, "enc_key");
	cJSON *jbdy = cJSON_GetObjectItem(json, "enc_body");
	cJSON *jupd = cJSON_GetObjectItem(json, "modified");
	cJSON *jcrt = cJSON_GetObjectItem(json, "created");
	cJSON *jsz = cJSON_GetObjectItem(json, "size");
	if (!jkey || !jkey->valuestring || !jbdy || !jbdy->valuestring) {
		drafts_set_error("Draft %s is corrupt.", draft_id);
		cJSON_Delete(json);
		return NULL;
	}
	cJSON *jrec = cJSON_GetObjectItem(json, "enc_recipient");
	cJSON *jsub = cJSON_GetObjectItem(json, "enc_subject");
	cJSON *jre = cJSON_GetObjectItem(json, "enc_reply_to");

	/* drafts are useless undecrypted: fail hard on key errors */
	shyake_selfdec *sd = shyake_selfdec_new(ctx);
	if (!sd) {
		const char *e = shyake_last_error(ctx);
		drafts_set_error("%s", (e && e[0]) ?
					       e :
					       "Cannot unlock the secret key.");
		cJSON_Delete(json);
		return NULL;
	}
	char *rec = NULL, *sub = NULL, *bdy = NULL, *re = NULL;
	u8 sym[32];
	int have_sym = shyake_selfdec_key(sd, jkey->valuestring, sym) ==
		       SHYAKE_OK;
	if (have_sym) {
		rec = decrypt_field(sym, jrec ? jrec->valuestring : NULL);
		sub = decrypt_field(sym, jsub ? jsub->valuestring : NULL);
		if (cJSON_IsString(jre) && jre->valuestring[0])
			re = shyake_unseal_b64(sym, jre->valuestring);
		if (decrypt_body)
			bdy = shyake_unseal_b64(sym, jbdy->valuestring);
		shyake_wipe(sym, sizeof(sym));
	}
	shyake_selfdec_free(sd);
	if (!have_sym || (decrypt_body && !bdy) ||
	    (cJSON_IsString(jre) && jre->valuestring[0] && !re)) {
		drafts_set_error("Cannot decrypt draft %s.", draft_id);
		free(re);
		free(rec);
		free(sub);
		free(bdy);
		cJSON_Delete(json);
		return NULL;
	}

	shyake_mail_detail *result = calloc(1, sizeof(shyake_mail_detail));
	result->mail_id = strdup(draft_id);
	result->sender = strdup((username && username[0]) ? username : "(me)");
	result->recipient = rec;
	result->subject = sub;
	result->body = bdy;
	result->timestamp = jupd ? (i64)jupd->valuedouble : 0;
	result->created = jcrt ? (i64)jcrt->valuedouble : 0;
	result->size = jsz ? jsz->valueint : 0;
	result->reply_ref = re;

	cJSON_Delete(json);
	return result;
}

shyake_mail_detail *cli_read_draft(shyake_ctx *ctx, const char *config_dir,
				   const char *username, const char *draft_id)
{
	drafts_error[0] = '\0';
	if (!ctx || !config_dir || !draft_id)
		return NULL;
	return parse_draft_json(ctx, config_dir, username, draft_id, 1);
}

/* qsort helper: ascending numeric id */
static int draft_entry_cmp(const void *a, const void *b)
{
	long ia = atol(((const shyake_saved_entry *)a)->mail_id);
	long ib = atol(((const shyake_saved_entry *)b)->mail_id);
	return (ia > ib) - (ia < ib);
}

/* one row of the drafts list, from drafts/<name>; -1 if unreadable */
static int fill_draft_entry(shyake_saved_entry *e, shyake_selfdec *sd,
			    const char *dir_path, const char *name, long id,
			    const char *self)
{
	char fpath[768];
	snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, name);
	char *raw = read_text_file(fpath);
	if (!raw)
		return -1;
	cJSON *json = cJSON_Parse(raw);
	free(raw);
	cJSON *jkey = json ? cJSON_GetObjectItem(json, "enc_key") : NULL;
	if (!jkey || !jkey->valuestring) {
		cJSON_Delete(json);
		return -1;
	}
	cJSON *jrec = cJSON_GetObjectItem(json, "enc_recipient");
	cJSON *jsub = cJSON_GetObjectItem(json, "enc_subject");
	cJSON *jupd = cJSON_GetObjectItem(json, "modified");
	cJSON *jcrt = cJSON_GetObjectItem(json, "created");
	cJSON *jsz = cJSON_GetObjectItem(json, "size");

	char *rec = NULL, *sub = NULL;
	u8 sym[32];
	if (shyake_selfdec_key(sd, jkey->valuestring, sym) == SHYAKE_OK) {
		rec = decrypt_field(sym, jrec ? jrec->valuestring : NULL);
		sub = decrypt_field(sym, jsub ? jsub->valuestring : NULL);
		shyake_wipe(sym, sizeof(sym));
	}

	char id_buf[32];
	snprintf(id_buf, sizeof(id_buf), "%ld", id);
	e->mail_id = strdup(id_buf);
	e->sender = strdup(self);
	e->recipient = rec ? rec : strdup("(decryption failed)");
	e->subject = sub ? sub : strdup("(decryption failed)");
	e->timestamp = jupd ? (i64)jupd->valuedouble : 0;
	e->created = jcrt ? (i64)jcrt->valuedouble : 0;
	e->size = jsz ? jsz->valueint : 0;
	cJSON_Delete(json);
	return 0;
}

shyake_saved_list *cli_list_drafts(shyake_ctx *ctx, const char *config_dir,
				   const char *username)
{
	drafts_error[0] = '\0';
	if (!ctx || !config_dir)
		return NULL;

	char dir_path[512];
	snprintf(dir_path, sizeof(dir_path), "%s/drafts", config_dir);

	/* missing directory just means no drafts yet */
	DIR *d = opendir(dir_path);
	if (!d)
		return calloc(1, sizeof(shyake_saved_list));

	/* count draft files */
	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (numeric_draft_id(ent->d_name) >= 0)
			count++;
	}
	rewinddir(d);

	shyake_saved_list *list = calloc(1, sizeof(shyake_saved_list));
	if (count == 0) {
		closedir(d);
		return list;
	}

	/* all metadata is encrypted: no key, no listing */
	shyake_selfdec *sd = shyake_selfdec_new(ctx);
	if (!sd) {
		const char *e = shyake_last_error(ctx);
		drafts_set_error("%s", (e && e[0]) ?
					       e :
					       "Cannot unlock the secret key.");
		closedir(d);
		free(list);
		return NULL;
	}

	list->entries = calloc(count, sizeof(shyake_saved_entry));
	const char *self = (username && username[0]) ? username : "(me)";

	int idx = 0;
	while ((ent = readdir(d)) != NULL && idx < count) {
		long id = numeric_draft_id(ent->d_name);
		if (id >= 0 &&
		    fill_draft_entry(&list->entries[idx], sd, dir_path,
				     ent->d_name, id, self) == 0)
			idx++;
	}

	list->count = idx;
	shyake_selfdec_free(sd);
	closedir(d);

	qsort(list->entries, list->count, sizeof(shyake_saved_entry),
	      draft_entry_cmp);
	return list;
}

shyake_err cli_delete_draft(const char *config_dir, const char *draft_id)
{
	drafts_error[0] = '\0';
	if (!config_dir || !draft_id)
		return SHYAKE_ERR;

	char path[640];
	snprintf(path, sizeof(path), "%s/drafts/%s.json", config_dir, draft_id);
	if (unlink(path) != 0)
		return errno == ENOENT ? SHYAKE_ERR_NOT_FOUND : SHYAKE_ERR;
	return SHYAKE_OK;
}
