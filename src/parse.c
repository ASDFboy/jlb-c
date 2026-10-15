// Builds the syntax tree from tokens.

#include "jlb.h"

typedef struct {
    Token *t;
    size_t n;
    size_t p;
    int loops;
    int funcs;
    SyntaxError *err;
} Parser;

static const Op ASSIGN_OPS[] = {OP_SET, OP_SHARE_SET, OP_ADD_SET, OP_SUB_SET, OP_MUL_SET, OP_DIV_SET, OP_POW_SET, OP_ROOT_SET, OP_REM_SET, OP_QUOT_SET};

static const Op LEVELS[][5] = {
    {OP_OR, OP_ORS, OP_NOR, OP_NORS, OP_NONE},
    {OP_XOR, OP_XNOR, OP_NONE},
    {OP_AND, OP_ANDS, OP_NAND, OP_NANDS, OP_NONE},
    {OP_EQ2, OP_EQ3, OP_EQ2N, OP_EQ3N, OP_NONE},
    {OP_LT, OP_GT, OP_LE, OP_GE, OP_LE2},
    {OP_ADD, OP_SUB, OP_NONE},
    {OP_MUL, OP_DIV, OP_REM, OP_QUOT, OP_NONE},
};
#define NLEVELS 7

enum {
    KW_IF, KW_ELIF, KW_ELSEIF, KW_ELSE, KW_WHILE, KW_REPEAT, KW_FOR, KW_IN, KW_TO, KW_STEP, KW_BREAK, KW_SKIP,
    KW_FUNC, KW_RETURN, KW_LOCAL, KW_TRY, KW_FAIL, KW_GOTO, KW_POINT, KW_IMPORT, KW_LOAD, KW_END, KW_TRUE,
    KW_FALSE, KW_NULL, KW_UNDEFINE, KW_INSERT, KW_DROP
};

static bool in_level(int lvl, Op o) {
    // The comparison level has six operators.
    if (lvl == 4) return o == OP_LT || o == OP_GT || o == OP_LE || o == OP_GE || o == OP_LE2 || o == OP_GE2;
    for (int i = 0; i < 5 && LEVELS[lvl][i] != OP_NONE; i++)
        if (LEVELS[lvl][i] == o) return true;
    return false;
}

static void *node(size_t n) {
    return xcalloc(1, n);
}

static char *describe(const Token *t) {
    Buf b;
    buf_init(&b);
    switch (t->k) {
    case T_NUM: buf_puts(&b, "a number"); break;
    case T_STR: buf_puts(&b, "some text"); break;
    case T_NAME: buf_printf(&b, "`%s`", t->s->s); break;
    case T_KW: buf_puts(&b, WORDS[t->kw]); break;
    case T_OP: buf_printf(&b, "`%s`", OP_TEXT[t->op]); break;
    case T_NEWLINE: buf_puts(&b, "the end of the line"); break;
    case T_EOF: buf_puts(&b, "the end of the file"); break;
    }
    return buf_take(&b);
}

static Token *peek(Parser *p) {
    return &p->t[p->p];
}
static Token *peek_at(Parser *p, size_t k) {
    size_t i = p->p + k;
    if (i > p->n - 1) i = p->n - 1;
    return &p->t[i];
}
static int line_of(Parser *p) {
    return p->t[p->p].line;
}
static Token *advance(Parser *p) {
    Token *t = &p->t[p->p];
    if (p->p + 1 < p->n) p->p++;
    return t;
}
static bool is_op(Parser *p, Op o) {
    return peek(p)->k == T_OP && peek(p)->op == o;
}
static bool is_kw(Parser *p, int k) {
    return peek(p)->k == T_KW && peek(p)->kw == k;
}

static bool pfail(Parser *p, const char *fmt, ...) {
    Buf b;
    buf_init(&b);
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(&b, fmt, ap);
    va_end(ap);
    p->err->msg = buf_take(&b);
    p->err->line = line_of(p);
    p->err->pos = (long)p->t[p->p].pos;
    return false;
}

static bool expect_op(Parser *p, Op o) {
    if (is_op(p, o)) {
        advance(p);
        return true;
    }
    char *d = describe(peek(p));
    pfail(p, "expected `%s` but found %s", OP_TEXT[o], d);
    free(d);
    return false;
}

static Str *expect_name(Parser *p, const char *what) {
    if (peek(p)->k == T_NAME) {
        Str *s = str_ref(peek(p)->s);
        advance(p);
        return s;
    }
    char *d = describe(peek(p));
    pfail(p, "expected %s but found %s", what, d);
    free(d);
    return NULL;
}

static void skip_seps(Parser *p) {
    while (peek(p)->k == T_NEWLINE || is_op(p, OP_SEMI)) advance(p);
}

static bool at_stmt_end(Parser *p) {
    TokKind k = peek(p)->k;
    return k == T_NEWLINE || k == T_EOF || is_op(p, OP_SEMI) || is_op(p, OP_RBRACE);
}

static void block_push(Block *b, Stmt *s) {
    b->s = xrealloc(b->s, (size_t)(b->n + 1) * sizeof(Stmt *));
    b->s[b->n++] = s;
}

static Stmt *statement(Parser *p);
static Expr *expr(Parser *p);
static FuncDecl *func_rest(Parser *p, Str *name);
static bool check_points(Parser *p, const Block *b);

bool is_lvalue(const Expr *e) {
    return e->k == E_VAR || e->k == E_FIELD || e->k == E_INDEX;
}

static bool program(Parser *p, Block *out) {
    memset(out, 0, sizeof *out);
    skip_seps(p);
    while (peek(p)->k != T_EOF) {
        if (is_op(p, OP_RBRACE)) return pfail(p, "there's a } here without a matching {");
        Stmt *s = statement(p);
        if (!s) return false;
        block_push(out, s);
        if (!at_stmt_end(p)) return pfail(p, "two statements on one line need a ; between them");
        skip_seps(p);
    }
    return check_points(p, out);
}

static bool block(Parser *p, Block *out) {
    memset(out, 0, sizeof *out);
    if (!is_op(p, OP_LBRACE)) {
        if (is_op(p, OP_SET)) return pfail(p, "= assigns a value; use == to compare");
        char *d = describe(peek(p));
        pfail(p, "expected { to start a block but found %s", d);
        free(d);
        return false;
    }
    int open = line_of(p);
    size_t open_pos = p->t[p->p].pos;
    advance(p);
    skip_seps(p);
    while (!is_op(p, OP_RBRACE)) {
        if (peek(p)->k == T_EOF) {
            p->err->msg = xstrdup("this block is never closed with }");
            p->err->line = open;
            p->err->pos = (long)open_pos;
            return false;
        }
        Stmt *s = statement(p);
        if (!s) return false;
        block_push(out, s);
        if (!at_stmt_end(p)) return pfail(p, "two statements on one line need a ; between them");
        skip_seps(p);
    }
    advance(p);
    return true;
}

static bool loop_body(Parser *p, Block *out) {
    p->loops++;
    bool ok = block(p, out);
    p->loops--;
    return ok;
}

static Stmt *new_stmt(StmtKind k, int line) {
    Stmt *s = node(sizeof(Stmt));
    s->k = k;
    s->line = line;
    return s;
}

static bool if_stmt(Parser *p, Stmt *s) {
    advance(p);
    Expr *c = expr(p);
    if (!c) return false;
    Block b;
    if (!block(p, &b)) return false;
    s->conds = xmalloc(sizeof(Expr *));
    s->blocks = xmalloc(sizeof(Block));
    s->conds[0] = c;
    s->blocks[0] = b;
    s->nconds = 1;
    for (;;) {
        size_t save = p->p;
        skip_seps(p);
        bool more = false;
        if (is_kw(p, KW_ELIF) || is_kw(p, KW_ELSEIF)) {
            advance(p);
            more = true;
        } else if (is_kw(p, KW_ELSE)) {
            advance(p);
            if (is_kw(p, KW_IF)) {
                advance(p);
                more = true;
            } else {
                if (!block(p, &s->body2)) return false;
                s->has_else = true;
                break;
            }
        } else {
            p->p = save;
            break;
        }
        if (more) {
            Expr *c2 = expr(p);
            if (!c2) return false;
            Block b2;
            if (!block(p, &b2)) return false;
            s->conds = xrealloc(s->conds, (size_t)(s->nconds + 1) * sizeof(Expr *));
            s->blocks = xrealloc(s->blocks, (size_t)(s->nconds + 1) * sizeof(Block));
            s->conds[s->nconds] = c2;
            s->blocks[s->nconds] = b2;
            s->nconds++;
        }
    }
    return true;
}

static bool loop_var(Parser *p, Str **name, bool *shared) {
    *shared = false;
    if (is_op(p, OP_AT)) {
        advance(p);
        *shared = true;
    }
    *name = expect_name(p, "a loop variable name");
    return *name != NULL;
}

static bool for_stmt(Parser *p, Stmt *s) {
    advance(p);
    if (!loop_var(p, &s->name, &s->shared)) return false;
    if (is_op(p, OP_COMMA)) {
        advance(p);
        if (!loop_var(p, &s->name2, &s->shared2)) return false;
        s->has2 = true;
    }
    if (!is_kw(p, KW_IN)) {
        char *d = describe(peek(p));
        pfail(p, "expected IN but found %s", d);
        free(d);
        return false;
    }
    advance(p);
    s->e = expr(p);
    if (!s->e) return false;
    if (is_kw(p, KW_TO)) {
        if (s->has2) return pfail(p, "a counting loop (FOR i IN 1 TO 10) takes just one variable");
        advance(p);
        s->e2 = expr(p);
        if (!s->e2) return false;
        if (is_kw(p, KW_STEP)) {
            advance(p);
            s->e3 = expr(p);
            if (!s->e3) return false;
        }
        s->k = S_FOR_TO;
        return loop_body(p, &s->body);
    }
    s->k = S_FOR_IN;
    return loop_body(p, &s->body);
}

static Stmt *expr_or_assign(Parser *p, int line) {
    Expr *e = expr(p);
    if (!e) return NULL;
    if (peek(p)->k == T_OP) {
        Op o = peek(p)->op;
        for (size_t i = 0; i < sizeof ASSIGN_OPS / sizeof ASSIGN_OPS[0]; i++) {
            if (ASSIGN_OPS[i] != o) continue;
            if (!is_lvalue(e)) {
                pfail(p, "the left side of %s must be a variable, a table entry or a position", OP_TEXT[o]);
                return NULL;
            }
            advance(p);
            Expr *rhs = expr(p);
            if (!rhs) return NULL;
            Stmt *s = new_stmt(S_ASSIGN, line);
            s->e = e;
            s->op = o;
            s->e2 = rhs;
            return s;
        }
    }
    Stmt *s = new_stmt(S_EXPR, line);
    s->e = e;
    return s;
}

static Stmt *statement(Parser *p) {
    int line = line_of(p);
    Token *t = peek(p);
    if (t->k != T_KW) return expr_or_assign(p, line);
    int k = t->kw;
    Stmt *s;
    switch (k) {
    case KW_IF:
        s = new_stmt(S_IF, line);
        return if_stmt(p, s) ? s : NULL;
    case KW_WHILE:
    case KW_REPEAT:
        s = new_stmt(k == KW_WHILE ? S_WHILE : S_REPEAT, line);
        advance(p);
        s->e = expr(p);
        if (!s->e) return NULL;
        return loop_body(p, &s->body) ? s : NULL;
    case KW_FOR:
        s = new_stmt(S_FOR_IN, line);
        return for_stmt(p, s) ? s : NULL;
    case KW_FUNC:
        if (peek_at(p, 1)->k != T_NAME) return expr_or_assign(p, line);
        {
            advance(p);
            Str *name = expect_name(p, "a function name");
            if (!name) return NULL;
            FuncDecl *d = func_rest(p, name);
            if (!d) return NULL;
            s = new_stmt(S_FUNC, line);
            s->fn = d;
            return s;
        }
    case KW_RETURN:
        if (p->funcs == 0) {
            pfail(p, "RETURN can only be used inside a function");
            return NULL;
        }
        advance(p);
        s = new_stmt(S_RETURN, line);
        if (!at_stmt_end(p)) {
            s->e = expr(p);
            if (!s->e) return NULL;
        }
        return s;
    case KW_BREAK:
    case KW_SKIP:
        if (p->loops == 0) {
            pfail(p, "%s can only be used inside a loop", WORDS[k]);
            return NULL;
        }
        advance(p);
        return new_stmt(k == KW_BREAK ? S_BREAK : S_SKIP, line);
    case KW_END:
        advance(p);
        return new_stmt(S_END, line);
    case KW_LOCAL:
        if (p->funcs == 0) {
            pfail(p, "LOCAL can only be used inside a function");
            return NULL;
        }
        advance(p);
        s = new_stmt(S_LOCAL, line);
        s->name = expect_name(p, "a variable name after LOCAL");
        if (!s->name) return NULL;
        if (is_op(p, OP_SET)) {
            advance(p);
            s->e = expr(p);
            if (!s->e) return NULL;
        }
        return s;
    case KW_TRY: {
        advance(p);
        s = new_stmt(S_TRY, line);
        if (!block(p, &s->body)) return NULL;
        size_t save = p->p;
        skip_seps(p);
        if (!is_kw(p, KW_FAIL)) {
            p->p = save;
            pfail(p, "TRY needs a FAIL block after it");
            return NULL;
        }
        advance(p);
        s->name = expect_name(p, "a name for the error after FAIL");
        if (!s->name) return NULL;
        if (!block(p, &s->body2)) return NULL;
        return s;
    }
    case KW_GOTO:
    case KW_POINT: {
        advance(p);
        Buf w;
        buf_init(&w);
        buf_printf(&w, "a point name after %s", WORDS[k]);
        Str *name = expect_name(p, w.p);
        buf_free(&w);
        if (!name) return NULL;
        s = new_stmt(k == KW_GOTO ? S_GOTO : S_POINT, line);
        s->name = name;
        return s;
    }
    case KW_IMPORT:
    case KW_LOAD: {
        advance(p);
        Token *f = advance(p);
        if (f->k != T_STR) {
            char *d = describe(f);
            pfail(p, "%s needs a file name in quotes, not %s", WORDS[k], d);
            free(d);
            return NULL;
        }
        s = new_stmt(k == KW_IMPORT ? S_IMPORT : S_LOAD, line);
        s->name = str_ref(f->s);
        return s;
    }
    case KW_ELIF:
    case KW_ELSEIF:
    case KW_ELSE:
        pfail(p, "%s without an IF before it", WORDS[k]);
        return NULL;
    case KW_FAIL:
        pfail(p, "FAIL without a TRY before it");
        return NULL;
    default:
        return expr_or_assign(p, line);
    }
}

static FuncDecl *func_rest(Parser *p, Str *name) {
    if (!expect_op(p, OP_LPAREN)) return NULL;
    FuncDecl *d = node(sizeof(FuncDecl));
    d->name = name;
    while (!is_op(p, OP_RPAREN)) {
        bool rest = false, shared = false;
        if (is_op(p, OP_DOTS)) {
            advance(p);
            rest = true;
        }
        if (is_op(p, OP_AT)) {
            advance(p);
            shared = true;
        }
        Str *pname = expect_name(p, "an input name");
        if (!pname) return NULL;
        Expr *def = NULL;
        if (is_op(p, OP_SET)) {
            advance(p);
            def = expr(p);
            if (!def) return NULL;
        }
        bool any_rest = false, any_def = false;
        for (int i = 0; i < d->nparams; i++) {
            if (d->params[i].rest) any_rest = true;
            if (d->params[i].def) any_def = true;
        }
        if (any_rest) {
            pfail(p, "an input with ... must be the last one");
            return NULL;
        }
        for (int i = 0; i < d->nparams; i++) {
            if (str_eq(d->params[i].name, pname)) {
                pfail(p, "the input %s is listed twice", pname->s);
                return NULL;
            }
        }
        if (rest && def) {
            pfail(p, "an input with ... can't have a default");
            return NULL;
        }
        if (!rest && !def && any_def) {
            pfail(p, "required inputs must come before optional ones");
            return NULL;
        }
        d->params = xrealloc(d->params, (size_t)(d->nparams + 1) * sizeof(Param));
        Param *pr = &d->params[d->nparams++];
        pr->name = pname;
        pr->shared = shared;
        pr->rest = rest;
        pr->def = def;
        if (is_op(p, OP_COMMA)) {
            advance(p);
        } else if (!is_op(p, OP_RPAREN)) {
            char *ds = describe(peek(p));
            pfail(p, "expected , or ) but found %s", ds);
            free(ds);
            return NULL;
        }
    }
    advance(p);
    int sl = p->loops, sf = p->funcs;
    p->loops = 0;
    p->funcs++;
    bool ok = block(p, &d->body);
    p->loops = sl;
    p->funcs = sf;
    if (!ok) return NULL;
    if (!check_points(p, &d->body)) return NULL;
    return d;
}

// ---------- expressions ----------

static Expr *new_expr(ExprKind k) {
    Expr *e = node(sizeof(Expr));
    e->k = k;
    return e;
}

static Expr *unary(Parser *p);

static Expr *bin_level(Parser *p, int lvl) {
    if (lvl == NLEVELS) return unary(p);
    Expr *left = bin_level(p, lvl + 1);
    if (!left) return NULL;
    for (;;) {
        Token *t = peek(p);
        if (t->k != T_OP || !in_level(lvl, t->op)) break;
        Op op = t->op;
        advance(p);
        Expr *right = bin_level(p, lvl + 1);
        if (!right) return NULL;
        Expr *e = new_expr(E_BINARY);
        e->op = op;
        e->x = left;
        e->y = right;
        left = e;
    }
    return left;
}

static Expr *expr(Parser *p) {
    return bin_level(p, 0);
}

static Expr *postfix(Parser *p);

static Expr *power(Parser *p) {
    Expr *base = postfix(p);
    if (!base) return NULL;
    Token *t = peek(p);
    if (t->k != T_OP || (t->op != OP_POW && t->op != OP_ROOT)) return base;
    Op op = t->op;
    advance(p);
    Expr *rhs = unary(p);
    if (!rhs) return NULL;
    Expr *e = new_expr(E_BINARY);
    e->op = op;
    e->x = base;
    e->y = rhs;
    return e;
}

static Expr *unary(Parser *p) {
    if (is_op(p, OP_SUB)) {
        advance(p);
        Expr *e = unary(p);
        if (!e) return NULL;
        if (e->k == E_NUM) {
            Number n = num_neg(e->num);
            num_release(e->num);
            e->num = n;
            return e;
        }
        Expr *u = new_expr(E_NEG);
        u->x = e;
        return u;
    }
    if (is_op(p, OP_NOT)) {
        advance(p);
        Expr *e = unary(p);
        if (!e) return NULL;
        Expr *u = new_expr(E_NOT);
        u->x = e;
        return u;
    }
    return power(p);
}

static bool args(Parser *p, Expr ***out, int *n) {
    if (!expect_op(p, OP_LPAREN)) return false;
    *out = NULL;
    *n = 0;
    while (!is_op(p, OP_RPAREN)) {
        Expr *e = expr(p);
        if (!e) return false;
        *out = xrealloc(*out, (size_t)(*n + 1) * sizeof(Expr *));
        (*out)[(*n)++] = e;
        if (is_op(p, OP_COMMA)) {
            advance(p);
        } else if (!is_op(p, OP_RPAREN)) {
            char *d = describe(peek(p));
            pfail(p, "expected , or ) but found %s", d);
            free(d);
            return false;
        }
    }
    advance(p);
    return true;
}

static Expr *primary(Parser *p);

static Expr *postfix(Parser *p) {
    Expr *e = primary(p);
    if (!e) return NULL;
    for (;;) {
        if (is_op(p, OP_LPAREN)) {
            Expr *c = new_expr(E_CALL);
            c->x = e;
            if (!args(p, &c->args, &c->nargs)) return NULL;
            e = c;
        } else if (is_op(p, OP_DOT)) {
            advance(p);
            Str *n = expect_name(p, "a name after .");
            if (!n) return NULL;
            Expr *f = new_expr(E_FIELD);
            f->x = e;
            f->name = n;
            e = f;
        } else if (is_op(p, OP_LBRACK)) {
            advance(p);
            Expr *a = expr(p);
            if (!a) return NULL;
            if (is_kw(p, KW_TO)) {
                advance(p);
                Expr *b = expr(p);
                if (!b) return NULL;
                Expr *step = NULL;
                if (is_kw(p, KW_STEP)) {
                    advance(p);
                    step = expr(p);
                    if (!step) return NULL;
                }
                if (!expect_op(p, OP_RBRACK)) return NULL;
                Expr *s = new_expr(E_SLICE);
                s->x = e;
                s->y = a;
                s->z = b;
                s->w = step;
                e = s;
            } else {
                if (!expect_op(p, OP_RBRACK)) return NULL;
                Expr *ix = new_expr(E_INDEX);
                ix->x = e;
                ix->y = a;
                e = ix;
            }
        } else {
            break;
        }
    }
    return e;
}

static Expr *table_literal(Parser *p) {
    advance(p);
    Expr *e = new_expr(E_TABLE);
    while (!is_op(p, OP_RBRACK)) {
        if (peek(p)->k == T_EOF) {
            pfail(p, "this table is never closed with ]");
            return NULL;
        }
        Str *key = NULL;
        Token *a = peek(p), *b = peek_at(p, 1);
        if ((a->k == T_NAME || a->k == T_STR) && b->k == T_OP && b->op == OP_COLON) {
            key = str_ref(a->s);
            advance(p);
            advance(p);
        }
        Expr *v = expr(p);
        if (!v) return NULL;
        e->args = xrealloc(e->args, (size_t)(e->nargs + 1) * sizeof(Expr *));
        e->keys = xrealloc(e->keys, (size_t)(e->nargs + 1) * sizeof(Str *));
        e->args[e->nargs] = v;
        e->keys[e->nargs] = key;
        e->nargs++;
        if (is_op(p, OP_COMMA)) {
            advance(p);
        } else if (!is_op(p, OP_RBRACK)) {
            char *d = describe(peek(p));
            pfail(p, "expected , or ] in the table but found %s", d);
            free(d);
            return NULL;
        }
    }
    advance(p);
    return e;
}

static Expr *primary(Parser *p) {
    Token *t = peek(p);
    Expr *e;
    switch (t->k) {
    case T_NUM:
        e = new_expr(E_NUM);
        e->num = num_retain(t->num);
        advance(p);
        return e;
    case T_STR:
        e = new_expr(E_TEXT);
        e->name = str_ref(t->s);
        advance(p);
        return e;
    case T_NAME:
        e = new_expr(E_VAR);
        e->name = str_ref(t->s);
        advance(p);
        return e;
    case T_OP:
        if (t->op == OP_LPAREN) {
            advance(p);
            e = expr(p);
            if (!e) return NULL;
            if (!expect_op(p, OP_RPAREN)) return NULL;
            return e;
        }
        if (t->op == OP_LBRACK) return table_literal(p);
        break;
    case T_KW: {
        int k = t->kw;
        switch (k) {
        case KW_TRUE:
        case KW_FALSE:
            advance(p);
            e = new_expr(E_BOOL);
            e->b = k == KW_TRUE;
            return e;
        case KW_NULL:
            advance(p);
            return new_expr(E_NULL);
        case KW_FUNC:
            advance(p);
            e = new_expr(E_FUNC);
            e->fn = func_rest(p, NULL);
            return e->fn ? e : NULL;
        case KW_UNDEFINE: {
            advance(p);
            Expr **a;
            int n;
            if (!args(p, &a, &n)) return NULL;
            if (n != 1 || !is_lvalue(a[0])) {
                pfail(p, "UNDEFINE needs one name, like UNDEFINE(score) or UNDEFINE(player.score)");
                return NULL;
            }
            e = new_expr(E_UNDEFINE);
            e->x = a[0];
            free(a);
            return e;
        }
        case KW_INSERT:
        case KW_DROP:
            advance(p);
            e = new_expr(k == KW_INSERT ? E_INSERT : E_DROP);
            return args(p, &e->args, &e->nargs) ? e : NULL;
        default:
            if (k >= N_KEYWORDS) {
                advance(p);
                e = new_expr(E_BUILTIN);
                e->bi = k;
                return e;
            }
            pfail(p, "%s can't be used here", WORDS[k]);
            return NULL;
        }
    }
    default: break;
    }
    char *d = describe(t);
    pfail(p, "expected a value but found %s", d);
    free(d);
    return NULL;
}

// ---------- POINT names ----------

typedef struct {
    Str **names;
    int *lines;
    int n;
} Seen;

static bool walk_points(Parser *p, const Block *b, Seen *seen) {
    for (int i = 0; i < b->n; i++) {
        Stmt *s = b->s[i];
        switch (s->k) {
        case S_POINT:
            for (int j = 0; j < seen->n; j++) {
                if (str_eq(seen->names[j], s->name)) {
                    Buf m;
                    buf_init(&m);
                    buf_printf(&m, "POINT %s is already used on line %d", s->name->s, seen->lines[j]);
                    p->err->msg = buf_take(&m);
                    p->err->line = s->line;
                    p->err->pos = -1;
                    return false;
                }
            }
            seen->names = xrealloc(seen->names, (size_t)(seen->n + 1) * sizeof(Str *));
            seen->lines = xrealloc(seen->lines, (size_t)(seen->n + 1) * sizeof(int));
            seen->names[seen->n] = s->name;
            seen->lines[seen->n] = s->line;
            seen->n++;
            break;
        case S_IF:
            for (int j = 0; j < s->nconds; j++)
                if (!walk_points(p, &s->blocks[j], seen)) return false;
            if (s->has_else && !walk_points(p, &s->body2, seen)) return false;
            break;
        case S_WHILE:
        case S_REPEAT:
        case S_FOR_IN:
        case S_FOR_TO:
            if (!walk_points(p, &s->body, seen)) return false;
            break;
        case S_TRY:
            if (!walk_points(p, &s->body, seen) || !walk_points(p, &s->body2, seen)) return false;
            break;
        default: break;
        }
    }
    return true;
}

/// POINT names must be unique within one function (or one file's top level).
static bool check_points(Parser *p, const Block *b) {
    Seen seen = {NULL, NULL, 0};
    bool ok = walk_points(p, b, &seen);
    free(seen.names);
    free(seen.lines);
    return ok;
}

bool parse_program(Tokens *t, Block *out, SyntaxError *err) {
    Parser p = {t->t, t->n, 0, 0, 0, err};
    return program(&p, out);
}

bool parse_text(const char *src, size_t len, Block *out, SyntaxError *err) {
    Tokens t;
    if (!lex(src, len, &t, err)) return false;
    bool ok = parse_program(&t, out, err);
    // The tree keeps its own references to names, text and numbers.
    tokens_free(&t);
    return ok;
}
