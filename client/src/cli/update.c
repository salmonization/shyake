#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include "cJSON.h"
#include "update.h"
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* growable buffer for curl responses */
struct mem_buf {
	char *data;
	size_t size;
};

static size_t mem_write_cb(void *contents, size_t size, size_t nmemb,
			   void *userp)
{
	size_t total = size * nmemb;
	struct mem_buf *buf = userp;
	char *p = realloc(buf->data, buf->size + total + 1);
	if (!p)
		return 0;
	buf->data = p;
	memcpy(buf->data + buf->size, contents, total);
	buf->size += total;
	buf->data[buf->size] = '\0';
	return total;
}

/* read whole file into malloc'd buffer */
static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *data = malloc((size_t)sz);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)sz;
	return data;
}

void cli_free_version_info(cli_version_info *v)
{
	if (!v)
		return;
	free(v->release);
	free(v->pre_release);
	free(v->release_digest);
	free(v->pre_release_digest);
	free(v);
}

/* compile-time platform artifact name */
static const char *platform_artifact(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
	return "shyake-darwin-arm64";
#elif defined(__APPLE__)
	return "shyake-darwin-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
	return "shyake-linux-aarch64";
#else
	return "shyake-linux-x86_64";
#endif
}

/* pull digest of this platform's asset from a digests object */
static char *extract_digest(cJSON *json, const char *key, const char *asset)
{
	cJSON *digests = cJSON_GetObjectItem(json, key);
	if (!digests)
		return NULL;
	cJSON *d = cJSON_GetObjectItem(digests, asset);
	if (!d || !cJSON_IsString(d))
		return NULL;
	const char *v = d->valuestring;
	if (strncmp(v, "sha256:", 7) == 0)
		v += 7;
	return strdup(v);
}

/* parse "vX.Y.Z" or "vX.Y.Z-anything" into components */
static void parse_ver(const char *s, int *maj, int *min, int *pat, int *has_pre)
{
	*maj = *min = *pat = *has_pre = 0;
	if (!s || !*s)
		return;
	const char *p = (*s == 'v') ? s + 1 : s;
	*maj = atoi(p);
	p = strchr(p, '.');
	if (!p)
		return;
	p++;
	*min = atoi(p);
	p = strchr(p, '.');
	if (!p)
		return;
	p++;
	*pat = atoi(p);
	*has_pre = (strchr(p, '-') != NULL);
}

int cli_version_cmp(const char *a, const char *b)
{
	int a_maj, a_min, a_pat, a_pre;
	int b_maj, b_min, b_pat, b_pre;
	parse_ver(a, &a_maj, &a_min, &a_pat, &a_pre);
	parse_ver(b, &b_maj, &b_min, &b_pat, &b_pre);

	if (a_maj != b_maj)
		return a_maj - b_maj;
	if (a_min != b_min)
		return a_min - b_min;
	if (a_pat != b_pat)
		return a_pat - b_pat;
	/* same base: release (no pre) > prerelease */
	if (a_pre != b_pre)
		return a_pre ? -1 : 1;
	return 0;
}

cli_version_info *cli_get_latest_version(const char *version_url, int debug)
{
	if (!version_url)
		return NULL;

	CURL *curl = curl_easy_init();
	if (!curl)
		return NULL;

	if (debug)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	struct mem_buf resp = { .data = malloc(1), .size = 0 };
	resp.data[0] = '\0';

	curl_easy_setopt(curl, CURLOPT_URL, version_url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

	CURLcode res = curl_easy_perform(curl);
	cli_version_info *info = NULL;

	if (res == CURLE_OK) {
		long http_code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code == 200) {
			cJSON *json = cJSON_Parse(resp.data);
			if (json) {
				cJSON *rel =
					cJSON_GetObjectItem(json, "release");
				cJSON *pre = cJSON_GetObjectItem(json,
								 "pre_release");

				char asset[64];
				snprintf(asset, sizeof(asset), "%s.tar.gz",
					 platform_artifact());

				info = calloc(1, sizeof(cli_version_info));
				if (rel && cJSON_IsString(rel))
					info->release =
						strdup(rel->valuestring);
				if (pre && cJSON_IsString(pre))
					info->pre_release =
						strdup(pre->valuestring);
				info->release_digest = extract_digest(
					json, "release_digests", asset);
				info->pre_release_digest = extract_digest(
					json, "pre_release_digests", asset);

				cJSON_Delete(json);
			}
		}
	}

	free(resp.data);
	curl_easy_cleanup(curl);
	return info;
}

/* download a URL to a tmp file, return allocated path or NULL */
static char *download_to_tmp(const char *download_url, const char *filename,
			     int debug)
{
	char *tmp_path = malloc(256);
	snprintf(tmp_path, 256, "/tmp/%s", filename);

	CURL *curl = curl_easy_init();
	if (!curl) {
		free(tmp_path);
		return NULL;
	}

	FILE *f = fopen(tmp_path, "wb");
	if (!f) {
		curl_easy_cleanup(curl);
		free(tmp_path);
		return NULL;
	}

	if (debug)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	curl_easy_setopt(curl, CURLOPT_URL, download_url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	fclose(f);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || http_code != 200) {
		remove(tmp_path);
		free(tmp_path);
		return NULL;
	}
	return tmp_path;
}

/* compare file SHA-256 with expected hex digest, 0 = match */
static int verify_sha256(const char *file_path, const char *expected_hex)
{
	if (!expected_hex || strlen(expected_hex) != SHA256_DIGEST_LENGTH * 2)
		return -1;

	size_t data_len = 0;
	uint8_t *data = read_file(file_path, &data_len);
	if (!data)
		return -1;

	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(data, data_len, digest);
	free(data);

	char actual[SHA256_DIGEST_LENGTH * 2 + 1];
	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
		snprintf(actual + i * 2, 3, "%02x", digest[i]);

	return strcasecmp(expected_hex, actual) == 0 ? 0 : -1;
}

int cli_self_update(const char *version_url, const char *current_version,
		    cli_update_channel channel, int debug)
{
	cli_version_info *info = cli_get_latest_version(version_url, debug);
	if (!info) {
		fprintf(stderr, "Failed to fetch version info.\n");
		return -1;
	}

	if (!info->release) {
		cli_free_version_info(info);
		fprintf(stderr, "No stable release available.\n");
		return -1;
	}

	const char *target = (channel == CLI_UPDATE_PREVIEW) ?
				     info->pre_release :
				     info->release;
	const char *channel_name = (channel == CLI_UPDATE_PREVIEW) ? "preview" :
								     "stable";

	if (!target) {
		fprintf(stderr, "No %s release available.\n", channel_name);
		cli_free_version_info(info);
		return -1;
	}

	/* reject preview that is not newer than stable */
	if (channel == CLI_UPDATE_PREVIEW &&
	    cli_version_cmp(info->pre_release, info->release) <= 0) {
		fprintf(stderr,
			"No preview release available newer than stable.\n");
		cli_free_version_info(info);
		return -1;
	}

	/* already on the requested target */
	if (current_version && strcmp(current_version, target) == 0) {
		printf("Already on the latest %s release.\n", channel_name);
		cli_free_version_info(info);
		return 0;
	}

	const char *digest = (channel == CLI_UPDATE_PREVIEW) ?
				     info->pre_release_digest :
				     info->release_digest;
	if (!digest) {
		fprintf(stderr, "No checksum available for this platform.\n");
		cli_free_version_info(info);
		return -1;
	}

	const char *artifact = platform_artifact();
	char asset[64];
	snprintf(asset, sizeof(asset), "%s.tar.gz", artifact);

	char dl_url[512];
	snprintf(dl_url, sizeof(dl_url),
		 "https://github.com/salmonization/shyake/releases/download"
		 "/%s/%s",
		 target, asset);

	fprintf(stderr, "Downloading %s %s...\n", target, asset);

	char *tar_path = download_to_tmp(dl_url, asset, debug);
	if (!tar_path) {
		fprintf(stderr, "Failed to download release archive.\n");
		cli_free_version_info(info);
		return -1;
	}

	/* verify sha256 */
	if (verify_sha256(tar_path, digest) != 0) {
		fprintf(stderr, "SHA-256 verification failed. Aborting.\n");
		remove(tar_path);
		free(tar_path);
		cli_free_version_info(info);
		return -1;
	}

	/* find current binary path */
	char self_path[512] = { 0 };
#if defined(__linux__)
	readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
#elif defined(__APPLE__)
	uint32_t sz = sizeof(self_path);
	_NSGetExecutablePath(self_path, &sz);
#endif

	if (self_path[0] == '\0') {
		FILE *wp = popen("which shyake", "r");
		if (wp) {
			fgets(self_path, sizeof(self_path), wp);
			pclose(wp);
		}
		size_t l = strlen(self_path);
		if (l > 0 && self_path[l - 1] == '\n')
			self_path[l - 1] = '\0';
	}

	if (self_path[0] == '\0') {
		fprintf(stderr, "Cannot determine shyake binary path.\n");
		remove(tar_path);
		free(tar_path);
		cli_free_version_info(info);
		return -1;
	}

	/* archive layout: <artifact>/shyake */
	char extract_cmd[1024];
	snprintf(extract_cmd, sizeof(extract_cmd),
		 "tar xzf '%s' -C /tmp && cp '/tmp/%s/shyake' '%s' && "
		 "chmod 755 '%s' && rm -rf '/tmp/%s'",
		 tar_path, artifact, self_path, self_path, artifact);
	int ext_ret = system(extract_cmd);

	char installed_ver[64];
	snprintf(installed_ver, sizeof(installed_ver), "%s", target);

	remove(tar_path);
	free(tar_path);
	cli_free_version_info(info);

	if (ext_ret != 0) {
		fprintf(stderr, "Installation failed.\n");
		return -1;
	}

	printf("Successfully updated to %s.\n", installed_ver);
	return 0;
}
