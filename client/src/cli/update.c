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
#include "internal.h"
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* growable buffer for curl responses */
struct mem_buf {
	char *data;
	usize size;
};

static usize mem_write_cb(void *contents, usize size, usize nmemb, void *userp)
{
	usize total = size * nmemb;
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
static u8 *read_file(const char *path, usize *len)
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
	u8 *data = malloc((usize)sz);
	if (!data) {
		fclose(f);
		return NULL;
	}
	if (fread(data, 1, (usize)sz, f) != (usize)sz) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (usize)sz;
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

/* version info from the JSON of the version endpoint */
static cli_version_info *parse_version_info(const char *body)
{
	cJSON *json = cJSON_Parse(body);
	if (!json)
		return NULL;
	cJSON *rel = cJSON_GetObjectItem(json, "release");
	cJSON *pre = cJSON_GetObjectItem(json, "pre_release");
	char asset[64];
	snprintf(asset, sizeof(asset), "%s.tar.gz", platform_artifact());

	cli_version_info *info = calloc(1, sizeof(cli_version_info));
	if (cJSON_IsString(rel))
		info->release = strdup(rel->valuestring);
	if (cJSON_IsString(pre))
		info->pre_release = strdup(pre->valuestring);
	info->release_digest = extract_digest(json, "release_digests", asset);
	info->pre_release_digest =
		extract_digest(json, "pre_release_digests", asset);
	cJSON_Delete(json);
	return info;
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
	long http_code = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	cli_version_info *info =
		http_code == 200 ? parse_version_info(resp.data) : NULL;

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

	usize data_len = 0;
	u8 *data = read_file(file_path, &data_len);
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

/* path of the running binary into buf; -1 when it cannot be found */
static int find_self_path(char *buf, usize len)
{
	memset(buf, 0, len);
#if defined(__linux__)
	readlink("/proc/self/exe", buf, len - 1);
#elif defined(__APPLE__)
	u32 sz = (u32)len;
	_NSGetExecutablePath(buf, &sz);
#endif
	if (buf[0] == '\0') {
		FILE *wp = popen("which shyake", "r");
		if (wp) {
			fgets(buf, (int)len, wp);
			pclose(wp);
		}
		usize l = strlen(buf);
		if (l > 0 && buf[l - 1] == '\n')
			buf[l - 1] = '\0';
	}
	return buf[0] ? 0 : -1;
}

/* download target, check it against digest, replace this binary */
static int install_release(const char *target, const char *digest, int debug)
{
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
		fprintf(stderr,
			"Update failed. Cannot download the release.\n");
		return -1;
	}

	/* archive layout: <artifact>/shyake */
	const char *why = NULL;
	char self_path[512];
	char extract_cmd[1024];
	int cmd_len = 0;
	if (verify_sha256(tar_path, digest) != 0)
		why = "The download does not match its checksum.";
	else if (find_self_path(self_path, sizeof(self_path)) != 0)
		why = "Cannot find the shyake binary.";
	else
		cmd_len = snprintf(
			extract_cmd, sizeof(extract_cmd),
			"tar xzf '%s' -C /tmp && cp '/tmp/%s/shyake' "
			"'%s' && chmod 755 '%s' && rm -rf '/tmp/%s'",
			tar_path, artifact, self_path, self_path, artifact);
	if (!why && (cmd_len < 0 || (usize)cmd_len >= sizeof(extract_cmd)))
		why = "The install path is too long. Reinstall manually.";
	else if (!why && system(extract_cmd) != 0)
		why = "Cannot install the new binary.";

	remove(tar_path);
	free(tar_path);
	if (why) {
		fprintf(stderr, "Update failed. %s\n", why);
		return -1;
	}
	printf("Updated to %s.\n", target);
	return 0;
}

int cli_self_update(const char *version_url, const char *current_version,
		    cli_update_channel channel, int debug)
{
	cli_version_info *info = cli_get_latest_version(version_url, debug);
	if (!info) {
		fprintf(stderr,
			"Update failed. Cannot get the latest version.\n");
		return -1;
	}

	int preview = channel == CLI_UPDATE_PREVIEW;
	const char *target = preview ? info->pre_release : info->release;
	const char *digest = preview ? info->pre_release_digest :
				       info->release_digest;
	const char *channel_name = preview ? "preview" : "stable";
	int ret = -1;

	if (!info->release) {
		fprintf(stderr, "Update failed. Stable release not found.\n");
	} else if (!target) {
		fprintf(stderr,
			"Update failed. Release not found on the %s "
			"channel.\n",
			channel_name);
	} else if (preview &&
		   cli_version_cmp(info->pre_release, info->release) <= 0) {
		/* reject preview that is not newer than stable */
		fprintf(stderr, "Update failed. No preview release is newer "
				"than stable.\n");
	} else if (current_version && strcmp(current_version, target) == 0) {
		printf("Already on the latest %s release.\n", channel_name);
		ret = 0;
	} else if (!digest) {
		fprintf(stderr, "Update failed. The release has no checksum "
				"for this platform.\n");
	} else {
		ret = install_release(target, digest, debug);
	}

	cli_free_version_info(info);
	return ret;
}
