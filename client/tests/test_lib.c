/* Unit tests for libshyake internals: encoding, PoW, keys, errors */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include "lib_internal.h"
#include "check.h"

static char tmpdir[64];

static void test_base64(void)
{
	const u8 data[] = { 0xfb, 0xff, 0x00, 'a', 'b' };
	for (usize n = 0; n <= sizeof(data); n++) {
		char *b64 = base64_encode(data, n);
		usize len = 99;
		u8 *back = base64_decode(b64, &len);
		CHECK(back && len == n && memcmp(back, data, n) == 0);
		free(b64);
		free(back);
	}

	/* URL-safe alphabet, missing padding, trailing newline */
	usize len;
	u8 *d = base64_decode("-_8", &len);
	CHECK(d && len == 2 && d[0] == 0xfb && d[1] == 0xff);
	free(d);
	d = base64_decode("YWI=\n", &len);
	CHECK(d && len == 2 && memcmp(d, "ab", 2) == 0);
	free(d);
}

static void test_hex(void)
{
	const u8 in[] = { 0x00, 0x0f, 0xa5, 0xff };
	char out[9];
	hex_encode(in, sizeof(in), out);
	CHECK_STR(out, "000fa5ff");
	hex_encode(in, 0, out);
	CHECK_STR(out, "");
}

static void test_fill_random(void)
{
	u8 a[32] = { 0 }, b[32] = { 0 };
	CHECK(fill_random(a, sizeof(a)) == 1);
	CHECK(fill_random(b, sizeof(b)) == 1);
	CHECK(memcmp(a, b, sizeof(a)) != 0);
}

/* hashcash stamp: version 1, the bits asked, the resource, and a
 * SHA-1 with that many leading zero bits */
static void test_pow(void)
{
	char *stamp = shyake_mint_pow("alice", 12);
	CHECK(stamp != NULL);
	if (!stamp)
		return;
	CHECK(strncmp(stamp, "1:12:", 5) == 0);
	CHECK(strstr(stamp, ":alice::") != NULL);
	u8 h[SHA_DIGEST_LENGTH];
	SHA1((const u8 *)stamp, strlen(stamp), h);
	CHECK(h[0] == 0 && (h[1] & 0xf0) == 0);
	free(stamp);
}

static void test_files_and_keys(void)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/blob", tmpdir);
	const u8 data[] = "binary\0data";
	CHECK(save_file(path, data, sizeof(data)) == 0);
	usize len;
	u8 *back = load_file(path, &len);
	CHECK(back && len == sizeof(data) && memcmp(back, data, len) == 0);
	free(back);
	CHECK(load_file("/nonexistent/x", &len) == NULL);

	/* a secret key saved under a passphrase opens only with it */
	shyake_config cfg = { .config_dir = tmpdir };
	shyake_ctx *ctx = shyake_init_ctx(&cfg);
	snprintf(path, sizeof(path), "%s/sk.bin", tmpdir);
	const u8 sk[] = "secret key bytes";
	CHECK(save_sk_encrypted(path, "pass", sk, sizeof(sk)) == 0);
	shyake_set_passphrase(ctx, "pass");
	u8 *open = load_sk_decrypted(ctx, path, &len);
	CHECK(open && len == sizeof(sk) && memcmp(open, sk, len) == 0);
	free(open);
	shyake_set_passphrase(ctx, "wrong");
	CHECK(load_sk_decrypted(ctx, path, &len) == NULL);
	shyake_free_ctx(ctx);
}

static void test_server_errors(void)
{
	shyake_config cfg = { .config_dir = tmpdir,
			      .instance_url = "https://example.org" };
	shyake_ctx *ctx = shyake_init_ctx(&cfg);

	/* known texts get a plain reason; others pass through */
	set_server_error(ctx, 401, "{\"error\":\"Timestamp out of window\"}");
	CHECK_STR(shyake_last_error(ctx),
		  "Your clock is off by more than 5 minutes.");
	set_server_error(ctx, 400, "{\"error\":\"Something odd\"}");
	CHECK_STR(shyake_last_error(ctx), "Something odd.");
	set_server_error(ctx, 400, "{\"error\":\"bad\\u0007bell\"}");
	CHECK_STR(shyake_last_error(ctx), "bad?bell.");
	set_server_error(ctx, 429, "not json");
	CHECK_STR(shyake_last_error(ctx),
		  "Too many requests. Try again later.");
	set_server_error(ctx, 503, "");
	CHECK_STR(shyake_last_error(ctx),
		  "The instance had an internal error.");
	set_server_error(ctx, 418, "");
	CHECK_STR(shyake_last_error(ctx),
		  "The instance answered with HTTP 418.");

	CHECK(http_error_is("{\"error\":\"Mail not found\"}",
			    "Mail not found"));
	CHECK(!http_error_is("{\"error\":\"Mail not found\"}", "Mail"));
	CHECK(!http_error_is("garbage", "Mail not found"));
	shyake_free_ctx(ctx);
}

/* a saved mail missing fields is refused, not read half-way */
static void test_malformed_saved(void)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/saved", tmpdir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/saved/bad.json", tmpdir);
	const char *json = "{\"sender\":\"alice\",\"timestamp\":\"soon\"}";
	CHECK(save_file(path, (const u8 *)json, strlen(json)) == 0);

	shyake_config cfg = { .config_dir = tmpdir, .username = "alice" };
	shyake_ctx *ctx = shyake_init_ctx(&cfg);
	CHECK(shyake_read_saved(ctx, "bad") == NULL);
	CHECK_STR(shyake_last_error(ctx), "Saved mail bad is damaged.");
	CHECK(shyake_check_saved_one(ctx, "bad") == NULL);
	shyake_saved_list *list = shyake_list_saved(ctx);
	CHECK(list && list->count == 0);
	shyake_free_saved_list(list);
	shyake_free_ctx(ctx);
}

static void test_crypto_round_trip(void)
{
	u8 key[32];
	CHECK(fill_random(key, sizeof(key)));
	char *b64 = encrypt_to_b64(key, (const u8 *)"subject", 7);
	CHECK(b64 != NULL);
	char *pt = decrypt_from_b64(key, b64);
	CHECK_STR(pt, "subject");
	free(pt);
	key[0] ^= 1;
	CHECK(decrypt_from_b64(key, b64) == NULL);
	free(b64);
	CHECK(encrypt_to_b64(key, (const u8 *)"", 0) == NULL);
	CHECK(decrypt_from_b64(key, "") == NULL);
}

int main(void)
{
	/* mkdtemp() is hidden on macOS under _POSIX_C_SOURCE */
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/shyake-test-lib.%ld",
		 (long)getpid());
	if (mkdir(tmpdir, 0700) != 0) {
		perror(tmpdir);
		return 1;
	}
	test_base64();
	test_hex();
	test_fill_random();
	test_pow();
	test_files_and_keys();
	test_server_errors();
	test_crypto_round_trip();
	test_malformed_saved();

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if (system(cmd) != 0)
		fprintf(stderr, "cannot remove %s\n", tmpdir);
	return check_report("test_lib");
}
