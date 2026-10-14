#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

typedef struct {
    const token_list *tl;
    size_t pos;
    const char *src;
    char *err;
    size_t errlen;
} parser;

static const token *peek(const parser *p) { return &p->tl->v[p->pos]; }

static int syntax_error(parser *p, const token *t)
{
    if (t->type == TOK_EOF)
        snprintf(p->err, p->errlen, "syntax error near unexpected token `newline'");
    else
        snprintf(p->err, p->errlen, "syntax error near unexpected token `%.*s'",
                 (int)(t->end - t->start), p->src + t->start);
    return -1;
}

/* The target of n>&m must be a descriptor number or "-". */
static int valid_dup_target(const char *s)
{
    if (strcmp(s, "-") == 0)
        return 1;
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

static int parse_command(parser *p, command *c)
{
    size_t argcap = 0, rcap = 0;

    for (;;) {
        const token *t = peek(p);
        if (t->type == TOK_WORD) {
            if (c->argc + 2 > argcap) {
                argcap = argcap ? argcap * 2 : 8;
                c->argv = xrealloc(c->argv, argcap * sizeof *c->argv);
            }
            c->argv[c->argc++] = xstrdup(t->text);
            c->argv[c->argc] = NULL;
            p->pos++;
        } else if (t->type == TOK_REDIR) {
            /* Safe: the list always ends with TOK_EOF, never with a REDIR. */
            const token *target = &p->tl->v[p->pos + 1];
            if (target->type != TOK_WORD)
                return syntax_error(p, target);
            if (t->rtype == REDIR_DUP && !valid_dup_target(target->text)) {
                snprintf(p->err, p->errlen, "%s: ambiguous redirect", target->text);
                return -1;
            }
            if (c->nredirs == rcap) {
                rcap = rcap ? rcap * 2 : 4;
                c->redirs = xrealloc(c->redirs, rcap * sizeof *c->redirs);
            }
            c->redirs[c->nredirs].type = t->rtype;
            c->redirs[c->nredirs].fd = t->fd;
            c->redirs[c->nredirs].target = xstrdup(target->text);
            c->nredirs++;
            p->pos += 2;
        } else {
            break;
        }
    }

    if (c->argc == 0 && c->nredirs == 0)
        return syntax_error(p, peek(p));
    if (!c->argv)
        c->argv = xcalloc(1, sizeof *c->argv); /* {NULL} */
    return 0;
}

static int parse_pipeline(parser *p, pipeline *pl)
{
    size_t cap = 0;
    size_t first = peek(p)->start;

    for (;;) {
        if (pl->ncmds == cap) {
            cap = cap ? cap * 2 : 4;
            pl->cmds = xrealloc(pl->cmds, cap * sizeof *pl->cmds);
        }
        command *c = &pl->cmds[pl->ncmds++];
        memset(c, 0, sizeof *c);
        if (parse_command(p, c) < 0)
            return -1;
        if (peek(p)->type != TOK_PIPE)
            break;
        p->pos++;
    }

    size_t last_end = p->tl->v[p->pos - 1].end;
    pl->text = xstrndup(p->src + first, last_end - first);
    return 0;
}

int parse_line(const char *src, cmd_list *out, char *err, size_t errlen)
{
    token_list tl;
    size_t cap = 0;

    memset(out, 0, sizeof *out);
    if (lex_line(src, &tl, err, errlen) < 0)
        return -1;

    parser p = {&tl, 0, src, err, errlen};

    while (peek(&p)->type != TOK_EOF) {
        if (out->n == cap) {
            cap = cap ? cap * 2 : 4;
            out->items = xrealloc(out->items, cap * sizeof *out->items);
        }
        list_item *it = &out->items[out->n++];
        memset(it, 0, sizeof *it);
        it->next = NEXT_END;

        if (parse_pipeline(&p, &it->pl) < 0)
            goto fail;

        const token *t = peek(&p);
        switch (t->type) {
        case TOK_EOF: break;
        case TOK_SEMI:
            it->next = NEXT_SEQ;
            p.pos++;
            break;
        case TOK_AMP:
            it->pl.background = 1;
            it->next = NEXT_SEQ;
            p.pos++;
            break;
        case TOK_AND:
        case TOK_OR:
            it->next = t->type == TOK_AND ? NEXT_AND : NEXT_OR;
            p.pos++;
            if (peek(&p)->type == TOK_EOF) {
                syntax_error(&p, peek(&p));
                goto fail;
            }
            break;
        default: syntax_error(&p, t); goto fail;
        }
    }

    if (out->n > 0)
        out->items[out->n - 1].next = NEXT_END;
    token_list_free(&tl);
    return 0;

fail:
    token_list_free(&tl);
    cmd_list_free(out);
    return -1;
}

void command_free(command *c)
{
    for (size_t i = 0; i < c->argc; i++)
        free(c->argv[i]);
    free(c->argv);
    for (size_t i = 0; i < c->nredirs; i++)
        free(c->redirs[i].target);
    free(c->redirs);
}

void cmd_list_free(cmd_list *l)
{
    for (size_t i = 0; i < l->n; i++) {
        pipeline *pl = &l->items[i].pl;
        for (size_t j = 0; j < pl->ncmds; j++)
            command_free(&pl->cmds[j]);
        free(pl->cmds);
        free(pl->text);
    }
    free(l->items);
    l->items = NULL;
    l->n = 0;
}
