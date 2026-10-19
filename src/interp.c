// The interpreter: runs the syntax tree.

#include <math.h>
#include <time.h>

#include "jlb.h"
#include "path.h"

#define MAX_DEPTH 2500

// ---------- frames and scopes ----------

static Frame *top(Interp *it) {
    return &it->frames[it->nframes - 1];
}

static void scopes_push(Scopes *s) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->s = xrealloc(s->s, (size_t)s->cap * sizeof(OrdMap));
    }
    om_init(&s->s[s->n++]);
}

static void scopes_pop(Scopes *s) {
    om_free(&s->s[--s->n]);
}

static void scopes_free(Scopes *s) {
    while (s->n) scopes_pop(s);
    free(s->s);
    s->s = NULL;
    s->cap = 0;
}

static void push_frame(Interp *it, Function *f, Env *env, int line, Source *src) {
    if (it->nframes == it->cap) {
        it->cap = it->cap ? it->cap * 2 : 16;
        it->frames = xrealloc(it->frames, (size_t)it->cap * sizeof(Frame));
    }
    Frame *fr = &it->frames[it->nframes++];
    memset(fr, 0, sizeof *fr);
    fr->func = f;
    if (f) f->rc++;
    fr->env = env;
    env->rc++;
    fr->line = line;
    fr->src = src;
    src->rc++;
}

static void pop_frame(Interp *it) {
    Frame *fr = &it->frames[--it->nframes];
    scopes_free(&fr->scopes);
    func_unref(fr->func);
    env_unref(fr->env);
    source_unref(fr->src);
}

void interp_init(Interp *it) {
    memset(it, 0, sizeof *it);
    it->start = now_seconds();
    it->last_delta = it->start;
    uint64_t seed = (uint64_t)time(NULL) * 1000000007ull ^ (uint64_t)(now_seconds() * 1e9);
    it->rng = seed ^ 0x9E3779B97F4A7C15ull;
    it->ret = NULL_VALUE;
}

void interp_free(Interp *it) {
    while (it->nframes) pop_frame(it);
    free(it->frames);
    for (int i = 0; i < it->nimported; i++) free(it->imported[i]);
    free(it->imported);
    for (int i = 0; i < it->nloaded; i++) {
        free(it->loaded[i].path);
        v_release(it->loaded[i].v);
    }
    free(it->loaded);
    v_release(it->ret);
    str_unref(it->goto_name);
    memset(it, 0, sizeof *it);
}

double now_seconds(void) {
#ifdef _WIN32
    return (double)clock() / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

// ---------- positions and entries ----------

typedef struct {
    bool is_name;
    Str *name; // borrowed or owned (see key_free)
    int64_t pos;
} Key;

static void key_free(Key *k) {
    if (k->is_name) str_unref(k->name);
}

static bool key_from(Value v, Key *out) {
    switch (v.t) {
    case V_NUM:
        if (!num_is_integer(v.num)) {
            char *s = num_str(v.num);
            fail("positions must be whole numbers, not %s", s);
            free(s);
            return false;
        }
        out->is_name = false;
        out->pos = num_to_i64_sat(v.num);
        return true;
    case V_TEXT:
        out->is_name = true;
        out->name = str_ref(v.s);
        return true;
    default: return fail("a %s can't be used as a position or a name", v_type(v));
    }
}

/// 1-based position (negative counts from the end) to a 0-based index, or -1.
static long resolve(int64_t p, size_t len) {
    int64_t n = (int64_t)len, i;
    if (p > 0) i = p - 1;
    else if (p < 0) i = n + p;
    else return -1;
    return i >= 0 && i < n ? (long)i : -1;
}

static const char *fname(Function *f) {
    return f->decl->name ? f->decl->name->s : "FUNCTION";
}

static bool whole_val(Value v, const char *what, int64_t *out) {
    if (v.t == V_NUM && num_is_integer(v.num)) {
        *out = num_to_i64_sat(v.num);
        return true;
    }
    if (v.t == V_NUM) {
        char *s = num_str(v.num);
        fail("%s must be a whole number, not %s", what, s);
        free(s);
        return false;
    }
    return fail("%s must be a number, not a %s", what, v_type(v));
}

/// The character at index i (0-based) as text.
static Value char_at(Str *s, size_t i) {
    size_t a = utf8_offset(s->s, s->len, i);
    size_t b = a;
    utf8_next(s->s, s->len, &b);
    return v_textn(s->s + a, b - a);
}

static bool index_get(Value b, Value i, Value *out) {
    Key k;
    switch (b.t) {
    case V_TABLE: {
        if (!key_from(i, &k)) return false;
        Table *t = b.tb.tab;
        if (k.is_name) {
            Slot *s = om_gets(&t->named, k.name);
            *out = s ? v_retain(s->v) : NULL_VALUE;
        } else {
            long x = resolve(k.pos, t->n);
            *out = x >= 0 ? v_retain(t->items[x]->v) : NULL_VALUE;
        }
        key_free(&k);
        return true;
    }
    case V_TEXT:
        if (!key_from(i, &k)) return false;
        if (k.is_name) {
            key_free(&k);
            return fail("text positions must be numbers");
        }
        {
            long x = resolve(k.pos, b.s->nchars);
            *out = x >= 0 ? char_at(b.s, (size_t)x) : NULL_VALUE;
        }
        return true;
    case V_NULL: return fail("can't use [ ] on NULL");
    default: return fail("can't use [ ] on a %s", v_type(b));
    }
}

static Value key_value(const Key *k) {
    return k->is_name ? v_text(str_ref(k->name)) : v_int(k->pos);
}

static bool get_entry(Slot *bs, const Key *key, Value *out) {
    Value kv = key_value(key);
    bool ok = index_get(bs->v, kv, out);
    v_release(kv);
    return ok;
}

/// Replaces character i of the text with `rep`.
static Str *splice_text(Str *s, size_t i, size_t remove, const char *rep, size_t replen) {
    size_t a = utf8_offset(s->s, s->len, i);
    size_t b = a;
    for (size_t k = 0; k < remove; k++) utf8_next(s->s, s->len, &b);
    Buf o;
    buf_init(&o);
    buf_put(&o, s->s, a);
    buf_put(&o, rep, replen);
    buf_put(&o, s->s + b, s->len - b);
    return str_from_buf(&o);
}

/// Sets an entry (takes v).
/// Nothing held by a constant (made with CONST) can change.
static bool not_const(const Slot *bs) {
    if (bs->constant) return fail("this is a constant (CONST), so it can't be changed");
    return true;
}

static bool const_name(const Slot *s, const Str *name) {
    if (s->constant) return fail("%s is a constant (CONST), so it can't be changed", name->s);
    return true;
}

static bool set_entry(Slot *bs, const Key *key, Value v) {
    if (!not_const(bs)) {
        v_release(v);
        return false;
    }
    Value *b = &bs->v;
    switch (b->t) {
    case V_TABLE: {
        Table *t = table_mut(b);
        if (key->is_name) {
            Slot *s = om_gets(&t->named, key->name);
            if (s) {
                if (!not_const(s)) {
                    v_release(v);
                    return false;
                }
                v_release(s->v);
                s->v = v;
            } else {
                om_insert(&t->named, key->name, slot_new(v));
            }
            return true;
        }
        size_t len = t->n;
        long i = resolve(key->pos, len);
        if (i >= 0) {
            if (!not_const(t->items[i])) {
                v_release(v);
                return false;
            }
            v_release(t->items[i]->v);
            t->items[i]->v = v;
            return true;
        }
        if (key->pos == (int64_t)len + 1) {
            table_push(t, v);
            return true;
        }
        v_release(v);
        if (key->pos == 0) return fail("positions start at 1, so [0] can't be written to");
        return fail("position %lld is past the end of the table (it has %zu items)", (long long)key->pos, len);
    }
    case V_TEXT: {
        if (key->is_name) {
            v_release(v);
            return fail("text positions must be numbers");
        }
        Str *s = b->s;
        long i = resolve(key->pos, s->nchars);
        if (i >= 0) {
            char *rep = display(v);
            Str *ns = splice_text(s, (size_t)i, 1, rep, strlen(rep));
            free(rep);
            v_release(v);
            str_unref(b->s);
            b->s = ns;
            return true;
        }
        v_release(v);
        if (key->pos == 0) return fail("positions start at 1, so [0] can't be written to");
        return fail("position %lld is past the end of the text", (long long)key->pos);
    }
    case V_NULL: v_release(v); return fail("can't set an entry of NULL");
    default: v_release(v); return fail("a %s has no entries to set", v_type(*b));
    }
}

/// Puts a shared slot in an entry (takes the slot).
static bool set_entry_slot(Slot *bs, const Key *key, Slot *sh) {
    if (!not_const(bs)) {
        slot_unref(sh);
        return false;
    }
    Value *b = &bs->v;
    if (b->t != V_TABLE) {
        slot_unref(sh);
        return fail("@= can only share into a table, not a %s", v_type(*b));
    }
    Table *t = table_mut(b);
    if (key->is_name) {
        om_insert(&t->named, key->name, sh);
        return true;
    }
    long i = resolve(key->pos, t->n);
    if (i >= 0) {
        slot_unref(t->items[i]);
        t->items[i] = sh;
        return true;
    }
    if (key->pos == (int64_t)t->n + 1) {
        table_push_slot(t, sh);
        return true;
    }
    slot_unref(sh);
    return fail("position %lld is outside the table", (long long)key->pos);
}

static bool insert_index(int64_t p, size_t len, size_t *out) {
    int64_t q;
    if (p > 0) q = p;
    else if (p < 0) q = (int64_t)len + 2 + p;
    else return fail("positions start at 1, so 0 isn't a position");
    if (q >= 1 && q <= (int64_t)len + 1) {
        *out = (size_t)(q - 1);
        return true;
    }
    return fail("position %lld is outside (there are %zu items)", (long long)p, len);
}

static bool apply_action(Value *v, bool insert, Value *extra) {
    const char *name = insert ? "INSERT" : "DROP";
    int64_t p;
    if (!whole_val(insert ? extra[1] : extra[0], "the position", &p)) return false;
    switch (v->t) {
    case V_TABLE: {
        Table *t = table_mut(v);
        size_t len = t->n;
        if (insert) {
            size_t i;
            if (!insert_index(p, len, &i)) return false;
            table_insert_slot(t, i, slot_new(v_copy(v_retain(extra[0]))));
        } else {
            long i = resolve(p, len);
            if (i < 0) return fail("DROP: position %lld doesn't exist (there are %zu items)", (long long)p, len);
            table_remove(t, (size_t)i);
        }
        return true;
    }
    case V_TEXT: {
        Str *s = v->s;
        size_t len = s->nchars;
        Str *ns;
        if (insert) {
            size_t i;
            if (!insert_index(p, len, &i)) return false;
            char *add = display(extra[0]);
            ns = splice_text(s, i, 0, add, strlen(add));
            free(add);
        } else {
            long i = resolve(p, len);
            if (i < 0) return fail("DROP: position %lld doesn't exist (the text has %zu characters)", (long long)p, len);
            ns = splice_text(s, (size_t)i, 1, "", 0);
        }
        str_unref(v->s);
        v->s = ns;
        return true;
    }
    default: return fail("%s works on tables and text, not a %s", name, v_type(*v));
    }
}

// ---------- errors ----------

static char *source_path(Source *src) {
    if (strcmp(src->name, "quick-try") == 0) return xstrdup("");
    return path_join(src->dir, src->name);
}

/// Fills in where an error happened and every variable, the first time it passes a statement.
static void capture(Interp *it, int line) {
    if (g_fault != FAULT_ERROR || !g_err || g_err->captured) return;
    ErrorInfo *e = g_err;
    Frame *t = top(it);
    t->line = line;
    e->captured = true;
    e->traced = true;
    e->line = line;
    free(e->file);
    e->file = xstrdup(t->src->name);
    free(e->path);
    e->path = source_path(t->src);
    for (int i = it->nframes; i-- > 0;) {
        Frame *fr = &it->frames[i];
        if (!fr->func) continue;
        e->frames = xrealloc(e->frames, (e->nframes + 1) * sizeof(ErrFrame));
        e->frame_paths = xrealloc(e->frame_paths, (e->nframes + 1) * sizeof(char *));
        ErrFrame *ef = &e->frames[e->nframes];
        memset(ef, 0, sizeof *ef);
        ef->name = xstrdup(fname(fr->func));
        ef->line = fr->line;
        for (int s = 0; s < fr->scopes.n; s++) vars_of(&fr->scopes.s[s], &ef->vars);
        e->frame_paths[e->nframes] = source_path(fr->src);
        e->nframes++;
    }
    vars_of(&t->env->map, &e->outside);
}

static bool goto_error(Interp *it, Str *name, int line) {
    fail("GOTO %s: there's no POINT %s it can reach (GOTO can't jump into a block or out of a function)", name->s, name->s);
    capture(it, line);
    return false;
}

static Value error_table(const ErrorInfo *e) {
    Table *t = table_new();
    Str *k;
    k = str_c("message");
    om_insert(&t->named, k, slot_new(v_textc(e->msg)));
    str_unref(k);
    k = str_c("line");
    om_insert(&t->named, k, slot_new(v_int(e->line)));
    str_unref(k);
    k = str_c("file");
    om_insert(&t->named, k, slot_new(v_textc(e->file)));
    str_unref(k);
    Table *vars = table_new();
    if (e->nframes) {
        const VarList *l = &e->frames[0].vars;
        for (size_t i = 0; i < l->n; i++) om_insert(&vars->named, l->v[i].name, slot_new(v_retain(*l->v[i].value)));
    }
    for (size_t i = 0; i < e->outside.n; i++) {
        if (!om_gets(&vars->named, e->outside.v[i].name))
            om_insert(&vars->named, e->outside.v[i].name, slot_new(v_retain(*e->outside.v[i].value)));
    }
    k = str_c("variables");
    om_insert(&t->named, k, slot_new(v_table(vars)));
    str_unref(k);
    char *r = report(e);
    t->report = str_c(r);
    free(r);
    return v_table(t);
}

// ---------- variables ----------

static Slot *lookup(Interp *it, const Str *name) {
    Frame *f = top(it);
    for (int i = f->scopes.n; i-- > 0;) {
        Slot *s = om_gets(&f->scopes.s[i], name);
        if (s) return s;
    }
    return om_gets(&f->env->map, name);
}

/// A name of the running function (its locals, FOR and FAIL names), or, outside
/// functions, any name. Inside a function, outside names are only read.
static Slot *lookup_own(Interp *it, const Str *name) {
    Frame *f = top(it);
    if (!f->func) return lookup(it, name);
    for (int i = f->scopes.n; i-- > 0;) {
        Slot *s = om_gets(&f->scopes.s[i], name);
        if (s) return s;
    }
    return NULL;
}

static void create_var(Interp *it, Str *name, Slot *s) {
    Frame *f = top(it);
    if (f->func) om_insert(&f->scopes.s[0], name, s);
    else om_insert(&f->env->map, name, s);
}

/// `name` on a line by itself: a new variable holding NULL (the function's own, inside a
/// function, like LOCAL), unless the name already has a value here.
static void declare(Interp *it, Str *name) {
    if (!lookup_own(it, name)) create_var(it, name, slot_new(NULL_VALUE));
}

/// Giving a value to a name: inside a function it's the function's own (a new local
/// unless it already is one); GLOBAL is how a function changes an outside name.
bool set_var(Interp *it, Str *name, Value v) {
    Slot *s = lookup_own(it, name);
    if (s) {
        if (!const_name(s, name)) {
            v_release(v);
            return false;
        }
        v_release(s->v);
        s->v = v;
    } else {
        create_var(it, name, slot_new(v));
    }
    return true;
}

/// Points a name at a slot (takes the slot).
static bool bind_var(Interp *it, Str *name, Slot *s) {
    Slot *old = lookup_own(it, name);
    if (old && !const_name(old, name)) {
        slot_unref(s);
        return false;
    }
    Frame *f = top(it);
    for (int i = f->scopes.n; i-- > 0;) {
        if (om_gets(&f->scopes.s[i], name)) {
            om_insert(&f->scopes.s[i], name, s);
            return true;
        }
    }
    if (!f->func && om_gets(&f->env->map, name)) {
        om_insert(&f->env->map, name, s);
        return true;
    }
    create_var(it, name, s);
    return true;
}

/// CONST name = value: a new name in this function (or file) that never changes (takes v).
static bool make_const(Interp *it, Str *name, Value v) {
    Frame *f = top(it);
    if (om_gets(f->func ? &f->scopes.s[0] : &f->env->map, name)) {
        v_release(v);
        return fail("%s already has a value here, so CONST can't make it a constant. Use CONST where %s first gets its value", name->s, name->s);
    }
    create_var(it, name, slot_const(v));
    return true;
}

static bool remove_var(Interp *it, const Str *name) {
    Frame *f = top(it);
    for (int i = f->scopes.n; i-- > 0;) {
        Slot *s = om_remove(&f->scopes.s[i], name->s, name->len);
        if (s) {
            slot_unref(s);
            return true;
        }
    }
    Slot *s = om_remove(&f->env->map, name->s, name->len);
    if (s) {
        slot_unref(s);
        return true;
    }
    return false;
}

static int func_frame(Interp *it, Function *f) {
    for (int i = it->nframes; i-- > 0;)
        if (it->frames[i].func == f) return i;
    fail("%s isn't running, so its local variables don't exist right now", fname(f));
    return -1;
}

/// A running function's local (borrowed), for f.name.
static Slot *func_local(Interp *it, Function *f, const Str *name) {
    int i = func_frame(it, f);
    if (i < 0) return NULL;
    Frame *fr = &it->frames[i];
    for (int s = fr->scopes.n; s-- > 0;) {
        Slot *sl = om_gets(&fr->scopes.s[s], name);
        if (sl) return sl;
    }
    fail("%s has no local variable named %s", fname(f), name->s);
    return NULL;
}

static Value make_func(Interp *it, FuncDecl *d) {
    Frame *f = top(it);
    Value v;
    v.t = V_FUNC;
    v.fn = func_new(d, f->env, f->src);
    return v;
}

// ---------- statements ----------

static Flow exec_block(Interp *it, const Block *b);
static bool eval(Interp *it, Expr *e, Value *out);
static bool start(Interp *it, Expr *call, Value *out);
static bool assign(Interp *it, Expr *target, Op op, Expr *rhs);
static Flow for_to(Interp *it, Stmt *s);
static Flow for_in(Interp *it, Stmt *s);
static bool exec_global(Interp *it, Stmt *s);
static bool do_import(Interp *it, Str *p);
static bool do_load(Interp *it, Str *p);

static long find_point(const Block *b, const Str *name) {
    for (int i = 0; i < b->n; i++)
        if (b->s[i]->k == S_POINT && str_eq(b->s[i]->name, name)) return i;
    return -1;
}

static Flow exec_kind(Interp *it, Stmt *s);

static Flow exec_stmt(Interp *it, Stmt *s) {
    top(it)->line = s->line;
    // With tasks running, each one gets a turn every so many statements.
    if (it->ntasks && !task_tick(it)) {
        capture(it, s->line);
        return F_ERR;
    }
    Flow f = exec_kind(it, s);
    if (f == F_ERR) capture(it, s->line);
    return f;
}

static Flow exec_block(Interp *it, const Block *b) {
    int i = 0;
    while (i < b->n) {
        Flow f = exec_stmt(it, b->s[i]);
        if (f == F_NORMAL) {
            i++;
        } else if (f == F_GOTO) {
            long j = find_point(b, it->goto_name);
            if (j < 0) return F_GOTO;
            str_unref(it->goto_name);
            it->goto_name = NULL;
            i = (int)j + 1;
        } else {
            return f;
        }
    }
    return F_NORMAL;
}

#define KEEP_LOOPING (-2)

/// KEEP_LOOPING, or the flow to leave the loop with (BREAK becomes NORMAL).
static int loop_body(Interp *it, const Block *b) {
    Flow f = exec_block(it, b);
    if (f == F_NORMAL || f == F_SKIP) return KEEP_LOOPING;
    if (f == F_BREAK) return F_NORMAL;
    return f;
}

static bool eval_truth(Interp *it, Expr *e, bool *out) {
    Value v;
    if (!eval(it, e, &v)) return false;
    bool ok = truth(v, out);
    v_release(v);
    return ok;
}

static Flow exec_kind(Interp *it, Stmt *s) {
    if (s->global) return exec_global(it, s) ? F_NORMAL : F_ERR;
    switch (s->k) {
    case S_EXPR: {
        // A name by itself makes that variable, with NULL, unless it already has a value.
        if (s->e->k == E_VAR) {
            declare(it, s->e->name);
            return F_NORMAL;
        }
        Value v;
        if (!eval(it, s->e, &v)) return F_ERR;
        v_release(v);
        return F_NORMAL;
    }
    case S_ASSIGN: return assign(it, s->e, s->op, s->e2) ? F_NORMAL : F_ERR;
    case S_CONST: {
        Value v;
        if (!eval(it, s->e, &v)) return F_ERR;
        return make_const(it, s->name, v) ? F_NORMAL : F_ERR;
    }
    case S_LOCAL: {
        Value v = NULL_VALUE;
        if (s->e) {
            if (!eval(it, s->e, &v)) return F_ERR;
            v = v_copy(v);
        }
        om_insert(&top(it)->scopes.s[0], s->name, slot_new(v));
        return F_NORMAL;
    }
    case S_IF:
        for (int i = 0; i < s->nconds; i++) {
            bool t;
            if (!eval_truth(it, s->conds[i], &t)) return F_ERR;
            if (t) return exec_block(it, &s->blocks[i]);
        }
        if (s->has_else) return exec_block(it, &s->body2);
        return F_NORMAL;
    case S_WHILE:
        for (;;) {
            bool t;
            if (!eval_truth(it, s->e, &t)) return F_ERR;
            if (!t) break;
            int f = loop_body(it, &s->body);
            if (f != KEEP_LOOPING) return (Flow)f;
        }
        return F_NORMAL;
    case S_REPEAT: {
        Value v;
        if (!eval(it, s->e, &v)) return F_ERR;
        uint64_t count;
        if (v.t != V_NUM) {
            fail("REPEAT needs a number, not a %s", v_type(v));
            v_release(v);
            return F_ERR;
        }
        if (num_is_neg(v.num)) {
            v_release(v);
            fail("REPEAT can't run a negative number of times");
            return F_ERR;
        }
        if (!num_is_integer(v.num)) {
            v_release(v);
            fail("REPEAT needs a whole number of times");
            return F_ERR;
        }
        count = (uint64_t)num_to_i64_sat(v.num);
        v_release(v);
        for (uint64_t k = 0; k < count; k++) {
            int f = loop_body(it, &s->body);
            if (f != KEEP_LOOPING) return (Flow)f;
        }
        return F_NORMAL;
    }
    case S_FOR_TO: return for_to(it, s);
    case S_FOR_IN: return for_in(it, s);
    case S_BREAK: return F_BREAK;
    case S_SKIP: return F_SKIP;
    case S_RETURN: {
        Value v = NULL_VALUE;
        if (s->e) {
            if (!eval(it, s->e, &v)) return F_ERR;
            v = v_copy(v);
        }
        v_release(it->ret);
        it->ret = v;
        return F_RETURN;
    }
    case S_END:
        g_fault = FAULT_END;
        return F_ERR;
    case S_FUNC: return set_var(it, s->fn->name, make_func(it, s->fn)) ? F_NORMAL : F_ERR;
    case S_TRY: {
        Flow f = exec_block(it, &s->body);
        if (f != F_ERR || g_fault != FAULT_ERROR) return f;
        ErrorInfo *e = take_error();
        Value t = error_table(e);
        error_free(e);
        Frame *fr = top(it);
        scopes_push(&fr->scopes);
        om_insert(&fr->scopes.s[fr->scopes.n - 1], s->name, slot_new(t));
        Flow r = exec_block(it, &s->body2);
        fr = top(it);
        scopes_pop(&fr->scopes);
        return r;
    }
    case S_GOTO:
        str_unref(it->goto_name);
        it->goto_name = str_ref(s->name);
        it->goto_line = s->line;
        return F_GOTO;
    case S_POINT: return F_NORMAL;
    case S_IMPORT: return do_import(it, s->name) ? F_NORMAL : F_ERR;
    case S_LOAD: return do_load(it, s->name) ? F_NORMAL : F_ERR;
    }
    return F_NORMAL;
}

static bool loop_num(Interp *it, Expr *e, const char *what, Number *out) {
    Value v;
    if (!eval(it, e, &v)) return false;
    if (v.t != V_NUM) {
        fail("the %s of a counting loop must be a number, not a %s", what, v_type(v));
        v_release(v);
        return false;
    }
    *out = v.num;
    return true;
}

static Flow for_to(Interp *it, Stmt *s) {
    Number start, end, st;
    if (!loop_num(it, s->e, "start", &start)) return F_ERR;
    if (!loop_num(it, s->e2, "end", &end)) {
        num_release(start);
        return F_ERR;
    }
    if (s->e3) {
        if (!loop_num(it, s->e3, "STEP", &st)) {
            num_release(start);
            num_release(end);
            return F_ERR;
        }
    } else {
        st = num_int(1);
    }
    Flow result = F_NORMAL;
    int64_t count = 0;
    bool approx = false;
    if (num_is_zero(st)) {
        fail("STEP 0 would loop forever");
        result = F_ERR;
        goto done;
    }
    bool up = num_cmp(start, end) <= 0;
    {
        Number a = num_abs(st);
        num_release(st);
        if (up) {
            st = a;
        } else {
            st = num_neg(a);
            num_release(a);
        }
    }
    // Exact numbers count exactly. Approximate ones get a little tidying.
    approx = num_is_float(start) || num_is_float(end) || num_is_float(st);
    if (approx) {
        double s0 = num_to_f64(start), e0 = num_to_f64(end), t0 = num_to_f64(st);
        double c = floor(((e0 - s0) / t0) + 1e-9);
        // Like Rust's `as i64` (saturating).
        int64_t ci = isnan(c) ? 0 : c >= 9.2233720368547758e18 ? INT64_MAX : c <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)c;
        count = ci == INT64_MAX ? INT64_MAX : ci + 1;
    } else {
        Number d, q, f;
        if (!num_sub(end, start, &d)) {
            result = F_ERR;
            goto done;
        }
        bool ok = num_div(d, st, &q);
        num_release(d);
        if (!ok) {
            result = F_ERR;
            goto done;
        }
        ok = num_round(q, 0, ROUND_FLOOR, &f);
        num_release(q);
        if (!ok) {
            result = F_ERR;
            goto done;
        }
        int64_t c = num_to_i64_sat(f);
        num_release(f);
        count = c == INT64_MAX ? INT64_MAX : c + 1;
    }
    {
        Frame *fr = top(it);
        scopes_push(&fr->scopes);
        Number v = num_retain(start);
        for (int64_t k = 0; k < (count > 0 ? count : 0); k++) {
            if (k > 0) {
                Number next;
                bool ok;
                if (approx) ok = num_from_f64_rounded(num_to_f64(start) + (double)k * num_to_f64(st), 12, &next);
                else ok = num_add(v, st, &next);
                if (!ok) {
                    result = F_ERR;
                    break;
                }
                num_release(v);
                v = next;
            }
            fr = top(it);
            om_insert(&fr->scopes.s[fr->scopes.n - 1], s->name, slot_new(v_num(num_retain(v))));
            int f = loop_body(it, &s->body);
            if (f != KEEP_LOOPING) {
                result = (Flow)f;
                break;
            }
        }
        num_release(v);
        fr = top(it);
        scopes_pop(&fr->scopes);
    }
done:
    num_release(start);
    num_release(end);
    num_release(st);
    return result;
}

static bool lvalue_slot(Interp *it, Expr *e, Slot **out);

typedef struct {
    Value key;
    Slot *slot;
} LoopEntry;

static LoopEntry *table_entries(Table *t, size_t *n) {
    LoopEntry *v = xmalloc((t->n + t->named.n + 1) * sizeof(LoopEntry));
    size_t k = 0;
    for (size_t i = 0; i < t->n; i++) {
        v[k].key = v_int((int64_t)i + 1);
        v[k++].slot = slot_ref(t->items[i]);
    }
    for (size_t i = 0; i < t->named.n; i++) {
        v[k].key = v_text(str_ref(t->named.e[i].key));
        v[k++].slot = slot_ref(t->named.e[i].slot);
    }
    *n = k;
    return v;
}

static LoopEntry *loop_entries(Interp *it, Expr *iter, bool want_slots, size_t *n) {
    if (want_slots && is_lvalue(iter)) {
        Slot *s;
        if (!lvalue_slot(it, iter, &s)) return NULL;
        if (s->v.t == V_TABLE) {
            // A constant's entries are constants too, so they're shared as they are.
            Table *t = s->constant ? s->v.tb.tab : table_mut(&s->v);
            LoopEntry *r = table_entries(t, n);
            slot_unref(s);
            return r;
        }
        slot_unref(s);
    }
    Value v;
    if (!eval(it, iter, &v)) return NULL;
    LoopEntry *r = NULL;
    if (v.t == V_TABLE) {
        r = table_entries(v.tb.tab, n);
    } else if (v.t == V_TEXT) {
        Str *s = v.s;
        r = xmalloc((s->nchars + 1) * sizeof(LoopEntry));
        size_t k = 0;
        for (size_t i = 0; i < s->len;) {
            size_t st = i;
            utf8_next(s->s, s->len, &i);
            r[k].key = v_int((int64_t)k + 1);
            r[k].slot = slot_new(v_textn(s->s + st, i - st));
            k++;
        }
        *n = k;
    } else {
        fail("FOR ... IN needs a table or text, not a %s", v_type(v));
    }
    v_release(v);
    return r;
}

static Slot *bind_slot(bool shared, Slot *s) {
    if (shared) return slot_ref(s);
    return slot_new(v_copy(v_retain(s->v)));
}

static Flow for_in(Interp *it, Stmt *s) {
    bool want = s->shared || (s->has2 && s->shared2);
    size_t n = 0;
    LoopEntry *entries = loop_entries(it, s->e, want, &n);
    if (!entries) return F_ERR;
    Frame *fr = top(it);
    scopes_push(&fr->scopes);
    Flow result = F_NORMAL;
    size_t k;
    for (k = 0; k < n; k++) {
        fr = top(it);
        OrdMap *sc = &fr->scopes.s[fr->scopes.n - 1];
        if (!s->has2) {
            om_insert(sc, s->name, bind_slot(s->shared, entries[k].slot));
        } else {
            om_insert(sc, s->name, slot_new(v_retain(entries[k].key)));
            om_insert(sc, s->name2, bind_slot(s->shared2, entries[k].slot));
        }
        int f = loop_body(it, &s->body);
        if (f != KEEP_LOOPING) {
            result = (Flow)f;
            break;
        }
    }
    for (size_t i = 0; i < n; i++) {
        v_release(entries[i].key);
        slot_unref(entries[i].slot);
    }
    free(entries);
    fr = top(it);
    scopes_pop(&fr->scopes);
    return result;
}

// ---------- assignment ----------

static bool slot_func(Slot *bs, Function **f) {
    if (bs->v.t == V_FUNC) {
        *f = bs->v.fn;
        return true;
    }
    return false;
}

/// The slot of a table entry (making the table private first), or a running function's local.
static bool entry_slot(Interp *it, Slot *bs, const Key *key, Slot **out) {
    Function *f;
    if (slot_func(bs, &f)) {
        if (!key->is_name) return fail("use .name to reach a function's local variable");
        Slot *s = func_local(it, f, key->name);
        if (!s) return false;
        *out = slot_ref(s);
        return true;
    }
    Value *v = &bs->v;
    switch (v->t) {
    case V_TABLE: {
        // Nothing to copy first in a constant: nothing in it changes.
        Table *t = bs->constant ? v->tb.tab : table_mut(v);
        if (key->is_name) {
            Slot *s = om_gets(&t->named, key->name);
            if (!s) return fail("the table has no entry named %s", key->name->s);
            *out = slot_ref(s);
            return true;
        }
        long i = resolve(key->pos, t->n);
        if (i < 0) return fail("position %lld is outside the table", (long long)key->pos);
        *out = slot_ref(t->items[i]);
        return true;
    }
    case V_TEXT: return fail("a single character of text can't be shared or changed this way");
    default: return fail("a %s has no entries", v_type(*v));
    }
}

/// The slot an lvalue names (the caller releases it).
static bool lvalue_slot(Interp *it, Expr *e, Slot **out) {
    switch (e->k) {
    case E_VAR: {
        Slot *s = lookup(it, e->name);
        if (!s) return fail("%s isn't defined", e->name->s);
        *out = slot_ref(s);
        return true;
    }
    case E_FIELD: {
        Slot *bs;
        if (!lvalue_slot(it, e->x, &bs)) return false;
        Key k = {true, e->name, 0};
        bool ok = entry_slot(it, bs, &k, out);
        slot_unref(bs);
        return ok;
    }
    case E_INDEX: {
        Slot *bs;
        if (!lvalue_slot(it, e->x, &bs)) return false;
        Value iv;
        if (!eval(it, e->y, &iv)) {
            slot_unref(bs);
            return false;
        }
        Key k;
        bool ok = key_from(iv, &k);
        v_release(iv);
        if (ok) {
            ok = entry_slot(it, bs, &k, out);
            key_free(&k);
        }
        slot_unref(bs);
        return ok;
    }
    default: return fail("this can't be changed");
    }
}

static bool share_slot(Interp *it, Expr *e, Slot **out) {
    if (is_lvalue(e)) {
        Slot *s;
        // Every kind of value is shared: both names hold the same variable.
        if (!lvalue_slot(it, e, &s)) return false;
        *out = s;
        return true;
    }
    Value v;
    if (!eval(it, e, &v)) return false;
    *out = slot_new(v);
    return true;
}

/// x += r and friends (takes r).
static bool compound(Slot *slot, Op op, Value r) {
    Op base;
    switch (op) {
    case OP_ADD_SET: base = OP_ADD; break;
    case OP_SUB_SET: base = OP_SUB; break;
    case OP_MUL_SET: base = OP_MUL; break;
    case OP_DIV_SET: base = OP_DIV; break;
    case OP_POW_SET: base = OP_POW; break;
    case OP_ROOT_SET: base = OP_ROOT; break;
    case OP_REM_SET: base = OP_REM; break;
    default: base = OP_QUOT; break;
    }
    if (!not_const(slot)) {
        v_release(r);
        return false;
    }
    // Fast path: `table += value` adds to the end in place.
    if (base == OP_ADD && r.t != V_TABLE && slot->v.t == V_TABLE) {
        Table *t = table_mut(&slot->v);
        table_push(t, v_copy(r));
        return true;
    }
    Value nv;
    bool ok = binop(base, slot->v, r, &nv);
    v_release(r);
    if (!ok) return false;
    v_release(slot->v);
    slot->v = nv;
    return true;
}

static Op base_op(Op op) {
    switch (op) {
    case OP_ADD_SET: return OP_ADD;
    case OP_SUB_SET: return OP_SUB;
    case OP_MUL_SET: return OP_MUL;
    case OP_DIV_SET: return OP_DIV;
    case OP_POW_SET: return OP_POW;
    case OP_ROOT_SET: return OP_ROOT;
    case OP_REM_SET: return OP_REM;
    default: return OP_QUOT;
    }
}

static bool assign_entry(Interp *it, Slot *bs, const Key *key, Op op, Expr *rhs) {
    Function *f;
    if (slot_func(bs, &f)) {
        if (!key->is_name) return fail("use .name to reach a function's local variable");
        Slot *s = func_local(it, f, key->name);
        if (!s) return false;
        slot_ref(s);
        Value v;
        if (!eval(it, rhs, &v)) {
            slot_unref(s);
            return false;
        }
        bool ok = true;
        if (s->constant) {
            v_release(v);
            ok = const_name(s, key->name);
        } else if (op == OP_SET || op == OP_SHARE_SET) {
            v_release(s->v);
            s->v = v_copy(v);
        } else {
            ok = compound(s, op, v);
        }
        slot_unref(s);
        return ok;
    }
    if (op == OP_SHARE_SET) {
        Slot *sh;
        if (!share_slot(it, rhs, &sh)) return false;
        return set_entry_slot(bs, key, sh);
    }
    Value r;
    if (!eval(it, rhs, &r)) return false;
    if (op == OP_SET) return set_entry(bs, key, v_copy(r));
    Value cur, nv;
    if (!get_entry(bs, key, &cur)) {
        v_release(r);
        return false;
    }
    bool ok = binop(base_op(op), cur, r, &nv);
    v_release(cur);
    v_release(r);
    if (!ok) return false;
    return set_entry(bs, key, nv);
}

/// GLOBAL name = ..., GLOBAL DEF name, GLOBAL CONST name: the file's outside name,
/// even inside a function.
static bool exec_global(Interp *it, Stmt *s) {
    OrdMap *env = &top(it)->env->map;
    // GLOBAL name by itself: the outside name, made with NULL if it has no value yet.
    if (s->k == S_EXPR) {
        if (!om_gets(env, s->e->name)) om_insert(env, s->e->name, slot_new(NULL_VALUE));
        return true;
    }
    if (s->k == S_CONST) {
        if (om_gets(env, s->name))
            return fail("%s already has an outside value, so CONST can't make it a constant. Use CONST where %s first gets its value", s->name->s, s->name->s);
        Value v;
        if (!eval(it, s->e, &v)) return false;
        om_insert(env, s->name, slot_const(v));
        return true;
    }
    Str *n = s->k == S_FUNC ? s->fn->name : s->e->name;
    Slot *old = om_gets(env, n);
    if (old && !const_name(old, n)) return false;
    if (s->k == S_FUNC) {
        Value f = make_func(it, s->fn);
        if (old) {
            v_release(old->v);
            old->v = f;
        } else {
            om_insert(env, n, slot_new(f));
        }
        return true;
    }
    if (s->op == OP_SHARE_SET) {
        Slot *sh;
        if (!share_slot(it, s->e2, &sh)) return false;
        om_insert(&top(it)->env->map, n, sh);
        return true;
    }
    if (s->op == OP_SET) {
        Value v;
        if (!eval(it, s->e2, &v)) return false;
        v = v_copy(v);
        old = om_gets(&top(it)->env->map, n); // the value may have made it
        if (old) {
            if (!const_name(old, n)) {
                v_release(v);
                return false;
            }
            v_release(old->v);
            old->v = v;
        } else {
            om_insert(&top(it)->env->map, n, slot_new(v));
        }
        return true;
    }
    if (!old) return fail("%s has no outside value yet, so GLOBAL %s %s can't change it", n->s, n->s, OP_TEXT[s->op]);
    slot_ref(old);
    Value r;
    if (!eval(it, s->e2, &r)) {
        slot_unref(old);
        return false;
    }
    bool ok = compound(old, s->op, r);
    slot_unref(old);
    return ok;
}

static bool assign(Interp *it, Expr *target, Op op, Expr *rhs) {
    switch (target->k) {
    case E_VAR: {
        Str *n = target->name;
        if (op == OP_SHARE_SET) {
            Slot *s;
            if (!share_slot(it, rhs, &s)) return false;
            return bind_var(it, n, s);
        }
        if (op == OP_SET) {
            Value v;
            if (!eval(it, rhs, &v)) return false;
            return set_var(it, n, v_copy(v));
        }
        Slot *slot = lookup_own(it, n);
        if (!slot) {
            if (lookup(it, n))
                return fail("%s isn't one of this function's own variables, so %s %s can't change it. To change the outside %s, write GLOBAL %s %s ...",
                            n->s, n->s, OP_TEXT[op], n->s, n->s, OP_TEXT[op]);
            return fail("%s isn't defined", n->s);
        }
        if (!const_name(slot, n)) return false;
        slot_ref(slot);
        Value r;
        if (!eval(it, rhs, &r)) {
            slot_unref(slot);
            return false;
        }
        bool ok = compound(slot, op, r);
        slot_unref(slot);
        return ok;
    }
    case E_FIELD: {
        Slot *bs;
        if (!lvalue_slot(it, target->x, &bs)) return false;
        Key k = {true, target->name, 0};
        bool ok = assign_entry(it, bs, &k, op, rhs);
        slot_unref(bs);
        return ok;
    }
    case E_INDEX: {
        Slot *bs;
        if (!lvalue_slot(it, target->x, &bs)) return false;
        Value iv;
        if (!eval(it, target->y, &iv)) {
            slot_unref(bs);
            return false;
        }
        Key k;
        bool ok = key_from(iv, &k);
        v_release(iv);
        if (ok) {
            ok = assign_entry(it, bs, &k, op, rhs);
            key_free(&k);
        }
        slot_unref(bs);
        return ok;
    }
    default: return fail("this can't be assigned to");
    }
}

static bool remove_entry(Interp *it, Slot *bs, const Key *key) {
    Function *f;
    if (slot_func(bs, &f)) {
        if (!key->is_name) return fail("use .name to reach a function's local variable");
        int i = func_frame(it, f);
        if (i < 0) return false;
        Frame *fr = &it->frames[i];
        for (int s = fr->scopes.n; s-- > 0;) {
            Slot *sl = om_remove(&fr->scopes.s[s], key->name->s, key->name->len);
            if (sl) {
                slot_unref(sl);
                return true;
            }
        }
        return fail("%s has no local variable named %s", fname(f), key->name->s);
    }
    if (!not_const(bs)) return false;
    Value *v = &bs->v;
    if (v->t != V_TABLE) return fail("a %s has no entries to UNDEFINE", v_type(*v));
    Table *t = table_mut(v);
    if (key->is_name) {
        Slot *sl = om_remove(&t->named, key->name->s, key->name->len);
        if (!sl) return fail("the table has no entry named %s to UNDEFINE", key->name->s);
        slot_unref(sl);
        return true;
    }
    long i = resolve(key->pos, t->n);
    if (i < 0) return fail("position %lld doesn't exist", (long long)key->pos);
    table_remove(t, (size_t)i);
    return true;
}

static bool undefine(Interp *it, Expr *t) {
    switch (t->k) {
    case E_VAR: {
        Slot *old = lookup(it, t->name);
        if (old && !const_name(old, t->name)) return false;
        if (remove_var(it, t->name)) return true;
        return fail("%s isn't defined, so it can't be UNDEFINEd", t->name->s);
    }
    case E_FIELD: {
        Slot *bs;
        if (!lvalue_slot(it, t->x, &bs)) return false;
        Key k = {true, t->name, 0};
        bool ok = remove_entry(it, bs, &k);
        slot_unref(bs);
        return ok;
    }
    case E_INDEX: {
        Slot *bs;
        if (!lvalue_slot(it, t->x, &bs)) return false;
        Value iv;
        if (!eval(it, t->y, &iv)) {
            slot_unref(bs);
            return false;
        }
        Key k;
        bool ok = key_from(iv, &k);
        v_release(iv);
        if (ok) {
            ok = remove_entry(it, bs, &k);
            key_free(&k);
        }
        slot_unref(bs);
        return ok;
    }
    default: return fail("UNDEFINE needs a name");
    }
}

// ---------- expressions ----------

static bool slice(Value bv, int64_t a, int64_t z, int64_t step, Value *out) {
    if (step == 0) return fail("a slice STEP can't be 0");
    int64_t len;
    if (bv.t == V_TABLE) len = (int64_t)bv.tb.tab->n;
    else if (bv.t == V_TEXT) len = (int64_t)bv.s->nchars;
    else return fail("can't take a slice of a %s", v_type(bv));
    if (a < 0) a = len + 1 + a;
    if (z < 0) z = len + 1 + z;
    int64_t dir = a <= z ? 1 : -1;
    int64_t st = (step < 0 ? -step : step) * dir;
    int64_t p = a;
    if (dir > 0 && p < 1) p += ((1 - p + st - 1) / st) * st;
    if (dir < 0 && p > len) {
        int64_t d = -st;
        p -= ((p - len + d - 1) / d) * d;
    }
    size_t *idx = NULL;
    size_t n = 0, cap = 0;
    while ((dir > 0 && p <= z && p <= len) || (dir < 0 && p >= z && p >= 1)) {
        if (p >= 1 && p <= len) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                idx = xrealloc(idx, cap * sizeof(size_t));
            }
            idx[n++] = (size_t)(p - 1);
        }
        p += st;
    }
    if (bv.t == V_TABLE) {
        Table *t = table_new();
        for (size_t i = 0; i < n; i++) table_push(t, v_copy(v_retain(bv.tb.tab->items[idx[i]]->v)));
        *out = v_table(t);
    } else {
        // Character starts, then pick.
        Str *s = bv.s;
        size_t *starts = xmalloc((s->nchars + 1) * sizeof(size_t));
        size_t k = 0;
        for (size_t i = 0; i < s->len;) {
            starts[k++] = i;
            utf8_next(s->s, s->len, &i);
        }
        starts[k] = s->len;
        Buf b;
        buf_init(&b);
        for (size_t i = 0; i < n; i++) buf_put(&b, s->s + starts[idx[i]], starts[idx[i] + 1] - starts[idx[i]]);
        free(starts);
        *out = v_text(str_from_buf(&b));
    }
    free(idx);
    return true;
}

static bool call_func(Interp *it, Function *func, Expr **args, int nargs, Value *out);

static bool eval_whole(Interp *it, Expr *e, const char *what, int64_t *out) {
    Value v;
    if (!eval(it, e, &v)) return false;
    bool ok = whole_val(v, what, out);
    v_release(v);
    return ok;
}

static bool binary(Interp *it, Expr *e, Value *out) {
    Op op = e->op;
    bool l, r, res;
    switch (op) {
    case OP_AND:
    case OP_NAND:
        if (!eval_truth(it, e->x, &l)) return false;
        if (l && !eval_truth(it, e->y, &r)) return false;
        res = l && r;
        if (op == OP_NAND) res = !res;
        break;
    case OP_ANDS:
    case OP_NANDS:
        if (!eval_truth(it, e->x, &l) || !eval_truth(it, e->y, &r)) return false;
        res = op == OP_NANDS ? !(l && r) : (l && r);
        break;
    case OP_OR:
    case OP_NOR:
        if (!eval_truth(it, e->x, &l)) return false;
        if (!l && !eval_truth(it, e->y, &r)) return false;
        res = l || r;
        if (op == OP_NOR) res = !res;
        break;
    case OP_ORS:
    case OP_NORS:
        if (!eval_truth(it, e->x, &l) || !eval_truth(it, e->y, &r)) return false;
        res = op == OP_NORS ? !(l || r) : (l || r);
        break;
    case OP_XOR:
    case OP_XNOR:
        if (!eval_truth(it, e->x, &l) || !eval_truth(it, e->y, &r)) return false;
        res = op == OP_XNOR ? l == r : l != r;
        break;
    case OP_EQ2:
    case OP_EQ2N:
    case OP_EQ3:
    case OP_EQ3N:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE:
    case OP_LE2:
    case OP_GE2: {
        Value x, y;
        if (!eval(it, e->x, &x)) return false;
        if (!eval(it, e->y, &y)) {
            v_release(x);
            return false;
        }
        bool ok = true;
        switch (op) {
        case OP_EQ2: res = loose_eq(x, y); break;
        case OP_EQ2N: res = !loose_eq(x, y); break;
        case OP_EQ3: res = strict_eq(x, y); break;
        case OP_EQ3N: res = !strict_eq(x, y); break;
        default: ok = compare_op(op, x, y, &res);
        }
        v_release(x);
        v_release(y);
        if (!ok) return false;
        break;
    }
    default: {
        Value x, y;
        if (!eval(it, e->x, &x)) return false;
        if (!eval(it, e->y, &y)) {
            v_release(x);
            return false;
        }
        bool ok = binop(op, x, y, out);
        v_release(x);
        v_release(y);
        return ok;
    }
    }
    *out = v_bool(res);
    return true;
}

static bool action(Interp *it, bool insert, Expr **args, int nargs, Value *out) {
    const char *name = insert ? "INSERT" : "DROP";
    int want = insert ? 3 : 2;
    if (nargs != want) return fail("%s takes %d inputs, but got %d", name, want, nargs);
    Value extra[2];
    int ne = 0;
    for (int i = 1; i < nargs; i++) {
        if (!eval(it, args[i], &extra[ne])) {
            for (int k = 0; k < ne; k++) v_release(extra[k]);
            return false;
        }
        ne++;
    }
    bool ok;
    if (is_lvalue(args[0])) {
        Slot *s;
        ok = lvalue_slot(it, args[0], &s);
        if (ok) {
            ok = not_const(s) && apply_action(&s->v, insert, extra);
            if (ok) *out = v_copy(v_retain(s->v));
            slot_unref(s);
        }
    } else {
        Value v;
        ok = eval(it, args[0], &v);
        if (ok) {
            ok = apply_action(&v, insert, extra);
            if (ok) *out = v;
            else v_release(v);
        }
    }
    for (int k = 0; k < ne; k++) v_release(extra[k]);
    return ok;
}

static bool call(Interp *it, Expr *e, Value *out) {
    Value f;
    if (!eval(it, e->x, &f)) return false;
    if (f.t == V_BUILTIN && f.bi == GFX_FIRST + 1 && e->nargs == 1 && is_lvalue(e->args[0])) {
        // DRAW_TO(name) of an image: drawing goes into that variable's image.
        Slot *s;
        if (!lvalue_slot(it, e->args[0], &s)) return false;
        if (s->v.t == V_IMAGE) {
            bool ok = gfx_draw_to_slot(it, s);
            slot_unref(s);
            *out = NULL_VALUE;
            return ok;
        }
        slot_unref(s);
    }
    if (f.t == V_BUILTIN) {
        Value *a = xmalloc((size_t)(e->nargs ? e->nargs : 1) * sizeof(Value));
        int n = 0;
        bool ok = true;
        for (int i = 0; i < e->nargs; i++) {
            if (!eval(it, e->args[i], &a[n])) {
                ok = false;
                break;
            }
            n++;
        }
        if (ok) ok = call_builtin(it, f.bi, a, n, out);
        for (int i = 0; i < n; i++) v_release(a[i]);
        free(a);
        return ok;
    }
    if (f.t == V_FUNC) {
        bool ok = call_func(it, f.fn, e->args, e->nargs, out);
        v_release(f);
        return ok;
    }
    v_release(f);
    if (f.t == V_NULL) return fail("can't call NULL like a function");
    return fail("a %s can't be called like a function", v_type(f));
}

static bool eval(Interp *it, Expr *e, Value *out) {
    switch (e->k) {
    case E_NUM: *out = v_num(num_retain(e->num)); return true;
    case E_TEXT: *out = v_text(str_ref(e->name)); return true;
    case E_BOOL: *out = v_bool(e->b); return true;
    case E_NULL: *out = NULL_VALUE; return true;
    case E_VAR: {
        Slot *s = lookup(it, e->name);
        if (!s) return fail("%s isn't defined", e->name->s);
        *out = v_retain(s->v);
        return true;
    }
    case E_BUILTIN:
        out->t = V_BUILTIN;
        out->bi = e->bi;
        return true;
    case E_TABLE: {
        Table *t = table_new();
        for (int i = 0; i < e->nargs; i++) {
            Value v;
            if (!eval(it, e->args[i], &v)) {
                table_unref(t);
                return false;
            }
            v = v_copy(v);
            if (e->keys[i]) om_insert(&t->named, e->keys[i], slot_new(v));
            else table_push(t, v);
        }
        *out = v_table(t);
        return true;
    }
    case E_FIELD: {
        Value bv;
        if (!eval(it, e->x, &bv)) return false;
        bool ok = true;
        switch (bv.t) {
        case V_TABLE: {
            Slot *s = om_gets(&bv.tb.tab->named, e->name);
            *out = s ? v_retain(s->v) : NULL_VALUE;
            break;
        }
        case V_FUNC: {
            Slot *s = func_local(it, bv.fn, e->name);
            if (s) *out = v_retain(s->v);
            else ok = false;
            break;
        }
        case V_IMAGE: case V_WINDOW: case V_FONT: ok = gfx_field(bv, e->name, out); break;
        case V_NULL: ok = fail("can't read .%s of NULL", e->name->s); break;
        default: ok = fail("can't read .%s of a %s", e->name->s, v_type(bv));
        }
        v_release(bv);
        return ok;
    }
    case E_INDEX: {
        Value bv, iv;
        if (!eval(it, e->x, &bv)) return false;
        if (!eval(it, e->y, &iv)) {
            v_release(bv);
            return false;
        }
        bool ok = index_get(bv, iv, out);
        v_release(bv);
        v_release(iv);
        return ok;
    }
    case E_SLICE: {
        Value bv;
        if (!eval(it, e->x, &bv)) return false;
        int64_t a, z, step = 1;
        bool ok = eval_whole(it, e->y, "the slice start", &a) && eval_whole(it, e->z, "the slice end", &z) &&
                  (!e->w || eval_whole(it, e->w, "the slice STEP", &step));
        if (ok) ok = slice(bv, a, z, step, out);
        v_release(bv);
        return ok;
    }
    case E_CALL: return call(it, e, out);
    case E_NEG: {
        Value v;
        if (!eval(it, e->x, &v)) return false;
        bool ok = negate(v, out);
        v_release(v);
        return ok;
    }
    case E_NOT: {
        bool t;
        if (!eval_truth(it, e->x, &t)) return false;
        *out = v_bool(!t);
        return true;
    }
    case E_BINARY: return binary(it, e, out);
    case E_FUNC: *out = make_func(it, e->fn); return true;
    case E_START: return start(it, e->x, out);
    case E_UNDEFINE:
        if (!undefine(it, e->x)) return false;
        *out = NULL_VALUE;
        return true;
    case E_INSERT: return action(it, true, e->args, e->nargs, out);
    case E_DROP: return action(it, false, e->args, e->nargs, out);
    }
    return fail("unknown expression");
}

// ---------- calling functions ----------

static bool run_call(Interp *it, Function *func, Slot **vals, int nvals, Value *out) {
    FuncDecl *d = func->decl;
    int k = 0;
    for (int i = 0; i < d->nparams; i++) {
        Param *p = &d->params[i];
        if (p->rest) {
            Table *t = table_new();
            for (; k < nvals; k++) table_push(t, v_retain(vals[k]->v));
            om_insert(&top(it)->scopes.s[0], p->name, slot_new(v_table(t)));
            break;
        }
        Slot *given = k < nvals ? vals[k++] : NULL;
        bool is_null = !given || given->v.t == V_NULL;
        Slot *slot;
        if (!is_null) {
            slot = slot_ref(given);
        } else if (p->def) {
            Value v;
            if (!eval(it, p->def, &v)) return false;
            slot = slot_new(v_copy(v));
        } else if (given) {
            slot = slot_ref(given);
        } else {
            return fail("%s is required (it's an input of %s)", p->name->s, fname(func));
        }
        om_insert(&top(it)->scopes.s[0], p->name, slot);
    }
    Flow f = exec_block(it, &d->body);
    if (f == F_ERR) return false;
    if (f == F_RETURN) {
        *out = it->ret;
        it->ret = NULL_VALUE;
        return true;
    }
    if (f == F_GOTO) {
        Str *n = it->goto_name;
        it->goto_name = NULL;
        bool ok = goto_error(it, n, it->goto_line);
        str_unref(n);
        return ok;
    }
    *out = NULL_VALUE;
    return true;
}

/// A call's inputs: copies, or shared slots for @ inputs (the caller unrefs them and frees the list).
static bool call_args(Interp *it, Function *func, Expr **args, int nargs, Slot ***vals_out, int *n_out) {
    FuncDecl *d = func->decl;
    bool has_rest = d->nparams && d->params[d->nparams - 1].rest;
    int fixed = d->nparams - (has_rest ? 1 : 0);
    *vals_out = NULL;
    *n_out = 0;
    if (nargs > fixed && !has_rest) return fail("%s takes at most %d input%s, but got %d", fname(func), fixed, fixed == 1 ? "" : "s", nargs);
    Slot **vals = xmalloc((size_t)(nargs ? nargs : 1) * sizeof(Slot *));
    int n = 0;
    bool ok = true;
    for (int i = 0; i < nargs; i++) {
        if (i < fixed && d->params[i].shared) {
            if (!share_slot(it, args[i], &vals[n])) {
                ok = false;
                break;
            }
        } else {
            Value v;
            if (!eval(it, args[i], &v)) {
                ok = false;
                break;
            }
            vals[n] = slot_new(v_copy(v));
        }
        n++;
    }
    *vals_out = vals;
    *n_out = n;
    return ok;
}

static bool call_func(Interp *it, Function *func, Expr **args, int nargs, Value *out) {
    Slot **vals;
    int n;
    bool ok = call_args(it, func, args, nargs, &vals, &n);
    if (ok && it->nframes >= MAX_DEPTH) ok = fail("too many function calls inside each other (is a function calling itself forever?)");
    if (ok) {
        int line = top(it)->line;
        push_frame(it, func, func->env, line, func->src);
        scopes_push(&top(it)->scopes);
        ok = run_call(it, func, vals, n, out);
        pop_frame(it);
    }
    for (int i = 0; i < n; i++) slot_unref(vals[i]);
    free(vals);
    return ok;
}

// For tasks.c: a task's first call, made in its own frame list.
void frame_push(Interp *it, Function *f, Env *env, int line, Source *src) {
    push_frame(it, f, env, line, src);
    scopes_push(&top(it)->scopes);
}

void frame_pop(Interp *it) {
    pop_frame(it);
}

bool call_prepared(Interp *it, Function *func, Slot **vals, int nvals, Value *out) {
    return run_call(it, func, vals, nvals, out);
}

/// START f(x): works out f and its inputs here, then runs it as a task.
static bool start(Interp *it, Expr *call, Value *out) {
    Value f;
    if (!eval(it, call->x, &f)) return false;
    if (f.t == V_BUILTIN) return fail("START runs your own functions, not built-in commands like %s", WORDS[f.bi]);
    if (f.t != V_FUNC) {
        bool r = fail("START needs a function, not a %s", v_type(f));
        v_release(f);
        return r;
    }
    Slot **vals;
    int n;
    if (!call_args(it, f.fn, call->args, call->nargs, &vals, &n)) {
        for (int i = 0; i < n; i++) slot_unref(vals[i]);
        free(vals);
        v_release(f);
        return false;
    }
    bool ok = task_start(it, f.fn, vals, n, top(it)->line, out);
    v_release(f);
    return ok;
}

// ---------- files ----------

/// Reads and parses a file. On failure the error is set (a syntax error is already placed).
static bool load_program(const char *path, Block *prog, Source **src) {
    char *text, *err;
    size_t len;
    if (!read_file(path, &text, &len, &err)) {
        fail("couldn't open %s: %s", path, err);
        free(err);
        return false;
    }
    char *name = path_file_name(path);
    char *dir = path_parent(path);
    SyntaxError se;
    bool ok = parse_text(text, len, prog, &se);
    free(text);
    if (!ok) {
        fail_str(se.msg);
        g_err->line = se.line;
        free(g_err->file);
        g_err->file = name;
        free(g_err->path);
        g_err->path = xstrdup(path);
        g_err->captured = true;
        free(dir);
        return false;
    }
    *src = source_new(name, dir);
    free(name);
    free(dir);
    return true;
}

static bool run_module(Interp *it, const char *path, Env *env) {
    Block prog;
    Source *src;
    if (!load_program(path, &prog, &src)) return false;
    if (it->nframes > 200) {
        source_unref(src);
        return fail("too many files loading each other");
    }
    push_frame(it, NULL, env, 0, src);
    source_unref(src);
    Flow f = exec_block(it, &prog);
    bool ok = f != F_ERR;
    if (f == F_GOTO) {
        Str *n = it->goto_name;
        it->goto_name = NULL;
        ok = goto_error(it, n, it->goto_line);
        str_unref(n);
    }
    pop_frame(it);
    return ok;
}

static char *file_path(Interp *it, Str *p) {
    char *joined = path_join(top(it)->src->dir, p->s);
    char *c = path_canonical(joined);
    free(joined);
    if (!c) fail("couldn't find the file %s", p->s);
    return c;
}

static bool do_import(Interp *it, Str *p) {
    char *path = file_path(it, p);
    if (!path) return false;
    for (int i = 0; i < it->nimported; i++) {
        if (strcmp(it->imported[i], path) == 0) {
            free(path);
            return true;
        }
    }
    it->imported = xrealloc(it->imported, (size_t)(it->nimported + 1) * sizeof(char *));
    it->imported[it->nimported++] = xstrdup(path);
    bool ok = run_module(it, path, top(it)->env);
    free(path);
    return ok;
}

static bool do_load(Interp *it, Str *p) {
    char *path = file_path(it, p);
    if (!path) return false;
    char *stem = path_stem(path);
    for (char *c = stem; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        if (*c == '-' || *c == ' ') *c = '_';
    }
    if (!is_ident(stem, strlen(stem))) {
        free(stem);
        free(path);
        return fail("LOAD can't make a name out of the file name %s", p->s);
    }
    Value v = NULL_VALUE;
    bool found = false;
    for (int i = 0; i < it->nloaded; i++) {
        if (strcmp(it->loaded[i].path, path) == 0) {
            v = v_retain(it->loaded[i].v);
            found = true;
            break;
        }
    }
    if (!found) {
        Env *env = env_new();
        if (!run_module(it, path, env)) {
            env_unref(env);
            free(stem);
            free(path);
            return false;
        }
        Table *t = table_new();
        for (size_t i = 0; i < env->map.n; i++) om_insert(&t->named, env->map.e[i].key, slot_new(v_retain(env->map.e[i].slot->v)));
        env_unref(env);
        v = v_table(t);
        it->loaded = xrealloc(it->loaded, (size_t)(it->nloaded + 1) * sizeof(Loaded));
        it->loaded[it->nloaded].path = xstrdup(path);
        it->loaded[it->nloaded].v = v_retain(v);
        it->nloaded++;
    }
    Str *name = str_c(stem);
    bool ok = set_var(it, name, v_copy(v));
    str_unref(name);
    free(stem);
    free(path);
    return ok;
}

int run_main(Interp *it, const char *path) {
    Env *env = env_new();
    char *c = path_canonical(path);
    if (c) {
        it->imported = xrealloc(it->imported, (size_t)(it->nimported + 1) * sizeof(char *));
        it->imported[it->nimported++] = c;
    }
    // When the program's own code ends, the tasks it started get to finish.
    bool ok = run_module(it, path, env) && tasks_finish(it) && gfx_finish(it);
    env_unref(env);
    out_flush();
    if (ok || g_fault == FAULT_END || g_fault == FAULT_CANCELLED) {
        g_fault = FAULT_NONE;
        return 0;
    }
    ErrorInfo *e = take_error();
    char *r = report(e);
    fprintf(stderr, "%s\n", r);
    free(r);
    if (it->ide) {
        char *j = report_json(e);
        fprintf(stderr, "%c%s\n", MARK, j);
        free(j);
    }
    fflush(stderr);
    error_free(e);
    return 1;
}

// ---------- Quick-try sessions (jlb --repl) ----------

void start_session(Interp *it, const char *dir) {
    Env *env = env_new();
    Source *src = source_new("quick-try", dir);
    push_frame(it, NULL, env, 0, src);
    env_unref(env);
    source_unref(src);
}

static int run_snippet_code(Interp *it, const char *text, Value *out);

int run_snippet(Interp *it, const char *text, Value *out) {
    // Tasks a snippet starts finish before its answer comes back.
    int r = run_snippet_code(it, text, out);
    if (r >= 0 && !tasks_finish(it)) {
        if (r == 1) v_release(*out);
        r = -1;
    }
    if (r < 0) {
        // Stop what's left without losing this error.
        int f = g_fault;
        ErrorInfo *e = take_error();
        tasks_cancel_all(it);
        g_fault = f;
        g_err = e;
    }
    return r;
}

static int run_snippet_code(Interp *it, const char *text, Value *out) {
    // Tidy up after an earlier error, which can leave loop scopes or calls behind.
    while (it->nframes > 1) pop_frame(it);
    scopes_free(&it->frames[0].scopes);
    Block prog;
    SyntaxError se;
    if (!parse_text(text, strlen(text), &prog, &se)) {
        fail_str(se.msg);
        g_err->line = se.line;
        free(g_err->file);
        g_err->file = xstrdup("quick-try");
        g_err->captured = true;
        return -1;
    }
    if (prog.n == 1 && prog.s[0]->k == S_EXPR) {
        Stmt *s = prog.s[0];
        top(it)->line = s->line;
        if (s->e->k == E_VAR) declare(it, s->e->name);
        Value v;
        if (!eval(it, s->e, &v)) {
            capture(it, s->line);
            return -1;
        }
        // A command that gives back nothing (like PRINT) shows no result.
        if (v.t == V_NULL && s->e->k == E_CALL) return 0;
        *out = v;
        return 1;
    }
    Flow f = exec_block(it, &prog);
    if (f == F_ERR) return -1;
    if (f == F_GOTO) {
        Str *n = it->goto_name;
        it->goto_name = NULL;
        goto_error(it, n, it->goto_line);
        str_unref(n);
        return -1;
    }
    return 0;
}
