/* Unit tests for the CLI's parsers, formatters and drafts store */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shyake.h"
#include "internal.h"
#include "config.h"
#include "compose.h"
#include "display.h"
#include "drafts.h"
#include "update.h"
#include "check.h"

static char tmpdir[] = "/tmp/shyake-test-cli.XXXXXX";

static void write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void test_trim(void)
{
	char a[] = "  \tvalue \r\n";
	CHECK_STR(cli_trim(a), "value");
	char b[] = " \n";
	CHECK_STR(cli_trim(b), "");
}

static void test_columns(void)
{
	int order[7], n;
	cli_parse_columns(NULL, order, &n);
	CHECK(n == 5 && order[0] == COL_ID && order[4] == COL_DATE);
	cli_parse_columns("subject, from,bogus,size", order, &n);
	CHECK(n == 3 && order[0] == COL_SUBJECT && order[1] == COL_PARTY &&
	      order[2] == COL_SIZE);
	cli_parse_columns("bogus", order, &n);
	CHECK(n == 5);
}

static void test_config(void)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/config", tmpdir);
	write_text(path, "# comment\n"
			 "INSTANCE=\n"
			 "USERNAME = \"alice\"\n"
			 "TIME_FORMAT='%d.%m.'\n"
			 "TIME_ZONE=-6\n"
			 "NO_COLOR=1\n"
			 "UNKNOWN=x\n"
			 "no equals sign\n");
	app_config *cfg = cli_read_config(tmpdir);
	CHECK(cfg && cfg->instance == NULL);
	CHECK_STR(cfg->username, "alice");
	CHECK_STR(cfg->time_format, "%d.%m.");
	CHECK(cfg->tz_hours == -6 && cfg->no_color == 1);
	cli_free_config(cfg);

	/* the account moves to the INSTANCE line, the old USERNAME goes */
	CHECK(cli_set_account(tmpdir, "bob", "https://b.example") == 0);
	cfg = cli_read_config(tmpdir);
	CHECK_STR(cfg->instance, "https://b.example");
	CHECK_STR(cfg->username, "bob");
	CHECK_STR(cfg->time_format, "%d.%m.");
	cli_free_config(cfg);

	cfg = cli_read_config("/nonexistent");
	CHECK(cfg && cfg->tz_hours == TZ_AUTO && !cfg->instance);
	cli_free_config(cfg);
}

static void test_compose(void)
{
	char buf[] = "To: bob \nSubject:  hi there\n---\nbody\n";
	char *to, *subject, *body;
	CHECK(cli_parse_compose(buf, &to, &subject, &body) == 0);
	CHECK_STR(to, "bob");
	CHECK_STR(subject, "hi there");
	CHECK_STR(body, "body\n");
	char none[] = "To: bob\nSubject: x\n";
	CHECK(cli_parse_compose(none, &to, &subject, &body) == -1);

	/* the scissors line splits quote from template; one inside the
	 * quoted mail is indented so it cannot cut there */
	const char *cut = "# ------------------------ >8 "
			  "------------------------";
	char orig[256];
	snprintf(orig, sizeof(orig), "FROM: a\n%s\nquoted", cut);
	char *full = cli_with_scissors("Subject: x\n---\n", orig);
	CHECK(full != NULL);
	CHECK_STR(cli_after_scissors(full), "Subject: x\n---\n");
	free(full);
	CHECK(cli_after_scissors("no scissors here\n") == NULL);
}

static void test_utf8(void)
{
	CHECK(cli_utf8_width("abc") == 3);
	CHECK(cli_utf8_width("\xe4\xb8\xad\xe6\x96\x87") == 4); /* 中文 */
	char buf[64];
	CHECK(cli_utf8_truncate("abcdef", buf, sizeof(buf), 10) == 6);
	CHECK_STR(buf, "abcdef");
	CHECK(cli_utf8_truncate("abcdefgh", buf, sizeof(buf), 6) == 6);
	CHECK_STR(buf, "abc...");
	/* a wide character never splits */
	CHECK(cli_utf8_truncate("\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97", buf,
				sizeof(buf), 5) == 5);
	CHECK_STR(buf, "\xe4\xb8\xad...");
}

static void test_formatters(void)
{
	char buf[64];
	cli_format_size(1023, buf, sizeof(buf));
	CHECK_STR(buf, "1023");
	cli_format_size(4096, buf, sizeof(buf));
	CHECK_STR(buf, "4K");
	cli_format_party("bob@b.example", buf, sizeof(buf));
	CHECK_STR(buf, "bob@");
	cli_format_party("bob", buf, sizeof(buf));
	CHECK_STR(buf, "bob");
	cli_format_timestamp(0, 8, NULL, NULL, buf, sizeof(buf));
	CHECK_STR(buf, "1970-01-01 08:00");
	cli_format_timestamp(86400, 0, "%d/%m", NULL, buf, sizeof(buf));
	CHECK_STR(buf, "02/01");
}

static void test_versions(void)
{
	CHECK(cli_version_cmp("v0.4.1", "v0.4.0") > 0);
	CHECK(cli_version_cmp("v0.4.0-rc1", "v0.4.0") < 0);
	CHECK(cli_version_cmp("v1.0.0", "v0.9.9") > 0);
	CHECK(cli_version_cmp("v0.4.0", "v0.4.0") == 0);
}

/* drafts round-trip through the user's own KEM key */
static void test_drafts(void)
{
	shyake_config c = { .config_dir = tmpdir };
	shyake_ctx *ctx = shyake_init_ctx(&c);
	CHECK(shyake_generate_keys(ctx) == 0);

	char *id = NULL;
	const char *body = "draft body";
	CHECK(cli_save_draft(ctx, tmpdir, "bob", "subj", NULL, (const u8 *)body,
			     strlen(body), NULL, &id) == SHYAKE_OK);
	CHECK_STR(id, "1");
	shyake_mail_detail *d = cli_read_draft(ctx, tmpdir, "alice", "1");
	CHECK(d != NULL);
	if (d) {
		CHECK_STR(d->recipient, "bob");
		CHECK_STR(d->subject, "subj");
		CHECK_STR(d->body, body);
		CHECK(d->reply_ref == NULL);
	}
	shyake_free_mail_detail(d);

	/* overwriting keeps the id; a missing id is NOT_FOUND */
	CHECK(cli_save_draft(ctx, tmpdir, "", "", NULL, (const u8 *)"x", 1, "1",
			     NULL) == SHYAKE_OK);
	CHECK(cli_save_draft(ctx, tmpdir, "", "", NULL, (const u8 *)"x", 1, "9",
			     NULL) == SHYAKE_ERR_NOT_FOUND);
	shyake_saved_list *list = cli_list_drafts(ctx, tmpdir, "alice");
	CHECK(list && list->count == 1);
	shyake_free_saved_list(list);
	CHECK(cli_delete_draft(tmpdir, "1") == SHYAKE_OK);
	CHECK(cli_delete_draft(tmpdir, "1") == SHYAKE_ERR_NOT_FOUND);
	CHECK(cli_read_draft(ctx, tmpdir, "alice", "1") == NULL);
	free(id);
	shyake_free_ctx(ctx);
}

int main(void)
{
	if (!mkdtemp(tmpdir)) {
		perror("mkdtemp");
		return 1;
	}
	test_trim();
	test_columns();
	test_config();
	test_compose();
	test_utf8();
	test_formatters();
	test_versions();
	test_drafts();

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if (system(cmd) != 0)
		fprintf(stderr, "cannot remove %s\n", tmpdir);
	return check_report("test_cli");
}
