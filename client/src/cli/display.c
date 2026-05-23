#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <openssl/sha.h>
#include "../lib/vendor/cJSON/cJSON.h"
#include "display.h"

/* ------------------------------------------------------------------ */
/* Pager                                                                */
/* ------------------------------------------------------------------ */

static int pager_pid    = -1;
static int saved_stdout = -1;

void
cli_setup_pager(int disable_pager)
{
    // spawn less pager if stdout is TTY
    if (disable_pager || !isatty(STDOUT_FILENO))
        return;

    int fd[2];
    if (pipe(fd) < 0) return;

    pager_pid = fork();
    if (pager_pid < 0) {
        close(fd[0]); close(fd[1]);
        return;
    }
    if (pager_pid == 0) {
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]); close(fd[1]);
        execlp("less", "less", "-F", "-R", "-X", NULL);
        exit(1);
    } else {
        saved_stdout = dup(STDOUT_FILENO);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]); close(fd[1]);
    }
}

void
cli_wait_pager(void)
{
    // wait for less pager process
    if (pager_pid > 0) {
        fflush(stdout);
        close(STDOUT_FILENO);
        if (saved_stdout >= 0) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
            saved_stdout = -1;
        }
        waitpid(pager_pid, NULL, 0);
        pager_pid = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Terminal width                                                        */
/* ------------------------------------------------------------------ */

int
cli_get_terminal_width(void)
{
    // get current terminal width
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

/* ------------------------------------------------------------------ */
/* Word wrap                                                            */
/* ------------------------------------------------------------------ */

void
cli_print_word_wrap(const char *text, int indent, int width)
{
    // word-wrap text with aligned continuation indent
    int avail = width - indent;
    if (avail < 20) avail = 20;

    int len = (int)strlen(text);
    int pos = 0;
    int first = 1;

    while (pos < len) {
        if (!first) printf("\n%*s", indent, "");
        first = 0;

        int remain = len - pos;
        if (remain <= avail) {
            printf("%.*s", remain, text + pos);
            pos += remain;
        } else {
            int cut = avail;
            while (cut > 0 && text[pos + cut] != ' ') cut--;
            if (cut == 0) cut = avail;
            printf("%.*s", cut, text + pos);
            pos += cut;
            while (pos < len && text[pos] == ' ') pos++;
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Fingerprint display                                                  */
/* ------------------------------------------------------------------ */

void
cli_print_fingerprint_hex(const unsigned char *fp)
{
    for (int i = 0; i < 16; i++) {
        printf("%02X%02X", fp[i*2], fp[i*2+1]);
        if (i == 7) printf("\n");
        else if (i == 15) printf("\n");
        else if (i == 3 || i == 11) printf("  ");
        else printf(" ");
    }
}

void
cli_print_randomart(const unsigned char *fp)
{
    int x = 8, y = 4;
    int grid[17][9] = {0};

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        unsigned char b = fp[i];
        for (int j = 0; j < 4; j++) {
            int dir = (b >> (j * 2)) & 3;
            int dx = (dir & 1) ? 1 : -1;
            int dy = (dir & 2) ? 1 : -1;
            x += dx; y += dy;
            if (x < 0) x = 0; if (x > 16) x = 16;
            if (y < 0) y = 0; if (y > 8)  y = 8;
            grid[x][y]++;
        }
    }

    const char *chars = " .o+=*BOX@%&#/^";
    printf("+-----------------+\n");
    for (int j = 0; j < 9; j++) {
        printf("|");
        for (int i = 0; i < 17; i++) {
            if (i == 8 && j == 4) {
                printf("S");
            } else if (i == x && j == y) {
                printf("E");
            } else {
                int val = grid[i][j];
                if (val > 14) val = 14;
                printf("%c", chars[val]);
            }
        }
        printf("|\n");
    }
    printf("+-----------------+\n");
}

void
cli_render_fingerprint(const char *label, const shyake_fp_result *r,
                       int is_self)
{
    if (is_self) {
        printf("Fingerprint for %s (local):\n", label);
        cli_print_fingerprint_hex(r->remote_fp);
        cli_print_randomart(r->remote_fp);
        return;
    }

    if (r->has_local) {
        printf("\n[Local known_hosts]\n");
        cli_print_fingerprint_hex(r->local_fp);
        cli_print_randomart(r->local_fp);

        printf("\n[Remote server]\n");
        cli_print_fingerprint_hex(r->remote_fp);
        cli_print_randomart(r->remote_fp);

        if (r->match)
            printf("\nStatus: MATCH\n");
        else
            printf("\nStatus: MISMATCH (Warning: Key has changed)\n");
    } else {
        printf("\n[Remote server]\n");
        cli_print_fingerprint_hex(r->remote_fp);
        cli_print_randomart(r->remote_fp);
        printf("\nStatus: NEW HOST (No local key found)\n");
    }
}

/* ------------------------------------------------------------------ */
/* Mail list rendering                                                  */
/* ------------------------------------------------------------------ */

void
cli_render_mail_list(const shyake_mail_list *list,
                     const cli_render_opts *opts)
{
    if (!list) return;
    int count = list->count;

    if (opts->count_only) {
        printf("%d\n", count);
        return;
    }

    if (count == 0) {
        if (opts->json_out)
            printf("[]\n");
        else if (!opts->csv_out)
            printf("No mail found.\n");
        return;
    }

    int is_sent = list->count > 0 && list->entries[0].is_sent;

    /* JSON output */
    if (opts->json_out) {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            shyake_mail_entry *e = &list->entries[i];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "mail_id", e->mail_id);
            cJSON_AddStringToObject(obj, is_sent ? "recipient" : "sender",
                                    e->party);
            cJSON_AddStringToObject(obj, "subject", e->subject);
            cJSON_AddStringToObject(obj, "size", e->size_str);
            cJSON_AddStringToObject(obj, "date", e->date);
            cJSON_AddItemToArray(arr, obj);
        }
        char *out = cJSON_Print(arr);
        printf("%s\n", out);
        free(out);
        cJSON_Delete(arr);
        return;
    }

    /* CSV output */
    if (opts->csv_out) {
        if (!opts->no_header)
            printf("Mail ID,%s,Subject,Size,Date\n",
                   is_sent ? "Recipient" : "Sender");
        for (int i = 0; i < count; i++) {
            shyake_mail_entry *e = &list->entries[i];
            char escaped[2048] = {0};
            int p = 0;
            for (int j = 0; e->subject[j] && p < 2040; j++) {
                if (e->subject[j] == '"') escaped[p++] = '"';
                escaped[p++] = e->subject[j];
            }
            printf("%s,%s,\"%s\",%s,%s\n",
                   e->mail_id, e->party, escaped,
                   e->size_str, e->date);
        }
        return;
    }

    /* Table output */
    if (!opts->json_out && !opts->csv_out)
        cli_setup_pager(opts->plain);

    int w_id  = 7;
    int w_snd = is_sent ? 9 : 6;
    int w_sub = 7;
    int w_sz  = 4;
    int w_dt  = 4;

    for (int i = 0; i < count; i++) {
        int l;
        l = (int)strlen(list->entries[i].mail_id);
        if (l > w_id)  w_id  = l;
        l = (int)strlen(list->entries[i].party);
        if (l > w_snd) w_snd = l;
        l = (int)strlen(list->entries[i].subject);
        if (l > w_sub) w_sub = l;
        l = (int)strlen(list->entries[i].size_str);
        if (l > w_sz)  w_sz  = l;
        l = (int)strlen(list->entries[i].date);
        if (l > w_dt)  w_dt  = l;
    }

    if (!opts->plain) {
        int tw = opts->term_width > 0
            ? opts->term_width : cli_get_terminal_width();
        int max_sub = tw - w_id - w_snd - w_sz - w_dt - 4;
        if (max_sub < 15) max_sub = 15;
        if (w_sub > max_sub) w_sub = max_sub;
    }

    const char *ul_on  = opts->no_color ? "" : "\033[4m";
    const char *ul_off = opts->no_color ? "" : "\033[24m";
    const char *c_w    = opts->no_color ? "" : "\033[39m";
    const char *c_cy   = opts->no_color ? "" : "\033[36m";
    const char *c_mg   = opts->no_color ? "" : "\033[35m";
    const char *c_rs   = opts->no_color ? "" : "\033[0m";

    const char *col2_hdr = is_sent ? "Recipient" : "Sender";

    if (!opts->no_header) {
        if (opts->no_color) {
            printf("%-*s %-*s %-*s %-*s %s\n",
                   w_id,  "Mail ID",
                   w_snd, col2_hdr,
                   w_sub, "Subject",
                   w_sz,  "Size",
                          "Date");
        } else {
            printf("%s", c_rs);
            printf("%s%sMail ID%s%-*s ",
                   c_w, ul_on, ul_off, w_id - 7, "");
            printf("%s%s%s%s%-*s ",
                   c_w, ul_on, col2_hdr, ul_off,
                   w_snd - (int)strlen(col2_hdr), "");
            printf("%s%sSubject%s%-*s ",
                   c_w, ul_on, ul_off, w_sub - 7, "");
            printf("%s%sSize%s%-*s ",
                   c_w, ul_on, ul_off, w_sz - 4, "");
            printf("%s%sDate%s\n", c_w, ul_on, ul_off);
        }
    }

    for (int i = 0; i < count; i++) {
        shyake_mail_entry *e = &list->entries[i];

        char sub_trunc[512];
        int slen = (int)strlen(e->subject);
        if (slen > w_sub && w_sub >= 3)
            snprintf(sub_trunc, sizeof(sub_trunc),
                     "%.*s...", w_sub - 3, e->subject);
        else
            snprintf(sub_trunc, sizeof(sub_trunc), "%s", e->subject);

        const char *c_sz = e->is_large
            ? (opts->no_color ? "" : "\033[1;35m") : c_mg;

        printf("%s%-*s%s ", c_rs, w_id, e->mail_id, c_rs);

        int len_snd = (int)strlen(e->party);
        if (len_snd > 0 && e->party[len_snd - 1] == '@') {
            printf("%s%.*s", c_cy, len_snd - 1, e->party);
            printf("%s@", c_w);
        } else {
            printf("%s%s", c_cy, e->party);
        }
        printf("%s%-*s ", c_rs, w_snd - len_snd, "");

        printf("%s%-*s%s ", c_w, w_sub, sub_trunc, c_rs);
        printf("%s%-*s%s ", c_sz, w_sz, e->size_str, c_rs);
        printf("%s%s%s\n", c_w, e->date, c_rs);
    }

    if (!opts->no_header)
        printf("\nTotal: %d item%s\n", count, count == 1 ? "" : "s");

    cli_wait_pager();
}

/* ------------------------------------------------------------------ */
/* Mail detail (fetch --raw or normal)                                  */
/* ------------------------------------------------------------------ */

void
cli_render_mail_detail(const shyake_mail_detail *d,
                       int raw, int no_color, int plain)
{
    if (!d) return;

    if (raw) {
        if (d->body) printf("%s", d->body);
        return;
    }

    cli_setup_pager(plain);
    const char *c_lbl = no_color ? "" : "\033[1;36m";
    const char *c_val = no_color ? "" : "\033[0m";
    int tw = cli_get_terminal_width();

    const char *sub_text = d->subject ? d->subject : "(decryption failed)";

    printf("%sFROM:%s  %s\n", c_lbl, c_val, d->sender);
    printf("%sTO:%s    %s\n", c_lbl, c_val, d->recipient);
    printf("%sDATE:%s  %s\n", c_lbl, c_val, d->date);
    printf("%sSUBJ:%s  ", c_lbl, c_val);
    cli_print_word_wrap(sub_text, 7, tw);
    printf("\n");

    if (d->body)
        printf("%s\n", d->body);
    else
        printf("(body decryption failed)\n");

    cli_wait_pager();
}

/* ------------------------------------------------------------------ */
/* Mail header view (check <id>)                                        */
/* ------------------------------------------------------------------ */

void
cli_render_mail_header(const shyake_mail_detail *d, int no_color)
{
    if (!d) return;

    const char *c_lbl = no_color ? "" : "\033[1;36m";
    const char *c_rs  = no_color ? "" : "\033[0m";
    int tw = cli_get_terminal_width();

    const char *sub_text = d->subject
        ? d->subject : "(decryption failed)";

    printf("%sFROM:%s %s\n", c_lbl, c_rs, d->sender);
    printf("%sTO:%s   %s\n", c_lbl, c_rs, d->recipient);
    printf("%sSUBJ:%s ", c_lbl, c_rs);
    cli_print_word_wrap(sub_text, 6, tw);
    printf("%sSIZE:%s %d\n", c_lbl, c_rs, d->size);
    printf("%sDATE:%s %s\n", c_lbl, c_rs, d->date);
}
