/*
 * Lexer: turns one command line into a list of tokens, and expands words.
 *
 * Two phases, like a real shell:
 *   1. lex_line()    splits the line into operators and *raw* words
 *                    ("$HOME/'a b'" stays exactly as typed) and reports
 *                    quoting errors.
 *   2. expand_word() removes quotes and substitutes $VAR, ${VAR}, $?, $$
 *                    and a leading ~, right before the command runs.
 *
 * Expansion must be late: in "false; echo $?" the $? has to see the status
 * of `false`, which does not exist yet when the line is parsed.
 *
 * Expansion results are never split on whitespace (a deliberate
 * simplification compared to POSIX sh: "$VAR" and $VAR behave the same,
 * except that an unquoted empty expansion produces no word at all).
 */
#ifndef LSH_LEXER_H
#define LSH_LEXER_H

#include <stddef.h>

typedef enum {
    TOK_WORD,  /* argument or redirection target (raw text)  */
    TOK_PIPE,  /* |                                          */
    TOK_AMP,   /* &                                          */
    TOK_SEMI,  /* ;                                          */
    TOK_AND,   /* &&                                         */
    TOK_OR,    /* ||                                         */
    TOK_REDIR, /* [n]<  [n]>  [n]>>  [n]>&  [n]<&            */
    TOK_EOF,
} tok_type;

typedef enum {
    REDIR_IN,     /* n<file   open for reading                      */
    REDIR_OUT,    /* n>file   open for writing, truncate            */
    REDIR_APPEND, /* n>>file  open for writing, append              */
    REDIR_DUP,    /* n>&m     make fd n a copy of fd m ("-" closes) */
} redir_type;

typedef struct {
    tok_type type;
    char *text;       /* TOK_WORD: the raw word as typed (owned)    */
    int fd;           /* TOK_REDIR: the file descriptor redirected  */
    redir_type rtype; /* TOK_REDIR: kind of redirection             */
    size_t start;     /* byte offset of the token in the source     */
    size_t end;       /* one past the last byte                     */
} token;

typedef struct {
    token *v;
    size_t n;
    size_t cap;
} token_list;

/* Values needed for $? and $$. $NAME is read with getenv(). */
typedef struct {
    int last_status;
    long shell_pid;
} lex_ctx;

/*
 * Tokenize src. On success returns 0 and fills out (always terminated by a
 * TOK_EOF token). On error returns -1, writes a message into err and leaves
 * out empty.
 */
int lex_line(const char *src, token_list *out, char *err, size_t errlen);

void token_list_free(token_list *tl);

/*
 * Expand one raw word. On success returns 0 and stores a malloc'd string in
 * *out, or NULL when the word disappears (unquoted empty expansion such as
 * $UNSET). Returns -1 with a message in err on error.
 */
int expand_word(const char *raw, const lex_ctx *ctx, char **out, char *err, size_t errlen);

#endif
