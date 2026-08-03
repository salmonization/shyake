#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include "prompt.h"
#include "shyake.h"

int read_passphrase(const char *prompt_str, char *buf, size_t buflen)
{
	buf[0] = '\0';

	/* CI/test automation: env var bypasses interactive prompt entirely.
	 * Avoids both stdin-consumed and no-tty problems in pipelines. */
	const char *env_pp = getenv("SHYAKE_PASSPHRASE");
	if (env_pp != NULL) {
		strncpy(buf, env_pp, buflen - 1);
		buf[buflen - 1] = '\0';
		return 0;
	}

	/* Always read from /dev/tty so that a consumed stdin (e.g. piped
	 * body in `send`) does not cause an immediate EOF here. */
	FILE *tty = fopen("/dev/tty", "r+");
	if (!tty) {
		/* No controlling terminal (CI, container): treat as empty. */
		return 0;
	}

	fprintf(tty, "%s", prompt_str);
	fflush(tty);

	struct termios old, noecho;
	int saved = 0;
	int fd = fileno(tty);
	if (tcgetattr(fd, &old) == 0) {
		noecho = old;
		noecho.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(fd, TCSAFLUSH, &noecho);
		saved = 1;
	}

	char *result = fgets(buf, (int)buflen, tty);

	if (saved) {
		tcsetattr(fd, TCSAFLUSH, &old);
		fputs("\n", tty);
		fflush(tty);
	}

	fclose(tty);

	if (!result) {
		buf[0] = '\0';
		return -1;
	}

	size_t len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';
	return 0;
}

int sk_file_is_encrypted(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	unsigned char magic[4];
	int n = (int)fread(magic, 1, 4, f);
	fclose(f);
	return n == 4 && magic[0] == 'S' && magic[1] == 'H' &&
	       magic[2] == 'Y' && magic[3] == 'K';
}

int prompt_passphrase(shyake_ctx *ctx, const char *config_dir)
{
	char sk_path[512];
	snprintf(sk_path, sizeof(sk_path), "%s/kem_sk.bin", config_dir);
	if (!sk_file_is_encrypted(sk_path))
		return 0;

	char prompt_str[600];
	snprintf(prompt_str, sizeof(prompt_str),
		 "Enter passphrase for key '%s': ", sk_path);

	char buf[512];
	if (read_passphrase(prompt_str, buf, sizeof(buf)) != 0) {
		memset(buf, 0, sizeof(buf));
		return -1;
	}
	shyake_set_passphrase(ctx, buf);
	memset(buf, 0, sizeof(buf));
	return 0;
}

int prompt_new_passphrase(shyake_ctx *ctx, const char *config_dir)
{
	char sk_path[512];
	snprintf(sk_path, sizeof(sk_path), "%s/kem_sk.bin", config_dir);

	if (!getenv("SHYAKE_PASSPHRASE")) {
		fprintf(stderr, "Key: %s\n", sk_path);
		fflush(stderr);
	}

	char pp1[512], pp2[512];
	if (read_passphrase("Enter passphrase (empty for no passphrase): ", pp1,
			    sizeof(pp1)) != 0) {
		memset(pp1, 0, sizeof(pp1));
		return -1;
	}

	if (pp1[0] == '\0') {
		shyake_set_new_passphrase(ctx, "");
		return 0;
	}

	if (read_passphrase("Enter same passphrase again: ", pp2,
			    sizeof(pp2)) != 0) {
		memset(pp1, 0, sizeof(pp1));
		memset(pp2, 0, sizeof(pp2));
		return -1;
	}

	if (strcmp(pp1, pp2) != 0) {
		fprintf(stderr, "Passphrases do not match.\n");
		memset(pp1, 0, sizeof(pp1));
		memset(pp2, 0, sizeof(pp2));
		return -1;
	}

	shyake_set_new_passphrase(ctx, pp1);
	memset(pp1, 0, sizeof(pp1));
	memset(pp2, 0, sizeof(pp2));
	return 0;
}
