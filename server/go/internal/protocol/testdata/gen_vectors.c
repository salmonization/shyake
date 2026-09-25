/*
 * Generate liboqs_vectors.json: signatures made by liboqs over messages
 * built with the client's own cJSON, exactly as libshyake does. The Go
 * tests rebuild each message from its fields and verify it with circl.
 *
 * Build and run from server/go/internal/protocol/testdata:
 *
 *   cc -std=c11 -o /tmp/gen gen_vectors.c \
 *      ../../../../../client/src/lib/vendor/cJSON/cJSON.c \
 *      -I../../../../../client/src/lib/vendor/cJSON \
 *      /usr/local/lib/liboqs.a -lcrypto
 *   /tmp/gen > liboqs_vectors.json
 */
#include <oqs/oqs.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static char *b64(const unsigned char *d, size_t n)
{
	char *out = malloc(4 * ((n + 2) / 3) + 1);
	EVP_EncodeBlock((unsigned char *)out, d, (int)n);
	return out;
}

static OQS_SIG *sig;
static unsigned char *pk, *sk;

static void emit(cJSON *arr, const char *kind, cJSON *fields)
{
	char *msg = cJSON_PrintUnformatted(fields);
	unsigned char *s = malloc(sig->length_signature);
	size_t sl;
	OQS_SIG_sign(sig, s, &sl, (unsigned char *)msg, strlen(msg), sk);

	cJSON *v = cJSON_CreateObject();
	cJSON_AddStringToObject(v, "kind", kind);
	cJSON_AddItemToObject(v, "fields", fields);
	cJSON_AddStringToObject(v, "signed", msg);
	char *sb = b64(s, sl);
	cJSON_AddStringToObject(v, "signature", sb);
	cJSON_AddItemToArray(arr, v);
	free(sb);
	free(s);
	free(msg);
}

static void mail(cJSON *arr, const char *subject, double size)
{
	cJSON *f = cJSON_CreateObject();
	cJSON_AddStringToObject(f, "sender", "alice@a.example");
	cJSON_AddStringToObject(f, "recipient", "bobby@b.example");
	cJSON_AddStringToObject(f, "recipient_kem_fingerprint",
				"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
	cJSON_AddStringToObject(f, "enc_subject", subject);
	cJSON_AddStringToObject(f, "enc_body", "Y2lwaGVydGV4dA==");
	cJSON_AddStringToObject(f, "timestamp", "1749513600");
	cJSON_AddNumberToObject(f, "size", size);
	emit(arr, "mail", f);
}

int main(void)
{
	sig = OQS_SIG_new("ML-DSA-65");
	pk = malloc(sig->length_public_key);
	sk = malloc(sig->length_secret_key);
	OQS_SIG_keypair(sig, pk, sk);

	cJSON *root = cJSON_CreateObject();
	char *pkb = b64(pk, sig->length_public_key);
	cJSON_AddStringToObject(root, "sig_pubkey", pkb);
	cJSON *arr = cJSON_AddArrayToObject(root, "vectors");

	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "username", "salmon");
	cJSON_AddStringToObject(r, "kem_pubkey", "a2VtLXB1YmxpYy1rZXk=");
	cJSON_AddStringToObject(r, "sig_pubkey", pkb);
	cJSON_AddStringToObject(r, "timestamp", "1749513600");
	emit(arr, "register", r);

	mail(arr, "c3ViamVjdA==", 512);
	mail(arr, "", 0);
	/* every escape class, plus what encoding/json would mangle */
	mail(arr, "q\"b\\s/ <&> \b\f\n\r\t \x01\x1f \x7f \xc3\xa9 \xe2\x80\xa8", 196608);

	cJSON *h = cJSON_CreateObject();
	cJSON_AddStringToObject(h, "method", "GET");
	cJSON_AddStringToObject(h, "path", "/api/mail?type=inbox");
	cJSON_AddStringToObject(h, "username", "salmon");
	cJSON_AddStringToObject(h, "timestamp", "1749513600");
	const char *hm = "GET:/api/mail?type=inbox:salmon:1749513600";
	unsigned char *s = malloc(sig->length_signature);
	size_t sl;
	OQS_SIG_sign(sig, s, &sl, (const unsigned char *)hm, strlen(hm), sk);
	cJSON *v = cJSON_CreateObject();
	cJSON_AddStringToObject(v, "kind", "header");
	cJSON_AddItemToObject(v, "fields", h);
	cJSON_AddStringToObject(v, "signed", hm);
	char *sb = b64(s, sl);
	cJSON_AddStringToObject(v, "signature", sb);
	cJSON_AddItemToArray(arr, v);

	char *out = cJSON_Print(root);
	puts(out);
	return 0;
}
