#ifndef SHYAKE_CLI_UPDATE_H
#define SHYAKE_CLI_UPDATE_H

/* Self-update of this reference CLI. Distribution-specific
 * (GitHub releases, platform artifacts); not part of libshyake. */

typedef struct {
	char *release; /* latest stable tag, e.g. "v0.1.1" */
	char *pre_release; /* latest pre-release tag, may be NULL */
	char *release_digest; /* sha256 hex of this platform's stable
                                 asset, may be NULL */
	char *pre_release_digest; /* same for the preview asset */
} cli_version_info;

typedef enum {
	CLI_UPDATE_STABLE = 0,
	CLI_UPDATE_PREVIEW = 1,
} cli_update_channel;

void cli_free_version_info(cli_version_info *v);

/*
 * Semver comparison: >0 if a>b, <0 if a<b, 0 if equal.
 * Handles "vX.Y.Z" and "vX.Y.Z-prerelease" (release > prerelease).
 */
int cli_version_cmp(const char *a, const char *b);

/*
 * Query version_url for the latest client version.
 * Returns allocated cli_version_info* on success, NULL on failure.
 */
cli_version_info *cli_get_latest_version(const char *version_url, int debug);

/*
 * Download and install the latest stable or preview release binary.
 * current_version: running version string (e.g. "v0.1.1").
 * Returns 0 on success, -1 on failure.
 */
int cli_self_update(const char *version_url, const char *current_version,
		    cli_update_channel channel, int debug);

#endif /* SHYAKE_CLI_UPDATE_H */
