#ifndef SHYAKE_CLI_DRAFTS_H
#define SHYAKE_CLI_DRAFTS_H

#include <stddef.h>
#include <stdint.h>
#include "shyake.h"

/* Local encrypted drafts of this reference CLI. The on-disk JSON
 * format is client-specific; the crypto uses libshyake's public
 * self-encryption primitives. These functions never print: on
 * failure the detail is recorded and read via
 * cli_drafts_last_error(). */

/* Detail of the last drafts failure, "" if none */
const char *cli_drafts_last_error(void);

/*
 * Save an encrypted draft to <config_dir>/drafts/<id>.json.
 * Fields are encrypted to the user's own KEM public key, so no
 * passphrase is needed to save (only to read back).
 * recipient: NULL or "" for a diary entry (no recipient).
 * subject: NULL or "" allowed (unlike send).
 * draft_id: NULL to create a new draft; existing id to overwrite
 *           (created timestamp is preserved, modified is refreshed).
 * out_id: if non-NULL, receives the allocated id string
 *         (caller must free()).
 * Returns SHYAKE_OK on success, SHYAKE_ERR_NOT_FOUND if draft_id
 * does not exist.
 */
shyake_err cli_save_draft(shyake_ctx *ctx, const char *config_dir,
			  const char *recipient, const char *subject,
			  const uint8_t *body, size_t body_len,
			  const char *draft_id, char **out_id);

/*
 * List all local drafts sorted by id (decrypts recipient + subject).
 * Returns allocated shyake_saved_list* on success, NULL on failure.
 * Entry recipient is "" for diary drafts.
 * Caller must free with shyake_free_saved_list().
 */
shyake_saved_list *cli_list_drafts(shyake_ctx *ctx, const char *config_dir,
				   const char *username);

/*
 * Load and decrypt a draft from disk (body included).
 * Returns allocated shyake_mail_detail* on success, NULL on failure.
 * Caller must free with shyake_free_mail_detail().
 */
shyake_mail_detail *cli_read_draft(shyake_ctx *ctx, const char *config_dir,
				   const char *username, const char *draft_id);

/*
 * Delete a draft by id.
 * Returns SHYAKE_OK, SHYAKE_ERR_NOT_FOUND, or SHYAKE_ERR.
 */
shyake_err cli_delete_draft(const char *config_dir, const char *draft_id);

#endif /* SHYAKE_CLI_DRAFTS_H */
