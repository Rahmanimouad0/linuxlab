/*
 * Unit tests for the shell's lexer and parser. They need no processes, so
 * they run fast and under AddressSanitizer/Valgrind without noise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../shell/src/lexer.h"
#include "../../shell/src/parser.h"

static int tests_run, tests_failed, current_failed;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);             \
            current_failed = 1;                                                                    \
        }                                                                                          \
    } while (0)

#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        const char *a_ = (a), *b_ = (b);                                                           \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                                   \
            fprintf(stderr, "  %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", \
                    b_ ? b_ : "(null)");                                                           \
            current_failed = 1;                                                                    \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        current_failed = 0;                                                                        \
        fn();                                                                                      \
        tests_run++;                                                                               \
        if (current_failed)                                                                        \
            tests_failed++;                                                                        \
        printf("%s  %s\n", current_failed ? "FAIL" : "ok  ", #fn);                                 \
    } while (0)

static const lex_ctx ctx = {7, 4242};

/* Parse src and expect success. */
static cmd_list parse_ok(const char *src)
{
    cmd_list l;
    char err[256] = "";
    if (parse_line(src, &l, err, sizeof err) != 0) {
        fprintf(stderr, "  unexpected parse error for \"%s\": %s\n", src, err);
        current_failed = 1;
        memset(&l, 0, sizeof l);
    }
    return l;
}

/* Parse src and expect an error containing `expect`. */
static void parse_fail(const char *src, const char *expect)
{
    cmd_list l;
    char err[256] = "";
    if (parse_line(src, &l, err, sizeof err) == 0) {
        fprintf(stderr, "  expected parse error for \"%s\"\n", src);
        current_failed = 1;
        cmd_list_free(&l);
        return;
    }
    if (!strstr(err, expect)) {
        fprintf(stderr, "  error \"%s\" does not contain \"%s\"\n", err, expect);
        current_failed = 1;
    }
}

/* Words of the first command of the first pipeline. */
static command *first_cmd(cmd_list *l)
{
    if (l->n == 0 || l->items[0].pl.ncmds == 0)
        return NULL;
    return &l->items[0].pl.cmds[0];
}

/* ------------------------------------------------------------------ lexer */

static void test_lex_operators(void)
{
    token_list tl;
    char err[128];
    CHECK(lex_line("a|b||c&d&&e;f", &tl, err, sizeof err) == 0);
    tok_type want[] = {TOK_WORD, TOK_PIPE, TOK_WORD, TOK_OR,   TOK_WORD, TOK_AMP,
                       TOK_WORD, TOK_AND,  TOK_WORD, TOK_SEMI, TOK_WORD, TOK_EOF};
    CHECK(tl.n == sizeof want / sizeof want[0]);
    for (size_t i = 0; i < tl.n && i < sizeof want / sizeof want[0]; i++)
        CHECK(tl.v[i].type == want[i]);
    token_list_free(&tl);
}

static void test_lex_redirections(void)
{
    token_list tl;
    char err[128];
    CHECK(lex_line("<a >b >>c 2>d 2>&1 3<&0", &tl, err, sizeof err) == 0);
    /* REDIR WORD pairs */
    CHECK(tl.n == 13);
    if (tl.n == 13) {
        CHECK(tl.v[0].type == TOK_REDIR && tl.v[0].rtype == REDIR_IN && tl.v[0].fd == 0);
        CHECK(tl.v[2].rtype == REDIR_OUT && tl.v[2].fd == 1);
        CHECK(tl.v[4].rtype == REDIR_APPEND && tl.v[4].fd == 1);
        CHECK(tl.v[6].rtype == REDIR_OUT && tl.v[6].fd == 2);
        CHECK(tl.v[8].rtype == REDIR_DUP && tl.v[8].fd == 2);
        CHECK_STR(tl.v[9].text, "1");
        CHECK(tl.v[10].rtype == REDIR_DUP && tl.v[10].fd == 3);
    }
    token_list_free(&tl);
}

static void test_lex_digit_word_not_redirect(void)
{
    /* "abc2>f" is the word abc2 followed by a stdout redirection. */
    cmd_list l = parse_ok("echo abc2>f 42");
    command *c = first_cmd(&l);
    CHECK(c && c->argc == 3);
    if (c && c->argc == 3) {
        CHECK_STR(c->argv[1], "abc2");
        CHECK_STR(c->argv[2], "42");
        CHECK(c->nredirs == 1 && c->redirs[0].fd == 1);
    }
    cmd_list_free(&l);
}

/*
 * Expand the words of the first command like the executor does.
 * Returns how many words survived; they are stored in w[] (free_words()).
 */
static size_t expand_first(cmd_list *l, char **w, size_t max)
{
    command *c = first_cmd(l);
    size_t n = 0;
    char err[128];
    if (!c)
        return 0;
    for (size_t i = 0; i < c->argc && n < max; i++) {
        char *word;
        if (expand_word(c->argv[i], &ctx, &word, err, sizeof err) < 0) {
            fprintf(stderr, "  expand_word(\"%s\") failed: %s\n", c->argv[i], err);
            current_failed = 1;
            continue;
        }
        if (word)
            w[n++] = word;
    }
    return n;
}

static void free_words(char **w, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(w[i]);
}

static void test_words_stay_raw_until_expansion(void)
{
    /* The parser keeps words exactly as typed; expansion happens later. */
    cmd_list l = parse_ok("echo \"$HOME\" 'a b'");
    command *c = first_cmd(&l);
    CHECK(c && c->argc == 3);
    if (c && c->argc == 3) {
        CHECK_STR(c->argv[1], "\"$HOME\"");
        CHECK_STR(c->argv[2], "'a b'");
    }
    cmd_list_free(&l);
}

static void test_lex_quotes(void)
{
    char *w[8];
    cmd_list l = parse_ok("echo 'a  b' \"c  d\" e\\ f \"x\\\"y\" 'it''s'");
    size_t n = expand_first(&l, w, 8);
    CHECK(n == 6);
    if (n == 6) {
        CHECK_STR(w[1], "a  b");
        CHECK_STR(w[2], "c  d");
        CHECK_STR(w[3], "e f");
        CHECK_STR(w[4], "x\"y");
        CHECK_STR(w[5], "its");
    }
    free_words(w, n);
    cmd_list_free(&l);
}

static void test_lex_metachars_in_quotes(void)
{
    char *w[8];
    cmd_list l = parse_ok("echo 'a|b' \"c;d\" e\\&f");
    CHECK(l.n == 1 && l.items[0].pl.ncmds == 1);
    size_t n = expand_first(&l, w, 8);
    CHECK(n == 4);
    if (n == 4) {
        CHECK_STR(w[1], "a|b");
        CHECK_STR(w[2], "c;d");
        CHECK_STR(w[3], "e&f");
    }
    free_words(w, n);
    cmd_list_free(&l);
}

static void test_lex_variables(void)
{
    char *w[8];
    setenv("LSH_TEST_VAR", "value", 1);
    cmd_list l = parse_ok("echo $LSH_TEST_VAR ${LSH_TEST_VAR}x \"$LSH_TEST_VAR\" '$LSH_TEST_VAR'");
    size_t n = expand_first(&l, w, 8);
    CHECK(n == 5);
    if (n == 5) {
        CHECK_STR(w[1], "value");
        CHECK_STR(w[2], "valuex");
        CHECK_STR(w[3], "value");
        CHECK_STR(w[4], "$LSH_TEST_VAR");
    }
    free_words(w, n);
    cmd_list_free(&l);
}

static void test_lex_special_params(void)
{
    char *w[8];
    cmd_list l = parse_ok("echo $? $$ $ a$");
    size_t n = expand_first(&l, w, 8);
    CHECK(n == 5);
    if (n == 5) {
        CHECK_STR(w[1], "7");    /* ctx.last_status */
        CHECK_STR(w[2], "4242"); /* ctx.shell_pid   */
        CHECK_STR(w[3], "$");
        CHECK_STR(w[4], "a$");
    }
    free_words(w, n);
    cmd_list_free(&l);
}

static void test_lex_empty_expansion(void)
{
    char *w[8];
    unsetenv("LSH_UNSET_VAR");
    cmd_list l = parse_ok("printf x $LSH_UNSET_VAR \"$LSH_UNSET_VAR\" ''");
    size_t n = expand_first(&l, w, 8);
    /* the unquoted empty expansion disappears, quoted ones stay as "" */
    CHECK(n == 4);
    if (n == 4) {
        CHECK_STR(w[2], "");
        CHECK_STR(w[3], "");
    }
    free_words(w, n);
    cmd_list_free(&l);
}

static void test_lex_tilde(void)
{
    char *w[8];
    setenv("HOME", "/home/tester", 1);
    cmd_list l = parse_ok("echo ~ ~/x a~ '~' ~user");
    size_t n = expand_first(&l, w, 8);
    CHECK(n == 6);
    if (n == 6) {
        CHECK_STR(w[1], "/home/tester");
        CHECK_STR(w[2], "/home/tester/x");
        CHECK_STR(w[3], "a~");
        CHECK_STR(w[4], "~");
        CHECK_STR(w[5], "~user");
    }
    free_words(w, n);
    cmd_list_free(&l);
}

static void test_lex_comments(void)
{
    cmd_list l = parse_ok("echo hi # this is ignored | nope");
    command *c = first_cmd(&l);
    CHECK(l.n == 1 && c && c->argc == 2);
    cmd_list_free(&l);

    l = parse_ok("   # only a comment");
    CHECK(l.n == 0);
    cmd_list_free(&l);
}

static void test_lex_errors(void)
{
    parse_fail("echo 'abc", "unterminated single quote");
    parse_fail("echo \"abc", "unterminated double quote");
    parse_fail("echo ${", "bad substitution");
    parse_fail("echo ${1abc}", "bad substitution");
}

/* ----------------------------------------------------------------- parser */

static void test_parse_empty(void)
{
    cmd_list l = parse_ok("   \t ");
    CHECK(l.n == 0);
    cmd_list_free(&l);
}

static void test_parse_pipeline(void)
{
    cmd_list l = parse_ok("cat file.txt | grep hello | wc -l");
    CHECK(l.n == 1);
    if (l.n == 1) {
        pipeline *p = &l.items[0].pl;
        CHECK(p->ncmds == 3);
        CHECK(!p->background);
        CHECK_STR(p->text, "cat file.txt | grep hello | wc -l");
        if (p->ncmds == 3) {
            CHECK_STR(p->cmds[0].argv[0], "cat");
            CHECK_STR(p->cmds[1].argv[1], "hello");
            CHECK_STR(p->cmds[2].argv[1], "-l");
            CHECK(p->cmds[2].argv[2] == NULL);
        }
    }
    cmd_list_free(&l);
}

static void test_parse_redirections(void)
{
    cmd_list l = parse_ok("sort < in.txt > out.txt 2>> err.log");
    command *c = first_cmd(&l);
    CHECK(c && c->argc == 1 && c->nredirs == 3);
    if (c && c->nredirs == 3) {
        CHECK(c->redirs[0].type == REDIR_IN && c->redirs[0].fd == 0);
        CHECK_STR(c->redirs[0].target, "in.txt");
        CHECK(c->redirs[1].type == REDIR_OUT && c->redirs[1].fd == 1);
        CHECK_STR(c->redirs[1].target, "out.txt");
        CHECK(c->redirs[2].type == REDIR_APPEND && c->redirs[2].fd == 2);
        CHECK_STR(c->redirs[2].target, "err.log");
    }
    cmd_list_free(&l);
}

static void test_parse_redirection_anywhere(void)
{
    cmd_list l = parse_ok(">out echo a >>log b");
    command *c = first_cmd(&l);
    CHECK(c && c->argc == 3 && c->nredirs == 2);
    if (c && c->argc == 3) {
        CHECK_STR(c->argv[0], "echo");
        CHECK_STR(c->argv[2], "b");
    }
    cmd_list_free(&l);
}

static void test_parse_redirect_only(void)
{
    cmd_list l = parse_ok("> newfile");
    command *c = first_cmd(&l);
    CHECK(c && c->argc == 0 && c->nredirs == 1 && c->argv && c->argv[0] == NULL);
    cmd_list_free(&l);
}

static void test_parse_lists(void)
{
    cmd_list l = parse_ok("a && b || c ; d & e");
    CHECK(l.n == 5);
    if (l.n == 5) {
        CHECK(l.items[0].next == NEXT_AND);
        CHECK(l.items[1].next == NEXT_OR);
        CHECK(l.items[2].next == NEXT_SEQ);
        CHECK(l.items[3].next == NEXT_SEQ && l.items[3].pl.background);
        CHECK(l.items[4].next == NEXT_END && !l.items[4].pl.background);
    }
    cmd_list_free(&l);
}

static void test_parse_background(void)
{
    cmd_list l = parse_ok("sleep 10 &");
    CHECK(l.n == 1);
    if (l.n == 1) {
        CHECK(l.items[0].pl.background);
        CHECK_STR(l.items[0].pl.text, "sleep 10");
    }
    cmd_list_free(&l);

    l = parse_ok("echo done;");
    CHECK(l.n == 1 && l.items[0].next == NEXT_END);
    cmd_list_free(&l);
}

static void test_parse_syntax_errors(void)
{
    parse_fail("| ls", "`|'");
    parse_fail("ls |", "`newline'");
    parse_fail("ls | | wc", "`|'");
    parse_fail("ls >", "`newline'");
    parse_fail("ls > | wc", "`|'");
    parse_fail("a &&", "`newline'");
    parse_fail("a || ; b", "`;'");
    parse_fail(";", "`;'");
    parse_fail("a & & b", "`&'");
    parse_fail("echo 2>&x", "ambiguous redirect");
}

static void test_parse_dup_close(void)
{
    cmd_list l = parse_ok("cmd 2>&- 1>&2");
    command *c = first_cmd(&l);
    CHECK(c && c->nredirs == 2);
    if (c && c->nredirs == 2) {
        CHECK(c->redirs[0].type == REDIR_DUP && c->redirs[0].fd == 2);
        CHECK_STR(c->redirs[0].target, "-");
        CHECK(c->redirs[1].type == REDIR_DUP && c->redirs[1].fd == 1);
        CHECK_STR(c->redirs[1].target, "2");
    }
    cmd_list_free(&l);
}

static void test_parse_many_args(void)
{
    /* Exercise argv growth well past the initial capacity. */
    char src[4096] = "cmd";
    for (int i = 0; i < 200; i++)
        strcat(src, " x");
    cmd_list l = parse_ok(src);
    command *c = first_cmd(&l);
    CHECK(c && c->argc == 201 && c->argv[201] == NULL);
    cmd_list_free(&l);
}

int main(void)
{
    RUN(test_lex_operators);
    RUN(test_lex_redirections);
    RUN(test_lex_digit_word_not_redirect);
    RUN(test_words_stay_raw_until_expansion);
    RUN(test_lex_quotes);
    RUN(test_lex_metachars_in_quotes);
    RUN(test_lex_variables);
    RUN(test_lex_special_params);
    RUN(test_lex_empty_expansion);
    RUN(test_lex_tilde);
    RUN(test_lex_comments);
    RUN(test_lex_errors);
    RUN(test_parse_empty);
    RUN(test_parse_pipeline);
    RUN(test_parse_redirections);
    RUN(test_parse_redirection_anywhere);
    RUN(test_parse_redirect_only);
    RUN(test_parse_lists);
    RUN(test_parse_background);
    RUN(test_parse_syntax_errors);
    RUN(test_parse_dup_close);
    RUN(test_parse_many_args);

    printf("\nparser unit tests: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
