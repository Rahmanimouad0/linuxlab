#include "lexer.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

/* A growable, always NUL-terminated string. */
typedef struct {
    char *s;
    size_t len;
    size_t cap;
} strbuf;

static void sb_putc(strbuf *b, char c)
{
    if (b->len + 2 > b->cap) {
        size_t cap = b->cap ? b->cap : 32;
        while (cap < b->len + 2)
            cap *= 2;
        b->s = xrealloc(b->s, cap);
        b->cap = cap;
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

static void sb_puts(strbuf *b, const char *s)
{
    while (*s)
        sb_putc(b, *s++);
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int is_meta(char c) { return c == '|' || c == '&' || c == ';' || c == '<' || c == '>'; }
static int is_name_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void push(token_list *tl, token t)
{
    if (tl->n == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 16;
        tl->v = xrealloc(tl->v, tl->cap * sizeof *tl->v);
    }
    tl->v[tl->n++] = t;
}

void token_list_free(token_list *tl)
{
    for (size_t i = 0; i < tl->n; i++)
        free(tl->v[i].text);
    free(tl->v);
    tl->v = NULL;
    tl->n = tl->cap = 0;
}

/*
 * Expand the $-reference at src[*i] (which is '$') into out.
 * Supports $?, $$, $NAME and ${NAME}. A '$' not followed by one of those is
 * kept literally, like in sh.
 */
static int expand_dollar(const char *src, size_t *i, const lex_ctx *ctx, strbuf *out, char *err,
                         size_t errlen)
{
    size_t j = *i + 1;
    char num[32];

    if (src[j] == '?') {
        snprintf(num, sizeof num, "%d", ctx->last_status);
        sb_puts(out, num);
        *i = j + 1;
        return 0;
    }
    if (src[j] == '$') {
        snprintf(num, sizeof num, "%ld", ctx->shell_pid);
        sb_puts(out, num);
        *i = j + 1;
        return 0;
    }

    int braced = src[j] == '{';
    if (braced)
        j++;
    size_t name_start = j;
    if (is_name_start(src[j]))
        while (is_name_char(src[j]))
            j++;
    size_t name_len = j - name_start;

    if (braced && (name_len == 0 || src[j] != '}')) {
        set_err(err, errlen, "bad substitution");
        return -1;
    }
    if (name_len == 0) { /* lone '$' */
        sb_putc(out, '$');
        *i += 1;
        return 0;
    }

    char *name = xstrndup(src + name_start, name_len);
    const char *val = getenv(name);
    free(name);
    if (val)
        sb_puts(out, val);
    *i = braced ? j + 1 : j;
    return 0;
}

/*
 * Read one word starting at src[*pos]. Sets *quoted if any quoting was used,
 * which distinguishes "" (an empty argument) from an empty expansion (no
 * argument at all).
 */
static int read_word(const char *src, size_t *pos, const lex_ctx *ctx, strbuf *w, int *quoted,
                     char *err, size_t errlen)
{
    size_t i = *pos;
    *quoted = 0;

    while (src[i] && !is_blank(src[i]) && !is_meta(src[i])) {
        char c = src[i];
        if (c == '\\') {
            /* Backslash quotes the next character; a trailing one is dropped. */
            if (src[i + 1]) {
                sb_putc(w, src[i + 1]);
                *quoted = 1;
                i += 2;
            } else {
                i++;
            }
        } else if (c == '\'') {
            /* Single quotes: everything literal until the next quote. */
            const char *close = strchr(src + i + 1, '\'');
            if (!close) {
                set_err(err, errlen, "unterminated single quote");
                return -1;
            }
            for (const char *p = src + i + 1; p < close; p++)
                sb_putc(w, *p);
            *quoted = 1;
            i = (size_t)(close - src) + 1;
        } else if (c == '"') {
            /* Double quotes: literal except for $ and a few backslash escapes. */
            *quoted = 1;
            i++;
            while (src[i] && src[i] != '"') {
                if (src[i] == '\\' && src[i + 1] && strchr("\"\\$`", src[i + 1])) {
                    sb_putc(w, src[i + 1]);
                    i += 2;
                } else if (src[i] == '$') {
                    if (expand_dollar(src, &i, ctx, w, err, errlen) < 0)
                        return -1;
                } else {
                    sb_putc(w, src[i++]);
                }
            }
            if (!src[i]) {
                set_err(err, errlen, "unterminated double quote");
                return -1;
            }
            i++;
        } else if (c == '$') {
            if (expand_dollar(src, &i, ctx, w, err, errlen) < 0)
                return -1;
        } else if (c == '~' && i == *pos &&
                   (src[i + 1] == '/' || src[i + 1] == '\0' || is_blank(src[i + 1]) ||
                    is_meta(src[i + 1]))) {
            /* Tilde expansion, only for a leading unquoted "~" or "~/". */
            const char *home = getenv("HOME");
            sb_puts(w, home ? home : "~");
            i++;
        } else {
            sb_putc(w, c);
            i++;
        }
    }
    *pos = i;
    return 0;
}

/* Lex the operator part of a redirection starting at src[*i] ('<' or '>'). */
static void lex_redir(const char *src, size_t *i, token *t)
{
    t->type = TOK_REDIR;
    if (src[*i] == '<') {
        (*i)++;
        if (src[*i] == '&') {
            t->rtype = REDIR_DUP;
            (*i)++;
        } else {
            t->rtype = REDIR_IN;
        }
        return;
    }
    (*i)++;
    if (src[*i] == '>') {
        t->rtype = REDIR_APPEND;
        (*i)++;
    } else if (src[*i] == '&') {
        t->rtype = REDIR_DUP;
        (*i)++;
    } else {
        t->rtype = REDIR_OUT;
    }
}

int lex_line(const char *src, token_list *out, char *err, size_t errlen)
{
    size_t i = 0;
    memset(out, 0, sizeof *out);

    for (;;) {
        while (is_blank(src[i]))
            i++;

        token t;
        memset(&t, 0, sizeof t);
        t.start = i;

        if (src[i] == '\0' || src[i] == '#') { /* end of line or comment */
            t.type = TOK_EOF;
            t.end = i;
            push(out, t);
            return 0;
        }

        /* "2>", "10<" ...: digits immediately followed by < or > name an fd. */
        size_t j = i;
        while (isdigit((unsigned char)src[j]))
            j++;

        if (j > i && j - i <= 4 && (src[j] == '<' || src[j] == '>')) {
            t.fd = atoi(src + i);
            i = j;
            lex_redir(src, &i, &t);
        } else if (src[i] == '<' || src[i] == '>') {
            t.fd = src[i] == '<' ? 0 : 1;
            lex_redir(src, &i, &t);
        } else if (src[i] == '|') {
            t.type = src[i + 1] == '|' ? TOK_OR : TOK_PIPE;
            i += t.type == TOK_OR ? 2 : 1;
        } else if (src[i] == '&') {
            t.type = src[i + 1] == '&' ? TOK_AND : TOK_AMP;
            i += t.type == TOK_AND ? 2 : 1;
        } else if (src[i] == ';') {
            t.type = TOK_SEMI;
            i++;
        } else {
            /* Scan the word with a dummy context: this finds where it ends
             * and reports quoting errors now, but the real expansion waits
             * until the command is about to run. */
            static const lex_ctx scan_ctx = {0, 0};
            strbuf scratch = {NULL, 0, 0};
            int quoted;
            int rc = read_word(src, &i, &scan_ctx, &scratch, &quoted, err, errlen);
            free(scratch.s);
            if (rc < 0) {
                token_list_free(out);
                return -1;
            }
            t.type = TOK_WORD;
            t.text = xstrndup(src + t.start, i - t.start);
        }
        t.end = i;
        push(out, t);
    }
}

int expand_word(const char *raw, const lex_ctx *ctx, char **out, char *err, size_t errlen)
{
    strbuf w = {NULL, 0, 0};
    size_t i = 0;
    int quoted;

    *out = NULL;
    if (read_word(raw, &i, ctx, &w, &quoted, err, errlen) < 0) {
        free(w.s);
        return -1;
    }
    if (w.len == 0 && !quoted) { /* unquoted empty expansion: the word vanishes */
        free(w.s);
        return 0;
    }
    *out = w.s ? w.s : xstrdup("");
    return 0;
}
