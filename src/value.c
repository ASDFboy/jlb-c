// Runtime values, tables, errors and formatting.

#include "jlb.h"

const Value NULL_VALUE = {.t = V_NULL};

int g_fault = FAULT_NONE;
ErrorInfo *g_err = NULL;

static uint64_t next_id = 1;

/// Every logical table copy gets its own identity, so `===` can tell a copy
/// from the same shared table even while copy-on-write keeps the data shared.
uint64_t new_id(void) {
    return next_id++;
}

// ---------- values ----------

Value v_num(Number n) {
    Value v;
    v.t = V_NUM;
    v.num = n;
    return v;
}

Value v_int(int64_t i) {
    return v_num(num_int(i));
}

Value v_text(Str *s) {
    Value v;
    v.t = V_TEXT;
    v.s = s;
    return v;
}

Value v_textc(const char *s) {
    return v_text(str_c(s));
}

Value v_textn(const char *s, size_t n) {
    return v_text(str_new(s, n));
}

Value v_bool(bool b) {
    Value v;
    v.t = V_BOOL;
    v.b = b;
    return v;
}

Value v_table(Table *t) {
    Value v;
    v.t = V_TABLE;
    v.tb.tab = t;
    v.tb.id = new_id();
    return v;
}

Value v_retain(Value v) {
    switch (v.t) {
    case V_NUM: num_retain(v.num); break;
    case V_TEXT: v.s->rc++; break;
    case V_TABLE: v.tb.tab->rc++; break;
    case V_FUNC: v.fn->rc++; break;
    case V_TASK: v.task.name->rc++; break;
    default: break;
    }
    return v;
}

void v_release(Value v) {
    switch (v.t) {
    case V_NUM: num_release(v.num); break;
    case V_TEXT: str_unref(v.s); break;
    case V_TABLE: table_unref(v.tb.tab); break;
    case V_FUNC: func_unref(v.fn); break;
    case V_TASK: str_unref(v.task.name); break;
    default: break;
    }
}

Value v_copy(Value v) {
    if (v.t == V_TABLE) v.tb.id = new_id();
    return v;
}

const char *v_type(Value v) {
    switch (v.t) {
    case V_NUM: return "NUMBER";
    case V_TEXT: return "TEXT";
    case V_BOOL: return "BOOL";
    case V_NULL: return "NULL";
    case V_TABLE: return "TABLE";
    case V_TASK: return "TASK";
    default: return "FUNC";
    }
}

bool v_is_func(Value v) {
    return v.t == V_FUNC || v.t == V_BUILTIN;
}

Slot *slot_new(Value v) {
    Slot *s = xmalloc(sizeof(Slot));
    s->rc = 1;
    s->constant = false;
    s->v = v;
    return s;
}

static Value freeze(Value v) {
    if (v.t != V_TABLE) return v;
    Table *src = v.tb.tab;
    Table *t = table_new();
    for (size_t i = 0; i < src->n; i++) table_push_slot(t, slot_const(v_retain(src->items[i]->v)));
    for (size_t i = 0; i < src->named.n; i++) om_insert(&t->named, src->named.e[i].key, slot_const(v_retain(src->named.e[i].slot->v)));
    if (src->report) t->report = str_ref(src->report);
    v_release(v);
    return v_table(t);
}

Slot *slot_const(Value v) {
    Slot *s = slot_new(freeze(v));
    s->constant = true;
    return s;
}

void slot_unref(Slot *s) {
    if (s && --s->rc == 0) {
        v_release(s->v);
        free(s);
    }
}

// ---------- ordered maps ----------

void om_init(OrdMap *m) {
    memset(m, 0, sizeof *m);
}

void om_free(OrdMap *m) {
    for (size_t i = 0; i < m->n; i++) {
        str_unref(m->e[i].key);
        slot_unref(m->e[i].slot);
    }
    free(m->e);
    free(m->idx);
    om_init(m);
}

static void om_reindex(OrdMap *m) {
    size_t cap = 8;
    while (cap < m->n * 2 + 2) cap *= 2;
    free(m->idx);
    m->idx = xcalloc(cap, sizeof(uint32_t));
    m->icap = cap;
    for (size_t i = 0; i < m->n; i++) {
        size_t h = m->e[i].key->hash & (cap - 1);
        while (m->idx[h]) h = (h + 1) & (cap - 1);
        m->idx[h] = (uint32_t)i + 1;
    }
}

static long om_find(const OrdMap *m, const char *k, size_t klen) {
    if (m->n == 0) return -1;
    if (!m->idx) {
        for (size_t i = 0; i < m->n; i++) {
            Str *key = m->e[i].key;
            if (key->len == klen && memcmp(key->s, k, klen) == 0) return (long)i;
        }
        return -1;
    }
    uint32_t hv = hash_bytes(k, klen);
    size_t h = hv & (m->icap - 1);
    while (m->idx[h]) {
        size_t i = m->idx[h] - 1;
        Str *key = m->e[i].key;
        if (key->hash == hv && key->len == klen && memcmp(key->s, k, klen) == 0) return (long)i;
        h = (h + 1) & (m->icap - 1);
    }
    return -1;
}

Slot *om_get(const OrdMap *m, const char *k, size_t klen) {
    long i = om_find(m, k, klen);
    return i < 0 ? NULL : m->e[i].slot;
}

void om_insert(OrdMap *m, Str *key, Slot *s) {
    long i = om_find(m, key->s, key->len);
    if (i >= 0) {
        slot_unref(m->e[i].slot);
        m->e[i].slot = s;
        return;
    }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 4;
        m->e = xrealloc(m->e, m->cap * sizeof(Entry));
    }
    m->e[m->n].key = str_ref(key);
    m->e[m->n].slot = s;
    m->n++;
    // Small maps are searched in order; bigger ones get a hash index.
    if (m->idx) {
        if (m->n * 2 + 2 > m->icap) {
            om_reindex(m);
        } else {
            size_t h = key->hash & (m->icap - 1);
            while (m->idx[h]) h = (h + 1) & (m->icap - 1);
            m->idx[h] = (uint32_t)m->n;
        }
    } else if (m->n > 4) {
        om_reindex(m);
    }
}

Slot *om_remove(OrdMap *m, const char *k, size_t klen) {
    long i = om_find(m, k, klen);
    if (i < 0) return NULL;
    Slot *s = m->e[i].slot;
    str_unref(m->e[i].key);
    memmove(&m->e[i], &m->e[i + 1], (m->n - (size_t)i - 1) * sizeof(Entry));
    m->n--;
    if (m->idx) om_reindex(m);
    return s;
}

void om_deep_clone(const OrdMap *src, OrdMap *dst) {
    om_init(dst);
    for (size_t i = 0; i < src->n; i++) om_insert(dst, src->e[i].key, slot_new(v_retain(src->e[i].slot->v)));
}

// ---------- tables ----------

Table *table_new(void) {
    Table *t = xcalloc(1, sizeof(Table));
    t->rc = 1;
    return t;
}

void table_unref(Table *t) {
    if (!t || --t->rc > 0) return;
    for (size_t i = 0; i < t->n; i++) slot_unref(t->items[i]);
    free(t->items);
    om_free(&t->named);
    str_unref(t->report);
    free(t);
}

void table_push_slot(Table *t, Slot *s) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 4;
        t->items = xrealloc(t->items, t->cap * sizeof(Slot *));
    }
    t->items[t->n++] = s;
}

void table_push(Table *t, Value v) {
    table_push_slot(t, slot_new(v));
}

void table_insert_slot(Table *t, size_t i, Slot *s) {
    table_push_slot(t, s);
    memmove(&t->items[i + 1], &t->items[i], (t->n - 1 - i) * sizeof(Slot *));
    t->items[i] = s;
}

void table_remove(Table *t, size_t i) {
    slot_unref(t->items[i]);
    memmove(&t->items[i], &t->items[i + 1], (t->n - i - 1) * sizeof(Slot *));
    t->n--;
}

/// A copy gets fresh slots, so changing the copy never touches the original.
static Table *table_clone(const Table *t) {
    Table *c = table_new();
    c->items = xmalloc((t->n ? t->n : 1) * sizeof(Slot *));
    c->cap = t->n ? t->n : 1;
    for (size_t i = 0; i < t->n; i++) c->items[i] = slot_new(v_retain(t->items[i]->v));
    c->n = t->n;
    om_deep_clone(&t->named, &c->named);
    c->report = str_ref(t->report);
    return c;
}

Table *table_mut(Value *v) {
    Table *t = v->tb.tab;
    if (t->rc > 1) {
        Table *c = table_clone(t);
        t->rc--;
        v->tb.tab = c;
        return c;
    }
    return t;
}

Env *env_new(void) {
    Env *e = xcalloc(1, sizeof(Env));
    e->rc = 1;
    return e;
}

void env_unref(Env *e) {
    if (e && --e->rc == 0) {
        om_free(&e->map);
        free(e);
    }
}

Source *source_new(const char *name, const char *dir) {
    Source *s = xmalloc(sizeof(Source));
    s->rc = 1;
    s->name = xstrdup(name);
    s->dir = xstrdup(dir);
    return s;
}

void source_unref(Source *s) {
    if (s && --s->rc == 0) {
        free(s->name);
        free(s->dir);
        free(s);
    }
}

Function *func_new(FuncDecl *d, Env *env, Source *src) {
    Function *f = xmalloc(sizeof(Function));
    f->rc = 1;
    f->decl = d;
    f->env = env;
    env->rc++;
    f->src = src;
    src->rc++;
    return f;
}

void func_unref(Function *f) {
    if (f && --f->rc == 0) {
        env_unref(f->env);
        source_unref(f->src);
        free(f);
    }
}

// ---------- formatting ----------

bool is_ident(const char *s, size_t len) {
    if (len == 0) return false;
    if (!((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return false;
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

void quote_text(Buf *b, const char *s, size_t len) {
    if (!memchr(s, '"', len)) {
        buf_putc(b, '"');
        buf_put(b, s, len);
        buf_putc(b, '"');
        return;
    }
    buf_putc(b, '\'');
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
        case '\'': buf_puts(b, "\\'"); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default: buf_putc(b, s[i]);
        }
    }
    buf_putc(b, '\'');
}

extern const char *const WORDS[];

void write_value(Buf *b, Value v, bool quote, int depth) {
    switch (v.t) {
    case V_NUM: num_write(b, v.num); break;
    case V_TEXT:
        if (quote) quote_text(b, v.s->s, v.s->len);
        else buf_put(b, v.s->s, v.s->len);
        break;
    case V_BOOL: buf_puts(b, v.b ? "TRUE" : "FALSE"); break;
    case V_NULL: buf_puts(b, "NULL"); break;
    case V_FUNC:
        if (v.fn->decl->name) {
            buf_puts(b, "FUNC ");
            buf_put(b, v.fn->decl->name->s, v.fn->decl->name->len);
        } else {
            buf_puts(b, "FUNC");
        }
        break;
    case V_BUILTIN: buf_puts(b, WORDS[v.bi]); break;
    case V_TASK:
        buf_puts(b, "TASK ");
        buf_puts(b, v.task.name->s);
        break;
    case V_TABLE: {
        Table *t = v.tb.tab;
        if (t->report) {
            buf_put(b, t->report->s, t->report->len);
            return;
        }
        if (depth > 40) {
            buf_puts(b, "[...]");
            return;
        }
        buf_putc(b, '[');
        bool first = true;
        for (size_t i = 0; i < t->n; i++) {
            if (!first) buf_puts(b, ", ");
            first = false;
            write_value(b, t->items[i]->v, true, depth + 1);
        }
        for (size_t i = 0; i < t->named.n; i++) {
            if (!first) buf_puts(b, ", ");
            first = false;
            Str *k = t->named.e[i].key;
            if (is_ident(k->s, k->len)) buf_put(b, k->s, k->len);
            else quote_text(b, k->s, k->len);
            buf_puts(b, ": ");
            write_value(b, t->named.e[i].slot->v, true, depth + 1);
        }
        buf_putc(b, ']');
        break;
    }
    }
}

char *display(Value v) {
    Buf b;
    buf_init(&b);
    write_value(&b, v, false, 0);
    return buf_take(&b);
}

char *repr(Value v) {
    Buf b;
    buf_init(&b);
    write_value(&b, v, true, 0);
    return buf_take(&b);
}

// ---------- errors ----------

static ErrorInfo *error_new(char *msg) {
    ErrorInfo *e = xcalloc(1, sizeof(ErrorInfo));
    e->msg = msg;
    e->file = xstrdup("");
    e->path = xstrdup("");
    return e;
}

bool fail_str(char *msg) {
    if (g_err) error_free(g_err);
    g_err = error_new(msg);
    g_fault = FAULT_ERROR;
    return false;
}

bool fail(const char *fmt, ...) {
    Buf b;
    buf_init(&b);
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(&b, fmt, ap);
    va_end(ap);
    return fail_str(buf_take(&b));
}

ErrorInfo *take_error(void) {
    ErrorInfo *e = g_err;
    g_err = NULL;
    g_fault = FAULT_NONE;
    return e;
}

void varlist_push(VarList *l, Str *name, Value v) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->v = xrealloc(l->v, l->cap * sizeof(Var));
    }
    l->v[l->n].name = str_ref(name);
    l->v[l->n].value = xmalloc(sizeof(Value));
    *l->v[l->n].value = v;
    l->n++;
}

void varlist_free(VarList *l) {
    for (size_t i = 0; i < l->n; i++) {
        str_unref(l->v[i].name);
        v_release(*l->v[i].value);
        free(l->v[i].value);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

/// The variables in a scope, leaving out functions.
void vars_of(const OrdMap *m, VarList *out) {
    for (size_t i = 0; i < m->n; i++) {
        Value v = m->e[i].slot->v;
        if (v_is_func(v)) continue;
        varlist_push(out, m->e[i].key, v_retain(v));
    }
}

void error_free(ErrorInfo *e) {
    if (!e) return;
    free(e->msg);
    free(e->file);
    free(e->path);
    for (size_t i = 0; i < e->nframes; i++) {
        free(e->frames[i].name);
        varlist_free(&e->frames[i].vars);
        free(e->frame_paths[i]);
    }
    free(e->frames);
    free(e->frame_paths);
    varlist_free(&e->outside);
    free(e);
}

static void write_vars(Buf *b, const VarList *l) {
    if (l->n == 0) {
        buf_puts(b, "(none)");
        return;
    }
    for (size_t i = 0; i < l->n; i++) {
        if (i) buf_puts(b, ", ");
        buf_put(b, l->v[i].name->s, l->v[i].name->len);
        buf_puts(b, " = ");
        write_value(b, *l->v[i].value, true, 0);
    }
}

char *report(const ErrorInfo *e) {
    Buf s;
    buf_init(&s);
    buf_printf(&s, "ERROR in %s, line %d: %s", e->file, e->line, e->msg);
    if (!e->traced) return buf_take(&s);
    if (e->nframes) {
        buf_puts(&s, "\n\nCalled from:");
        size_t w = 0;
        for (size_t i = 0; i < e->nframes; i++) {
            size_t n = utf8_count(e->frames[i].name, strlen(e->frames[i].name)) + 2;
            if (n > w) w = n;
        }
        for (size_t i = 0; i < e->nframes; i++) {
            Buf call;
            buf_init(&call);
            buf_printf(&call, "%s()", e->frames[i].name);
            buf_puts(&s, "\n    ");
            buf_pad(&s, call.p, w);
            buf_printf(&s, "  line %d", e->frames[i].line);
            buf_free(&call);
        }
    }
    buf_puts(&s, "\n\nVariables:");
    size_t w = utf8_count("outside:", 8);
    for (size_t i = 0; i < e->nframes; i++) {
        size_t n = utf8_count(e->frames[i].name, strlen(e->frames[i].name)) + 1;
        if (n > w) w = n;
    }
    w += 1;
    for (size_t i = 0; i < e->nframes; i++) {
        Buf label;
        buf_init(&label);
        buf_printf(&label, "%s:", e->frames[i].name);
        buf_puts(&s, "\n    ");
        buf_pad(&s, label.p, w);
        write_vars(&s, &e->frames[i].vars);
        buf_free(&label);
    }
    buf_puts(&s, "\n    ");
    buf_pad(&s, "outside:", w);
    write_vars(&s, &e->outside);
    return buf_take(&s);
}
