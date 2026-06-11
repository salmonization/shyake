#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <openssl/sha.h>
#include "../lib/vendor/cJSON/cJSON.h"
#include "display.h"

/* ------------------------------------------------------------------ */
/* UTF-8 display width helpers                                        */
/* ------------------------------------------------------------------ */

/* decode one UTF-8 codepoint; advance *s; return codepoint or -1 */
static int
utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    if (!*p) return -1;
    int cp, n;
    if (*p < 0x80)  { cp = *p;            n = 1; } else
    if (*p < 0xC2)  { (*s)++; return 0; } /* continuation / invalid */
    else if (*p < 0xE0) { cp = *p & 0x1F; n = 2; } else
    if (*p < 0xF0)  { cp = *p & 0x0F; n = 3; } else
    if (*p < 0xF8)  { cp = *p & 0x07; n = 4; }
    else            { (*s)++; return 0; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { (*s)++; return 0; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return cp;
}

/* return terminal display width of a single Unicode codepoint */
static int
cp_width(int cp)
{
    if (cp <= 0) return 0;
    /* CJK Unified Ideographs and common wide blocks */
    if ((cp >= 0x1100  && cp <= 0x115F)  || /* Hangul Jamo */
        (cp == 0x2329 || cp == 0x232A)   ||
        (cp >= 0x2E80  && cp <= 0x303E)  || /* CJK Radicals, etc. */
        (cp >= 0x3040  && cp <= 0x33FF)  || /* Hiragana..CJK Compat */
        (cp >= 0x3400  && cp <= 0x4DBF)  || /* CJK Ext-A */
        (cp >= 0x4E00  && cp <= 0xA4CF)  || /* CJK Unified */
        (cp >= 0xA960  && cp <= 0xA97F)  || /* Hangul Jamo Ext-A */
        (cp >= 0xAC00  && cp <= 0xD7FF)  || /* Hangul Syllables */
        (cp >= 0xF900  && cp <= 0xFAFF)  || /* CJK Compat Ideographs */
        (cp >= 0xFE10  && cp <= 0xFE1F)  || /* Vertical forms */
        (cp >= 0xFE30  && cp <= 0xFE6F)  || /* CJK Compat Forms */
        (cp >= 0xFF00  && cp <= 0xFF60)  || /* Fullwidth */
        (cp >= 0xFFE0  && cp <= 0xFFE6)  ||
        (cp >= 0x1B000 && cp <= 0x1B0FF) || /* Kana Supplement */
        (cp >= 0x1F004 && cp <= 0x1F0CF) || /* Mahjong/Playing cards */
        (cp >= 0x1F300 && cp <= 0x1F9FF) || /* Misc symbols & Emoji */
        (cp >= 0x20000 && cp <= 0x2FFFD) || /* CJK Ext-B..F */
        (cp >= 0x30000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

/* return display width of a UTF-8 string */
static int
utf8_display_width(const char *s)
{
    int w = 0;
    while (*s) {
        int cp = utf8_next(&s);
        if (cp > 0) w += cp_width(cp);
    }
    return w;
}

/*
 * Truncate UTF-8 string so its display width <= max_w.
 * Writes into buf (size buf_sz).  Appends ellipsis (".." or "..."
 * depending on available space) when truncation actually occurs.
 * Returns the display width of the result written.
 */
static int
utf8_truncate(const char *src, char *buf, int buf_sz, int max_w)
{
    const char *ellipsis = "...";
    int ew = 3;
    if (max_w < 3) { buf[0] = '\0'; return 0; }

    int total_w = utf8_display_width(src);
    if (total_w <= max_w) {
        int len = (int)strlen(src);
        if (len >= buf_sz) len = buf_sz - 1;
        memcpy(buf, src, len);
        buf[len] = '\0';
        return total_w;
    }

    /* scan forward, accumulate width up to (max_w - ew) */
    int limit_w = max_w - ew;
    int accum   = 0;
    const char *p = src;
    const char *last_safe = src;
    while (*p) {
        int cp = utf8_next(&p);
        int cw = cp > 0 ? cp_width(cp) : 0;
        if (accum + cw > limit_w) break;
        accum    += cw;
        last_safe = p;
    }
    int head = (int)(last_safe - src);
    int elen = (int)strlen(ellipsis);
    if (head + elen >= buf_sz) head = buf_sz - elen - 1;
    if (head < 0) head = 0;
    memcpy(buf, src, head);
    memcpy(buf + head, ellipsis, elen);
    buf[head + elen] = '\0';
    return accum + ew;
}

/* ------------------------------------------------------------------ */
/* Pager                                                              */
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
/* Terminal width                                                     */
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
/* Word wrap                                                          */
/* ------------------------------------------------------------------ */

void
cli_print_word_wrap(const char *text, int indent, int width)
{
    // word-wrap text with UTF-8 display width awareness
    int avail = width - indent;
    if (avail < 20) avail = 20;
    int first = 1;
    const char *p = text;
    while (*p) {
        if (!first) printf("\n%*s", indent, "");
        first = 0;
        const char *line_start = p;
        const char *last_space = NULL;
        const char *safe_cut = p;
        int accum = 0;
        while (*p) {
            const char *before = p;
            int cp = utf8_next(&p);
            if (cp == ' ') {
                last_space = before;
            }
            int cw = cp > 0 ? cp_width(cp) : 0;
            if (accum + cw > avail) {
                p = before; // rewind to just before this codepoint
                break;
            }
            accum += cw;
            safe_cut = p;
        }
        if (*p == '\0') {
            printf("%.*s", (int)(safe_cut - line_start), line_start);
            break;
        }
        if (last_space != NULL && last_space > line_start) {
            printf("%.*s", (int)(last_space - line_start), line_start);
            p = last_space;
            while (*p == ' ') p++;
        } else {
            printf("%.*s", (int)(safe_cut - line_start), line_start);
            p = safe_cut;
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Timestamp formatting with timezone support                         */
/* ------------------------------------------------------------------ */

void
cli_format_timestamp(int64_t ts, int tz_hours,
                     const char *fmt, const char *fmt_recent,
                     char *buf, int buf_len)
{
    // convert UNIX ts to broken-down time with optional tz offset
    const char *f = fmt ? fmt : "%Y-%m-%d %H:%M";
    time_t t = (time_t)ts;
    struct tm tmbuf;
    struct tm *tmi;
    if (tz_hours == TZ_AUTO) {
        tmi = localtime_r(&t, &tmbuf);
    } else {
        /* apply manual UTC offset */
        time_t shifted = t + (time_t)tz_hours * 3600;
        tmi = gmtime_r(&shifted, &tmbuf);
    }
    if (!tmi) {
        snprintf(buf, buf_len, "?");
        return;
    }
    if (fmt_recent) {
        time_t now = time(NULL);
        if (now - t < 180 * 24 * 3600)
            f = fmt_recent;
    }
    strftime(buf, buf_len, f, tmi);
}

/* ------------------------------------------------------------------ */
/* Party / size display helpers                                        */
/* ------------------------------------------------------------------ */

void
cli_format_party(const char *party, char *buf, int buf_len)
{
    // for remote parties keep username@ only
    const char *at = strchr(party, '@');
    if (at) {
        int name_len = (int)(at - party);
        snprintf(buf, buf_len, "%.*s@", name_len, party);
    } else {
        snprintf(buf, buf_len, "%s", party);
    }
}

void
cli_format_size(int size, char *buf, int buf_len)
{
    if (size >= 1024)
        snprintf(buf, buf_len, "%dK", size / 1024);
    else
        snprintf(buf, buf_len, "%d", size);
}

/* ------------------------------------------------------------------ */
/* Fingerprint display                                                */
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
            if (x < 0) x = 0;
            if (x > 16) x = 16;
            if (y < 0) y = 0;
            if (y > 8)  y = 8;
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
    (void)label;
    if (is_self) {
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
            printf("\nStatus: MISMATCH (Warning: Key has changed)\n\n"
                   "Please verify the new fingerprint in a secondary, "
                   "trusted out-of-band channel\n"
                   "before running the update command "
                   "('shyake fingerprint <username> --update').\n");
    } else {
        printf("\n[Remote server]\n");
        cli_print_fingerprint_hex(r->remote_fp);
        cli_print_randomart(r->remote_fp);
        printf("\nStatus: NEW HOST (No local key found)\n");
    }
}

/* ------------------------------------------------------------------ */
/* Mail list rendering                                                */
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

    /* Pre-format all entries into display strings */
    char **d_party   = calloc(count, sizeof(char *));
    char **d_subject = calloc(count, sizeof(char *));
    char **d_size    = calloc(count, sizeof(char *));
    char **d_date    = calloc(count, sizeof(char *));

    for (int i = 0; i < count; i++) {
        shyake_mail_entry *e = &list->entries[i];

        char pbuf[256];
        cli_format_party(e->party, pbuf, sizeof(pbuf));
        d_party[i] = strdup(pbuf);

        d_subject[i] = e->subject ? strdup(e->subject)
                                  : strdup("<decryption failed>");

        char sbuf[16];
        cli_format_size(e->size, sbuf, sizeof(sbuf));
        d_size[i] = strdup(sbuf);

        char dbuf[64];
        cli_format_timestamp(e->timestamp, opts->tz_hours,
                             opts->time_fmt, opts->time_fmt_recent,
                             dbuf, sizeof(dbuf));
        d_date[i] = strdup(dbuf);
    }

    /* JSON output */
    if (opts->json_out) {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            shyake_mail_entry *e = &list->entries[i];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "mail_id", e->mail_id);
            cJSON_AddStringToObject(obj,
                is_sent ? "recipient" : "sender", d_party[i]);
            cJSON_AddStringToObject(obj, "subject", d_subject[i]);
            cJSON_AddStringToObject(obj, "size", d_size[i]);
            cJSON_AddStringToObject(obj, "date", d_date[i]);
            cJSON_AddItemToArray(arr, obj);
        }
        char *out = cJSON_PrintUnformatted(arr);
        printf("%s\n", out);
        free(out);
        cJSON_Delete(arr);
        goto cleanup;
    }

    /* CSV output */
    if (opts->csv_out) {
        if (!opts->no_header)
            printf("Mail ID,%s,Subject,Size,Date\n",
                   is_sent ? "To" : "From");
        for (int i = 0; i < count; i++) {
            shyake_mail_entry *e = &list->entries[i];
            char escaped[2048] = {0};
            int p = 0;
            for (int j = 0; d_subject[i][j] && p < 2040; j++) {
                if (d_subject[i][j] == '"') escaped[p++] = '"';
                escaped[p++] = d_subject[i][j];
            }
            printf("%s,%s,\"%s\",%s,%s\n",
                   e->mail_id, d_party[i], escaped,
                   d_size[i], d_date[i]);
        }
        goto cleanup;
    }

    /* Table output */
    cli_setup_pager(opts->plain);

    /* build active column list; default to all if col_count == 0 */
    static const int def_order[] = {
        COL_ID, COL_PARTY, COL_SUBJECT, COL_SIZE, COL_DATE
    };
    const int *order = (opts->col_count > 0)
        ? opts->col_order : def_order;
    int ncols = (opts->col_count > 0) ? opts->col_count : 5;

    /* build active-column bitmask for fast membership test */
    unsigned int active = 0;
    for (int c = 0; c < ncols; c++) active |= (unsigned)order[c];

    /* column widths */
    int w_id  = 7;
    int w_snd = is_sent ? 2 : 4;
    int w_sub = 7;
    int w_sz  = 4;
    int w_dt  = 4;

    for (int i = 0; i < count; i++) {
        int l;
        if (active & COL_ID) {
            l = (int)strlen(list->entries[i].mail_id);
            if (l > w_id)  w_id  = l;
        }
        if (active & COL_PARTY) {
            l = (int)strlen(d_party[i]);
            if (l > w_snd) w_snd = l;
        }
        if (active & COL_SUBJECT) {
            l = utf8_display_width(d_subject[i]);
            if (l > w_sub) w_sub = l;
        }
        if (active & COL_SIZE) {
            l = (int)strlen(d_size[i]);
            if (l > w_sz)  w_sz  = l;
        }
        if (active & COL_DATE) {
            l = (int)strlen(d_date[i]);
            if (l > w_dt)  w_dt  = l;
        }
    }

    if (!opts->plain && (active & COL_SUBJECT)) {
        int tw = opts->term_width > 0
            ? opts->term_width : cli_get_terminal_width();
        int used = 0;
        if (active & COL_ID)    used += w_id  + 1;
        if (active & COL_PARTY) used += w_snd + 1;
        if (active & COL_SIZE)  used += w_sz  + 1;
        if (active & COL_DATE)  used += w_dt;
        int max_sub = tw - used - 1;
        if (max_sub < 15) max_sub = 15;
        if (w_sub > max_sub) w_sub = max_sub;
    }

    const char *ul_on  = opts->no_color ? "" : "\033[4m";
    const char *ul_off = opts->no_color ? "" : "\033[24m";
    const char *c_w    = opts->no_color ? "" : "\033[39m";
    const char *c_cy   = opts->no_color ? "" : "\033[36m";
    const char *c_mg   = opts->no_color ? "" : "\033[35m";
    const char *c_rs   = opts->no_color ? "" : "\033[0m";

    const char *col2_hdr = is_sent ? "To" : "From";

    /* header row: iterate col_order */
    if (!opts->no_header) {
        if (!opts->no_color) printf("%s", c_rs);
        for (int c = 0; c < ncols; c++) {
            int is_last = (c == ncols - 1);
            switch (order[c]) {
            case COL_ID:
                if (opts->no_color)
                    printf("%-*s%s", w_id, "Mail ID",
                           is_last ? "" : " ");
                else
                    printf("%s%sMail ID%s%-*s%s",
                           c_w, ul_on, ul_off, w_id - 7, "",
                           is_last ? "" : " ");
                break;
            case COL_PARTY:
                if (opts->no_color)
                    printf("%-*s%s", w_snd, col2_hdr,
                           is_last ? "" : " ");
                else
                    printf("%s%s%s%s%-*s%s",
                           c_w, ul_on, col2_hdr, ul_off,
                           w_snd - (int)strlen(col2_hdr), "",
                           is_last ? "" : " ");
                break;
            case COL_SUBJECT:
                if (opts->no_color)
                    printf("%-*s%s", w_sub, "Subject",
                           is_last ? "" : " ");
                else
                    printf("%s%sSubject%s%-*s%s",
                           c_w, ul_on, ul_off, w_sub - 7, "",
                           is_last ? "" : " ");
                break;
            case COL_SIZE:
                if (opts->no_color)
                    printf("%-*s%s", w_sz, "Size",
                           is_last ? "" : " ");
                else
                    printf("%s%sSize%s%-*s%s",
                           c_w, ul_on, ul_off, w_sz - 4, "",
                           is_last ? "" : " ");
                break;
            case COL_DATE:
                if (opts->no_color)
                    printf("%-*s%s", w_dt, "Date",
                           is_last ? "" : " ");
                else
                    printf("%s%sDate%s%-*s%s",
                           c_w, ul_on, ul_off, w_dt - 4, "",
                           is_last ? "" : " ");
                break;
            default: break;
            }
        }
        printf("\n");
    }

    /* data rows: iterate col_order */
    for (int i = 0; i < count; i++) {
        shyake_mail_entry *e = &list->entries[i];
        int is_large = (e->size >= 1024);
        const char *c_sz = is_large
            ? (opts->no_color ? "" : "\033[1;35m") : c_mg;

        for (int c = 0; c < ncols; c++) {
            int is_last = (c == ncols - 1);
            switch (order[c]) {
            case COL_ID:
                printf("%s%-*s%s%s", c_rs, w_id, e->mail_id, c_rs,
                       is_last ? "" : " ");
                break;
            case COL_PARTY: {
                int len_snd = (int)strlen(d_party[i]);
                if (len_snd > 0 && d_party[i][len_snd - 1] == '@') {
                    printf("%s%.*s%s@",
                           c_cy, len_snd - 1, d_party[i], c_w);
                } else {
                    printf("%s%s", c_cy, d_party[i]);
                }
                printf("%s%-*s%s", c_rs, w_snd - len_snd, "",
                       is_last ? "" : " ");
                break;
            }
            case COL_SUBJECT: {
                char sub_trunc[512];
                int dw = utf8_truncate(
                    d_subject[i], sub_trunc,
                    sizeof(sub_trunc), w_sub);
                /* pad to w_sub with spaces (byte printf won't) */
                printf("%s%s%*s%s%s",
                       c_w, sub_trunc,
                       w_sub - dw, "",
                       c_rs, is_last ? "" : " ");
                break;
            }
            case COL_SIZE:
                printf("%s%-*s%s%s", c_sz, w_sz, d_size[i], c_rs,
                       is_last ? "" : " ");
                break;
            case COL_DATE:
                printf("%s%-*s%s%s", c_w, w_dt, d_date[i], c_rs,
                       is_last ? "" : " ");
                break;
            default: break;
            }
        }
        printf("\n");
    }

    if (!opts->no_header)
        printf("\nTotal: %d item%s\n", count, count == 1 ? "" : "s");

    cli_wait_pager();

cleanup:
    for (int i = 0; i < count; i++) {
        free(d_party[i]);
        free(d_subject[i]);
        free(d_size[i]);
        free(d_date[i]);
    }
    free(d_party);
    free(d_subject);
    free(d_size);
    free(d_date);
}

/* ------------------------------------------------------------------ */
/* Mail detail (fetch --raw or normal)                                */
/* ------------------------------------------------------------------ */

void
cli_render_mail_detail(const shyake_mail_detail *d,
                       int raw, int no_color, int plain,
                       int tz_hours,
                       const char *time_fmt,
                       const char *time_fmt_recent)
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

    char date_buf[64];
    cli_format_timestamp(d->timestamp, tz_hours,
                         time_fmt, time_fmt_recent,
                         date_buf, sizeof(date_buf));

    const char *sub_text = d->subject ? d->subject : "<decryption failed>";

    printf("%sFROM:%s  %s\n", c_lbl, c_val, d->sender);
    printf("%sTO:%s    %s\n", c_lbl, c_val, d->recipient);
    printf("%sDATE:%s  %s\n", c_lbl, c_val, date_buf);
    printf("%sSUBJ:%s  ", c_lbl, c_val);
    cli_print_word_wrap(sub_text, 7, tw);
    printf("\n");

    if (d->body) {
        printf("%s", d->body);
        if (isatty(STDOUT_FILENO) || saved_stdout >= 0) {
            printf("\n");
        }
    } else {
        printf("<body decryption failed>\n");
    }

    cli_wait_pager();
}

/* ------------------------------------------------------------------ */
/* Mail header view (check <id>)                                      */
/* ------------------------------------------------------------------ */

void
cli_render_mail_header(const shyake_mail_detail *d,
                       int no_color, int tz_hours,
                       const char *time_fmt,
                       const char *time_fmt_recent)
{
    if (!d) return;

    const char *c_lbl = no_color ? "" : "\033[1;36m";
    const char *c_rs  = no_color ? "" : "\033[0m";
    int tw = cli_get_terminal_width();

    char date_buf[64];
    cli_format_timestamp(d->timestamp, tz_hours,
                         time_fmt, time_fmt_recent,
                         date_buf, sizeof(date_buf));

    const char *sub_text = d->subject
        ? d->subject : "(decryption failed)";

    printf("%sFROM:%s %s\n", c_lbl, c_rs, d->sender);
    printf("%sTO:%s   %s\n", c_lbl, c_rs, d->recipient);
    printf("%sSUBJ:%s ", c_lbl, c_rs);
    cli_print_word_wrap(sub_text, 6, tw);
    printf("%sSIZE:%s %d\n", c_lbl, c_rs, d->size);
    printf("%sDATE:%s %s\n", c_lbl, c_rs, date_buf);
}
