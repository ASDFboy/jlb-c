// Operators: math, text and table tricks, comparisons and truth rules.

#include "jlb.h"

#define MAX_LEN 100000000ull

static char *num_text(Number n) {
    return num_str(n);
}

/// The truth rules shared by IF, WHILE and every logic operator.
bool truth(Value v, bool *out) {
    switch (v.t) {
    case V_BOOL: *out = v.b; return true;
    case V_NULL: *out = false; return true;
    case V_NUM:
        if (num_is_int(v.num, 1)) {
            *out = true;
            return true;
        }
        if (num_is_zero(v.num)) {
            *out = false;
            return true;
        }
        {
            char *s = num_text(v.num);
            fail("%s can't be used as TRUE or FALSE (only 1 and 0 can)", s);
            free(s);
            return false;
        }
    case V_TEXT:
        if (str_is(v.s, "1") || str_is(v.s, "TRUE")) {
            *out = true;
            return true;
        }
        if (str_is(v.s, "0") || str_is(v.s, "FALSE")) {
            *out = false;
            return true;
        }
        {
            Buf q;
            buf_init(&q);
            quote_text(&q, v.s->s, v.s->len);
            fail("the text %s can't be used as TRUE or FALSE", q.p);
            buf_free(&q);
            return false;
        }
    default: return fail("a %s can't be used as TRUE or FALSE", v_type(v));
    }
}

static bool whole(Number x, const char *what, int64_t *out) {
    if (!num_is_integer(x)) {
        char *s = num_text(x);
        fail("%s must be a whole number, not %s", what, s);
        free(s);
        return false;
    }
    *out = num_to_i64_sat(x);
    return true;
}

/// Value as a number for math. NULL counts as 0.
bool math_num(Value v, const char *op, Number *out) {
    switch (v.t) {
    case V_NUM: *out = num_retain(v.num); return true;
    case V_NULL: *out = num_int(0); return true;
    case V_TEXT: {
        Buf q;
        buf_init(&q);
        quote_text(&q, v.s->s, v.s->len);
        fail("can't use %s on text (%s); convert it with NUM() first", op, q.p);
        buf_free(&q);
        return false;
    }
    default: return fail("can't use %s on a %s", op, v_type(v));
    }
}

/// Text used when joining with +: NULL adds nothing.
static void join_str(Buf *b, Value v) {
    if (v.t != V_NULL) write_value(b, v, false, 0);
}

/// New table: the given numbered items (taken) plus copies of `t`'s named entries.
static Value rebuild(const Table *t, Value *items, size_t n) {
    Table *r = table_new();
    for (size_t i = 0; i < n; i++) table_push(r, items[i]);
    om_deep_clone(&t->named, &r->named);
    return v_table(r);
}

/// The numbered items of a table, retained.
static Value *items_of(const Table *t) {
    Value *v = xmalloc((t->n ? t->n : 1) * sizeof(Value));
    for (size_t i = 0; i < t->n; i++) v[i] = v_retain(t->items[i]->v);
    return v;
}


static uint64_t abs_count(int64_t n) {
    return n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
}

static bool num_result(bool ok, Number n, Value *out) {
    if (ok) *out = v_num(n);
    return ok;
}

static bool arith(Op op, Value a, Value b, const char *name, Value *out) {
    Number x, y, r;
    if (!math_num(a, name, &x)) return false;
    if (!math_num(b, name, &y)) {
        num_release(x);
        return false;
    }
    bool ok;
    switch (op) {
    case OP_ADD: ok = num_add(x, y, &r); break;
    case OP_SUB: ok = num_sub(x, y, &r); break;
    case OP_MUL: ok = num_mul(x, y, &r); break;
    case OP_DIV: ok = num_div(x, y, &r); break;
    case OP_REM: ok = num_rem(x, y, &r); break;
    case OP_QUOT: ok = num_quot(x, y, &r); break;
    case OP_POW: ok = num_pow(x, y, &r); break;
    default: ok = num_root(x, y, &r); break;
    }
    num_release(x);
    num_release(y);
    return num_result(ok, r, out);
}

/// Replaces every `pat` in s.
static Str *replace_all(const Str *s, const Str *pat) {
    Buf b;
    buf_init(&b);
    size_t i = 0;
    while (i < s->len) {
        if (i + pat->len <= s->len && memcmp(s->s + i, pat->s, pat->len) == 0) {
            i += pat->len;
        } else {
            buf_putc(&b, s->s[i]);
            i++;
        }
    }
    return str_from_buf(&b);
}

/// Repeats characters (reversed for a negative count).
static bool repeat_text(const Str *s, int64_t n, Value *out) {
    uint64_t count = abs_count(n);
    if (s->nchars && count > MAX_LEN / s->nchars) return fail("that would be too big to store");
    if (s->nchars * count > MAX_LEN) return fail("that would be too big to store");
    Buf base;
    buf_init(&base);
    if (n < 0) {
        // Reverse by characters.
        size_t *starts = xmalloc((s->nchars + 1) * sizeof(size_t));
        size_t k = 0;
        for (size_t i = 0; i < s->len;) {
            starts[k++] = i;
            utf8_next(s->s, s->len, &i);
        }
        starts[k] = s->len;
        for (size_t j = k; j-- > 0;) buf_put(&base, s->s + starts[j], starts[j + 1] - starts[j]);
        free(starts);
    } else {
        buf_put(&base, s->s, s->len);
    }
    Buf r;
    buf_init(&r);
    for (uint64_t i = 0; i < count; i++) buf_put(&r, base.p ? base.p : "", base.len);
    buf_free(&base);
    *out = v_text(str_from_buf(&r));
    return true;
}

bool binop(Op op, Value a, Value b, Value *out) {
    switch (op) {
    case OP_ADD:
        if (a.t == V_TABLE && b.t == V_TABLE) {
            Table *t = table_new();
            table_push(t, v_copy(v_retain(a)));
            table_push(t, v_copy(v_retain(b)));
            *out = v_table(t);
            return true;
        }
        if (a.t == V_TABLE) {
            Table *t = a.tb.tab;
            Value *v = items_of(t);
            v = xrealloc(v, (t->n + 1) * sizeof(Value));
            v[t->n] = v_copy(v_retain(b));
            *out = rebuild(t, v, t->n + 1);
            free(v);
            return true;
        }
        if (b.t == V_TABLE) {
            Table *t = b.tb.tab;
            Value *v = xmalloc((t->n + 1) * sizeof(Value));
            v[0] = v_copy(v_retain(a));
            for (size_t i = 0; i < t->n; i++) v[i + 1] = v_retain(t->items[i]->v);
            *out = rebuild(t, v, t->n + 1);
            free(v);
            return true;
        }
        if (a.t == V_TEXT || b.t == V_TEXT) {
            Buf s;
            buf_init(&s);
            join_str(&s, a);
            join_str(&s, b);
            *out = v_text(str_from_buf(&s));
            return true;
        }
        return arith(op, a, b, "+", out);
    case OP_SUB:
        if (a.t == V_TABLE && b.t == V_NUM) {
            int64_t k;
            if (!whole(b.num, "the number of items to remove", &k)) return false;
            Table *t = a.tb.tab;
            Value *v = items_of(t);
            size_t n = t->n;
            if (k >= 0) {
                size_t keep = (uint64_t)k >= n ? 0 : n - (size_t)k;
                for (size_t i = keep; i < n; i++) v_release(v[i]);
                n = keep;
            } else {
                v = xrealloc(v, (n + 1) * sizeof(Value));
                v[n++] = v_num(num_neg(b.num));
            }
            *out = rebuild(t, v, n);
            free(v);
            return true;
        }
        if (a.t == V_TABLE) {
            Table *t = a.tb.tab;
            Value *v = xmalloc((t->n ? t->n : 1) * sizeof(Value));
            size_t n = 0;
            for (size_t i = 0; i < t->n; i++) {
                Value x = t->items[i]->v;
                if (!match_eq(x, b)) v[n++] = v_retain(x);
            }
            *out = rebuild(t, v, n);
            free(v);
            return true;
        }
        if (a.t == V_TEXT && b.t == V_NUM) {
            int64_t k;
            if (!whole(b.num, "the number of characters to trim", &k)) return false;
            if (k >= 0) {
                size_t keep = (uint64_t)k >= a.s->nchars ? 0 : a.s->nchars - (size_t)k;
                size_t off = utf8_offset(a.s->s, a.s->len, keep);
                *out = v_textn(a.s->s, off);
            } else {
                Buf s;
                buf_init(&s);
                buf_put(&s, a.s->s, a.s->len);
                Number nn = num_neg(b.num);
                num_write(&s, nn);
                num_release(nn);
                *out = v_text(str_from_buf(&s));
            }
            return true;
        }
        if (a.t == V_TEXT && b.t == V_TEXT) {
            if (b.s->len == 0) {
                *out = v_retain(a);
                return true;
            }
            *out = v_text(replace_all(a.s, b.s));
            return true;
        }
        if (a.t == V_TEXT && b.t == V_NULL) {
            *out = v_retain(a);
            return true;
        }
        if (a.t == V_TEXT) return fail("can't subtract a %s from text", v_type(b));
        if ((a.t == V_NUM || a.t == V_NULL) && b.t == V_TEXT) return fail("a number minus text has no meaning");
        return arith(op, a, b, "-", out);
    case OP_MUL:
        if ((a.t == V_TABLE && b.t == V_NUM) || (a.t == V_NUM && b.t == V_TABLE)) {
            Table *t = a.t == V_TABLE ? a.tb.tab : b.tb.tab;
            Number nn = a.t == V_TABLE ? b.num : a.num;
            int64_t n;
            if (!whole(nn, "the number of repeats", &n)) return false;
            uint64_t count = abs_count(n);
            if (t->n && count > MAX_LEN / t->n) return fail("that would be too big to store");
            size_t total = t->n * (size_t)count;
            Value *v = xmalloc((total ? total : 1) * sizeof(Value));
            size_t k = 0;
            for (uint64_t r = 0; r < count; r++) {
                for (size_t i = 0; i < t->n; i++) {
                    size_t j = n < 0 ? t->n - 1 - i : i;
                    v[k++] = v_retain(t->items[j]->v);
                }
            }
            *out = rebuild(t, v, k);
            free(v);
            return true;
        }
        if (a.t == V_TABLE || b.t == V_TABLE) return fail("a table can only be multiplied by a number");
        if ((a.t == V_TEXT && b.t == V_NUM) || (a.t == V_NUM && b.t == V_TEXT)) {
            Str *s = a.t == V_TEXT ? a.s : b.s;
            Number n = a.t == V_TEXT ? b.num : a.num;
            if (!num_is_integer(n)) return fail("can't repeat text a fraction of times");
            return repeat_text(s, num_to_i64_sat(n), out);
        }
        if ((a.t == V_TEXT && b.t == V_NULL) || (a.t == V_NULL && b.t == V_TEXT)) {
            *out = v_textc("");
            return true;
        }
        if (a.t == V_TEXT || b.t == V_TEXT) return fail("text can only be repeated a number of times (the repeat count must be a number)");
        return arith(op, a, b, "*", out);
    case OP_DIV:
    case OP_REM:
    case OP_QUOT:
    case OP_POW:
    case OP_ROOT:
        return arith(op, a, b, OP_TEXT[op], out);
    default: return fail("unknown operator %s", OP_TEXT[op]);
    }
}

bool negate(Value v, Value *out) {
    Number n;
    if (!math_num(v, "-", &n)) return false;
    *out = v_num(num_neg(n));
    num_release(n);
    return true;
}

static bool cmp_num(Value v, Number *out) {
    switch (v.t) {
    case V_NUM: *out = num_retain(v.num); return true;
    case V_NULL: *out = num_int(0); return true;
    case V_TEXT:
        if (num_parse_text(v.s->s, v.s->len, out)) return true;
        {
            Buf q;
            buf_init(&q);
            quote_text(&q, v.s->s, v.s->len);
            fail("can't compare the text %s with a number", q.p);
            buf_free(&q);
            return false;
        }
    default: return fail("can't compare a %s with < or >", v_type(v));
    }
}

bool compare_op(Op op, Value a, Value b, bool *out) {
    if (a.t == V_TEXT && b.t == V_TEXT) {
        size_t x = a.s->nchars, y = b.s->nchars;
        bool same = str_eq(a.s, b.s);
        switch (op) {
        case OP_LT: *out = x < y; break;
        case OP_GT: *out = x > y; break;
        case OP_LE: *out = x <= y; break;
        case OP_GE: *out = x >= y; break;
        case OP_LE2: *out = x < y || same; break;
        default: *out = x > y || same; break;
        }
        return true;
    }
    Number x, y;
    if (!cmp_num(a, &x)) return false;
    if (!cmp_num(b, &y)) {
        num_release(x);
        return false;
    }
    int o = num_cmp(x, y);
    num_release(x);
    num_release(y);
    switch (op) {
    case OP_LT: *out = o < 0; break;
    case OP_GT: *out = o > 0; break;
    case OP_LE:
    case OP_LE2: *out = o <= 0; break;
    default: *out = o >= 0; break;
    }
    return true;
}

static bool loose_eq_d(Value a, Value b, int d);

static bool tables_eq(const Table *a, const Table *b, int depth) {
    if (depth > 40) return true;
    if (a->n != b->n || a->named.n != b->named.n) return false;
    for (size_t i = 0; i < a->n; i++)
        if (!loose_eq_d(a->items[i]->v, b->items[i]->v, depth + 1)) return false;
    for (size_t i = 0; i < a->named.n; i++) {
        Slot *o = om_gets(&b->named, a->named.e[i].key);
        if (!o || !loose_eq_d(a->named.e[i].slot->v, o->v, depth + 1)) return false;
    }
    return true;
}

static bool text_num_eq(Str *s, Number n) {
    Number v;
    if (!num_parse_text(s->s, s->len, &v)) return false;
    bool r = num_eq(v, n);
    num_release(v);
    return r;
}

static bool loose_eq_d(Value a, Value b, int d) {
    VType x = a.t, y = b.t;
    if (x == V_NUM && y == V_NUM) return num_eq(a.num, b.num);
    if (x == V_TEXT && y == V_TEXT) return str_eq(a.s, b.s);
    if (x == V_TEXT && y == V_NUM) return text_num_eq(a.s, b.num);
    if (x == V_NUM && y == V_TEXT) return text_num_eq(b.s, a.num);
    if (x == V_BOOL && y == V_BOOL) return a.b == b.b;
    if (x == V_BOOL && y == V_NUM) return num_is_int(b.num, a.b ? 1 : 0);
    if (x == V_NUM && y == V_BOOL) return num_is_int(a.num, b.b ? 1 : 0);
    if ((x == V_BOOL && y == V_TEXT) || (x == V_TEXT && y == V_BOOL)) {
        bool bv = x == V_BOOL ? a.b : b.b;
        Str *s = x == V_TEXT ? a.s : b.s;
        if (str_is(s, "TRUE") || str_is(s, "1")) return bv;
        if (str_is(s, "FALSE") || str_is(s, "0")) return !bv;
        return false;
    }
    if (x == V_NULL && y == V_NULL) return true;
    if (x == V_NULL && y == V_NUM) return num_is_zero(b.num);
    if (x == V_NUM && y == V_NULL) return num_is_zero(a.num);
    if (x == V_NULL && y == V_BOOL) return !b.b;
    if (x == V_BOOL && y == V_NULL) return !a.b;
    if (x == V_TABLE && y == V_TABLE) return a.tb.tab == b.tb.tab || tables_eq(a.tb.tab, b.tb.tab, d);
    if (x == V_FUNC && y == V_FUNC) return a.fn == b.fn;
    if (x == V_BUILTIN && y == V_BUILTIN) return a.bi == b.bi;
    if (x == V_TASK && y == V_TASK) return a.task.id == b.task.id;
    if (x == y && (x == V_IMAGE || x == V_FONT)) return a.gfx.p == b.gfx.p;
    if (x == V_WINDOW && y == V_WINDOW) return a.gfx.id == b.gfx.id;
    return false;
}

bool loose_eq(Value a, Value b) {
    return loose_eq_d(a, b, 0);
}

bool strict_eq(Value a, Value b) {
    if (a.t != b.t) return false;
    switch (a.t) {
    case V_NUM: return num_eq(a.num, b.num);
    case V_TEXT: return str_eq(a.s, b.s);
    case V_BOOL: return a.b == b.b;
    case V_NULL: return true;
    case V_TABLE: return a.tb.id == b.tb.id;
    case V_FUNC: return a.fn == b.fn;
    case V_BUILTIN: return a.bi == b.bi;
    case V_TASK: return a.task.id == b.task.id;
    case V_IMAGE: case V_FONT: return a.gfx.p == b.gfx.p;
    case V_WINDOW: return a.gfx.id == b.gfx.id;
    }
    return false;
}

/// FIND and `table - value`: === for everything except tables, which match by contents.
bool match_eq(Value a, Value b) {
    if (a.t == V_TABLE && b.t == V_TABLE) return loose_eq(a, b);
    return strict_eq(a, b);
}
