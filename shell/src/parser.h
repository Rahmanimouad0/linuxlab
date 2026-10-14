/*
 * Parser: builds a small AST from the token list.
 *
 *   line     := pipeline ( ('&&' | '||' | ';' | '&') pipeline )* [';' | '&']
 *   pipeline := command ( '|' command )*
 *   command  := ( WORD | REDIR WORD )+
 *
 * Example:  cat < in.txt | grep x > out.txt && echo ok &
 *
 *   cmd_list
 *   ├── item 0: pipeline [cat <in.txt] | [grep x >out.txt]   next = AND
 *   └── item 1: pipeline [echo ok]  (background)              next = SEQ
 */
#ifndef LSH_PARSER_H
#define LSH_PARSER_H

#include <stddef.h>

#include "lexer.h"

typedef struct {
    redir_type type;
    int fd;       /* fd being redirected (0 for <, 1 for >) */
    char *target; /* file name, or fd number / "-" for REDIR_DUP */
} redirection;

/*
 * Words and redirection targets are stored raw (quotes and $VAR intact);
 * the executor expands them with expand_word() just before running.
 */
typedef struct {
    char **argv; /* NULL-terminated; argc may be 0 for "> file" */
    size_t argc;
    redirection *redirs; /* applied left to right */
    size_t nredirs;
} command;

typedef struct {
    command *cmds;
    size_t ncmds;
    int background; /* ended with '&' */
    char *text;     /* source text, used for job listings */
} pipeline;

/* How the pipeline relates to the one that follows it. */
typedef enum {
    NEXT_END, /* last item                              */
    NEXT_SEQ, /* ';' or '&': always run the next one    */
    NEXT_AND, /* '&&': run the next one only if status == 0 */
    NEXT_OR,  /* '||': run the next one only if status != 0 */
} next_op;

typedef struct {
    pipeline pl;
    next_op next;
} list_item;

typedef struct {
    list_item *items;
    size_t n;
} cmd_list;

/*
 * Parse one command line. Returns 0 on success (an empty line yields
 * n == 0), or -1 with a message in err on a lexing or syntax error.
 */
int parse_line(const char *src, cmd_list *out, char *err, size_t errlen);

void command_free(command *c);

void cmd_list_free(cmd_list *l);

#endif
