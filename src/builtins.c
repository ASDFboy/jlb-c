// Built-in commands (ALL CAPS).

#include <math.h>
#include <wctype.h>

#include "jlb.h"
#include "path.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

void out_write(const char *s, size_t n) {
    fwrite(s, 1, n, stdout);
}

void out_flush(void) {
    fflush(stdout);
}

/// CLEAR: wipes what's been printed. An editor (--ide, --repl) is sent a form feed and
/// clears its output itself; a terminal gets the codes that clear its screen and
/// scrollback. Output going to a file or another program is left alone.
static void clear_output(Interp *it) {
    if (it->ide) {
        out_write("\f", 1);
    } else {
#ifdef _WIN32
        // Windows consoles understand the codes once asked to (Windows 10 and later).
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        if (!GetConsoleMode(h, &mode)) return;
        if (!(mode & 0x0004)) SetConsoleMode(h, mode | 0x0004); // ENABLE_VIRTUAL_TERMINAL_PROCESSING
#else
        if (!isatty(fileno(stdout))) return;
#endif
        out_flush();
        out_write("\033[H\033[2J\033[3J", 11);
    }
    out_flush();
}

static bool count(const char *name, int n, int min, int max) {
    if (n >= min && n <= max) return true;
    char need[64];
    if (min == max) snprintf(need, sizeof need, "%d", min);
    else if (max == 1 << 30) snprintf(need, sizeof need, "at least %d", min);
    else snprintf(need, sizeof need, "%d to %d", min, max);
    const char *plural = min == 1 && max == 1 ? "" : "s";
    return fail("%s takes %s input%s, but got %d", name, need, plural, n);
}
#define ANY (1 << 30)

/// An optional input: missing or NULL both mean "use the default".
static Value *opt(Value *a, int n, int i) {
    return i < n && a[i].t != V_NULL ? &a[i] : NULL;
}

static bool quoted_fail(const char *fmt, Str *s) {
    Buf q;
    buf_init(&q);
    quote_text(&q, s->s, s->len);
    fail(fmt, q.p);
    buf_free(&q);
    return false;
}

static bool number(const char *name, Value v, Number *out) {
    switch (v.t) {
    case V_NUM: *out = num_retain(v.num); return true;
    case V_NULL: *out = num_int(0); return true;
    case V_TEXT: {
        Buf q;
        buf_init(&q);
        quote_text(&q, v.s->s, v.s->len);
        fail("%s needs a number, not the text %s (use NUM() to convert)", name, q.p);
        buf_free(&q);
        return false;
    }
    default: return fail("%s needs a number, not a %s", name, v_type(v));
    }
}

static double number_f64(const char *name, Value v, bool *ok) {
    Number n;
    *ok = number(name, v, &n);
    if (!*ok) return 0;
    double x = num_to_f64(n);
    num_release(n);
    return x;
}

#define MAX_DECIMALS 1000

/// -1 when not given.
static bool decimals(Value *v, long *out) {
    *out = -1;
    if (!v) return true;
    Number d;
    if (!number("decimal places", *v, &d)) return false;
    int64_t x = num_to_i64_sat(d);
    bool okv = num_is_integer(d) && x >= 0 && x <= MAX_DECIMALS;
    num_release(d);
    if (!okv) return fail("decimal places must be a whole number from 0 to %d", MAX_DECIMALS);
    *out = (long)x;
    return true;
}

static bool depth_arg(Value *v, size_t *out) {
    *out = 0;
    if (!v) return true;
    Number d;
    if (!number("depth", *v, &d)) return false;
    bool okv = num_is_integer(d) && !num_is_neg(d);
    int64_t x = num_to_i64_sat(d);
    num_release(d);
    if (!okv) return fail("depth must be a whole number, 0 or more");
    *out = (size_t)x;
    return true;
}

static bool table_arg(const char *name, Value v, Table **out) {
    if (v.t != V_TABLE) return fail("%s needs a table, not a %s", name, v_type(v));
    *out = v.tb.tab;
    return true;
}

static Value text_take(char *s) {
    Value v = v_textc(s);
    free(s);
    return v;
}

/// Degree trig results are tidied to 12 decimals, so COS(90) is exactly 0 and SIN(30) exactly 0.5.
static bool snap(double x, Number *out) {
    return num_from_f64_rounded(x, 12, out);
}

static bool to_num(Value v, Value *out) {
    switch (v.t) {
    case V_NUM: *out = v_num(num_retain(v.num)); return true;
    case V_TEXT: {
        Number n;
        if (num_parse_text(v.s->s, v.s->len, &n)) {
            *out = v_num(n);
            return true;
        }
        return quoted_fail("can't convert %s to a number", v.s);
    }
    case V_BOOL: *out = v_int(v.b ? 1 : 0); return true;
    case V_NULL: *out = v_int(0); return true;
    default: return fail("can't convert a %s to a number", v_type(v));
    }
}

static uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t splitmix(uint64_t *state) {
    *state += 0x9E3779B97F4A7C15ull;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t f64_bits(double x) {
    uint64_t b;
    memcpy(&b, &x, 8);
    return b;
}

static bool random_cmd(Interp *it, Value *a, int n, Value *out) {
    if (!count("RANDOM", n, 2, 4)) return false;
    Number lo, hi;
    if (!number("RANDOM", a[0], &lo)) return false;
    if (!number("RANDOM", a[1], &hi)) {
        num_release(lo);
        return false;
    }
    if (num_cmp(lo, hi) > 0) {
        Number t = lo;
        lo = hi;
        hi = t;
    }
    long dl;
    bool ok = decimals(opt(a, n, 2), &dl);
    size_t d = 0;
    Number scale = num_int(0), steps = num_int(0), diff, prod, k, frac, sum;
    if (!ok) goto bad;
    if (dl >= 0) d = (size_t)dl;
    else {
        size_t x = num_decimal_places(lo), y = num_decimal_places(hi);
        d = x > y ? x : y;
    }
    if (!num_pow(num_int(10), num_int((int64_t)d), &scale)) goto bad;
    if (!num_sub(hi, lo, &diff)) goto bad;
    ok = num_mul(diff, scale, &prod);
    num_release(diff);
    if (!ok) goto bad;
    ok = num_round(prod, 0, ROUND_HALF, &steps);
    num_release(prod);
    if (!ok) goto bad;
    if (num_cmp(steps, num_int(9000000000000000ll)) > 0) {
        fail("that RANDOM range is too big for that many decimal places");
        goto bad;
    }
    {
        uint64_t st = (uint64_t)num_to_i64_sat(steps);
        uint64_t r;
        Value *seed = opt(a, n, 3);
        if (seed) {
            bool sok;
            double s = number_f64("the RANDOM seed", *seed, &sok);
            if (!sok) goto bad;
            uint64_t state = f64_bits(s) ^ rotl(f64_bits(num_to_f64(lo)), 13) ^ rotl(f64_bits(num_to_f64(hi)), 29) ^ rotl((uint64_t)d, 47);
            splitmix(&state);
            r = splitmix(&state);
        } else {
            r = splitmix(&it->rng);
        }
        k = num_int((int64_t)(r % (st + 1)));
    }
    if (!num_div(k, scale, &frac)) goto bad;
    ok = num_add(lo, frac, &sum);
    num_release(frac);
    if (!ok) goto bad;
    {
        Number res;
        ok = num_round(sum, d, ROUND_HALF, &res);
        num_release(sum);
        if (!ok) goto bad;
        *out = v_num(res);
    }
    num_release(lo);
    num_release(hi);
    num_release(scale);
    num_release(steps);
    return true;
bad:
    num_release(lo);
    num_release(hi);
    num_release(scale);
    num_release(steps);
    return false;
}

static const double PI_F = 3.14159265358979323846264338327950288;

static double rem_euclid(double x, double m) {
    double r = fmod(x, m);
    return r < 0.0 ? r + fabs(m) : r;
}

static bool trig(const char *name, Number xn, Number *out) {
    double x = num_to_f64(xn);
    bool rad = strncmp(name, "RAD_", 4) == 0;
    const char *base = rad ? name + 4 : name;
    double r = rad ? x : rem_euclid(x, 360.0) * (PI_F / 180.0);
    double s = sin(r), c = cos(r);
#define TINY(v) (fabs(v) < 1e-12)
    double o;
    char *xs = NULL;
#define UNDEFINED()                                    \
    do {                                               \
        xs = num_str(xn);                              \
        fail("%s is undefined at %s", name, xs);       \
        free(xs);                                      \
        return false;                                  \
    } while (0)
    if (!strcmp(base, "SIN")) o = s;
    else if (!strcmp(base, "COS")) o = c;
    else if (!strcmp(base, "TAN")) {
        if (TINY(c)) UNDEFINED();
        o = s / c;
    } else if (!strcmp(base, "CSC")) {
        if (TINY(s)) UNDEFINED();
        o = 1.0 / s;
    } else if (!strcmp(base, "SEC")) {
        if (TINY(c)) UNDEFINED();
        o = 1.0 / c;
    } else if (!strcmp(base, "COT")) {
        if (TINY(s)) UNDEFINED();
        o = c / s;
    } else if (!strcmp(base, "ASIN") || !strcmp(base, "ACOS")) {
        if (!(x >= -1.0 && x <= 1.0)) return fail("%s only works on numbers from -1 to 1", name);
        double av = !strcmp(base, "ASIN") ? asin(x) : acos(x);
        o = rad ? av : av * (180.0 / PI_F);
    } else {
        return fail("unknown trig function %s", name);
    }
#undef TINY
#undef UNDEFINED
    // Degree results are tidied (COS(90) is exactly 0); radian results keep full precision.
    return rad ? num_from_f64(o, out) : snap(o, out);
}

static bool extreme(const char *name, Value *vals, size_t n, bool want_max, Value *out) {
    long best = -1;
    for (size_t i = 0; i < n; i++) {
        if (vals[i].t == V_TABLE) return fail("%s works on single values; use TABLE_%s for a table", name, name);
        if (best < 0) {
            best = (long)i;
            continue;
        }
        bool better;
        if (!compare_op(want_max ? OP_GT : OP_LT, vals[i], vals[best], &better)) return false;
        if (better) best = (long)i;
    }
    *out = best < 0 ? NULL_VALUE : v_retain(vals[best]);
    return true;
}

typedef struct {
    Value *v;
    size_t n, cap;
} Values;

static void values_push(Values *l, Value v) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, l->cap * sizeof(Value));
    }
    l->v[l->n++] = v;
}

static void values_free(Values *l) {
    for (size_t i = 0; i < l->n; i++) v_release(l->v[i]);
    free(l->v);
}

static void collect_values(Table *t, size_t depth, Values *out) {
    for (size_t i = 0; i < t->n + t->named.n; i++) {
        Value v = i < t->n ? t->items[i]->v : t->named.e[i - t->n].slot->v;
        if (v.t == V_TABLE) {
            if (depth > 0) collect_values(v.tb.tab, depth - 1, out);
        } else {
            values_push(out, v_retain(v));
        }
    }
}

static bool find_path(Table *t, Value v, size_t depth, Values *path) {
    for (size_t i = 0; i < t->n; i++) {
        if (match_eq(t->items[i]->v, v)) {
            values_push(path, v_int((int64_t)i + 1));
            return true;
        }
    }
    for (size_t i = 0; i < t->named.n; i++) {
        if (match_eq(t->named.e[i].slot->v, v)) {
            values_push(path, v_text(str_ref(t->named.e[i].key)));
            return true;
        }
    }
    if (depth > 0) {
        for (size_t i = 0; i < t->n + t->named.n; i++) {
            Value sv = i < t->n ? t->items[i]->v : t->named.e[i - t->n].slot->v;
            if (sv.t != V_TABLE) continue;
            Values sub = {0};
            if (find_path(sv.tb.tab, v, depth - 1, &sub)) {
                values_push(path, i < t->n ? v_int((int64_t)i + 1) : v_text(str_ref(t->named.e[i - t->n].key)));
                for (size_t k = 0; k < sub.n; k++) values_push(path, sub.v[k]);
                free(sub.v);
                return true;
            }
            values_free(&sub);
        }
    }
    return false;
}

static Value with_named(Table *t, Value *items, size_t n) {
    Table *r = table_new();
    for (size_t i = 0; i < n; i++) table_push(r, items[i]);
    om_deep_clone(&t->named, &r->named);
    return v_table(r);
}

/// Stable merge sort of indexes with a comparison that can't fail.
typedef int (*Cmp)(const void *ctx, size_t a, size_t b);

static void merge_sort(size_t *ix, size_t *tmp, size_t n, Cmp cmp, const void *ctx) {
    if (n < 2) return;
    size_t m = n / 2;
    merge_sort(ix, tmp, m, cmp, ctx);
    merge_sort(ix + m, tmp, n - m, cmp, ctx);
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) tmp[k++] = cmp(ctx, ix[j], ix[i]) < 0 ? ix[j++] : ix[i++];
    while (i < m) tmp[k++] = ix[i++];
    while (j < n) tmp[k++] = ix[j++];
    memcpy(ix, tmp, n * sizeof(size_t));
}

static int cmp_len(const void *ctx, size_t a, size_t b) {
    const Value *v = ctx;
    size_t x = v[a].s->nchars, y = v[b].s->nchars;
    return x < y ? -1 : x > y;
}

static int cmp_num(const void *ctx, size_t a, size_t b) {
    const Number *k = ctx;
    return num_cmp(k[a], k[b]);
}

typedef struct {
    char **text;
    char **lower;
} AlphaKeys;

static int cmp_alpha(const void *ctx, size_t a, size_t b) {
    const AlphaKeys *k = ctx;
    int c = strcmp(k->lower[a], k->lower[b]);
    if (c) return c;
    return strcmp(k->text[a], k->text[b]);
}

/// Letters that become more than one letter in capitals (Unicode's special casing).
static const char *upper_special(uint32_t c) {
    switch (c) {
    case 0xDF: return "SS";
    case 0xFB00: return "FF";
    case 0xFB01: return "FI";
    case 0xFB02: return "FL";
    case 0xFB03: return "FFI";
    case 0xFB04: return "FFL";
    case 0xFB05:
    case 0xFB06: return "ST";
    case 0x149: return "\xCA\xBCN";
    case 0x1F0: return "J\xCC\x8C";
    case 0x587: return "\xD4\xB5\xD5\x92";
    default: return NULL;
    }
}

static bool cased(uint32_t c) {
    return c < 0x80 ? ((c | 32) >= 'a' && (c | 32) <= 'z') : (bool)iswalpha((wint_t)c);
}

static char *case_map(const char *s, bool upper) {
    Buf b;
    buf_init(&b);
    size_t len = strlen(s);
    uint32_t prev = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = utf8_next(s, len, &i);
        uint32_t orig = c;
        if (c < 0x80) {
            if (upper && c >= 'a' && c <= 'z') c -= 32;
            else if (!upper && c >= 'A' && c <= 'Z') c += 32;
        } else if (upper && upper_special(c)) {
            buf_puts(&b, upper_special(c));
            prev = orig;
            continue;
        } else if (!upper && c == 0x130) {
            buf_puts(&b, "i\xCC\x87"); // I with a dot: i and a combining dot
            prev = orig;
            continue;
        } else if (!upper && c == 0x3A3) {
            // A capital sigma ending a word becomes a final sigma.
            size_t j = i;
            uint32_t next = j < len ? utf8_next(s, len, &j) : 0;
            c = prev && cased(prev) && !(next && cased(next)) ? 0x3C2 : 0x3C3;
        } else {
            c = upper ? (uint32_t)towupper((wint_t)c) : (uint32_t)towlower((wint_t)c);
        }
        buf_utf8(&b, c);
        prev = orig;
    }
    return buf_take(&b);
}

static bool sort_cmd(Value *a, int n, Value *out) {
    if (!count("SORT", n, 1, 1)) return false;
    Table *t = NULL;
    if (!table_arg("SORT", a[0], &t)) return false;
    size_t m = t->n;
    Value *vals = xmalloc((m ? m : 1) * sizeof(Value));
    bool all_text = true;
    for (size_t i = 0; i < m; i++) {
        vals[i] = t->items[i]->v;
        if (vals[i].t != V_TEXT) all_text = false;
    }
    size_t *ix = xmalloc((m ? m : 1) * sizeof(size_t));
    size_t *tmp = xmalloc((m ? m : 1) * sizeof(size_t));
    for (size_t i = 0; i < m; i++) ix[i] = i;
    bool ok = true;
    if (all_text) {
        merge_sort(ix, tmp, m, cmp_len, vals);
    } else {
        Number *keys = xmalloc((m ? m : 1) * sizeof(Number));
        size_t made = 0;
        for (size_t i = 0; i < m && ok; i++) {
            Value v = vals[i];
            switch (v.t) {
            case V_NUM: keys[made++] = num_retain(v.num); break;
            case V_NULL: keys[made++] = num_int(0); break;
            case V_TEXT:
                if (num_parse_text(v.s->s, v.s->len, &keys[made])) made++;
                else ok = quoted_fail("SORT can't compare the text %s with numbers", v.s);
                break;
            default: ok = fail("SORT can't compare a %s", v_type(v));
            }
        }
        if (ok) merge_sort(ix, tmp, m, cmp_num, keys);
        for (size_t i = 0; i < made; i++) num_release(keys[i]);
        free(keys);
    }
    if (ok) {
        Value *sorted = xmalloc((m ? m : 1) * sizeof(Value));
        for (size_t i = 0; i < m; i++) sorted[i] = v_retain(vals[ix[i]]);
        *out = with_named(t, sorted, m);
        free(sorted);
    }
    free(vals);
    free(ix);
    free(tmp);
    return ok;
}

static bool alpha_sort(Value *a, int n, Value *out) {
    if (!count("ALPHA_SORT", n, 1, 1)) return false;
    Table *t = NULL;
    if (!table_arg("ALPHA_SORT", a[0], &t)) return false;
    size_t m = t->n;
    AlphaKeys k;
    k.text = xmalloc((m ? m : 1) * sizeof(char *));
    k.lower = xmalloc((m ? m : 1) * sizeof(char *));
    size_t *ix = xmalloc((m ? m : 1) * sizeof(size_t));
    size_t *tmp = xmalloc((m ? m : 1) * sizeof(size_t));
    for (size_t i = 0; i < m; i++) {
        k.text[i] = display(t->items[i]->v);
        k.lower[i] = case_map(k.text[i], false);
        ix[i] = i;
    }
    merge_sort(ix, tmp, m, cmp_alpha, &k);
    Value *sorted = xmalloc((m ? m : 1) * sizeof(Value));
    for (size_t i = 0; i < m; i++) sorted[i] = v_retain(t->items[ix[i]]->v);
    *out = with_named(t, sorted, m);
    for (size_t i = 0; i < m; i++) {
        free(k.text[i]);
        free(k.lower[i]);
    }
    free(k.text);
    free(k.lower);
    free(sorted);
    free(ix);
    free(tmp);
    return true;
}

static void sleep_seconds(double s) {
    if (s > 1e9) s = 1e9;
#ifdef _WIN32
    Sleep((DWORD)(s * 1000.0));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

static char *read_line(void) {
    Buf b;
    buf_init(&b);
    int c;
    while ((c = getchar()) != EOF) {
        buf_putc(&b, (char)c);
        if (c == '\n') break;
    }
    while (b.len && (b.p[b.len - 1] == '\n' || b.p[b.len - 1] == '\r')) b.p[--b.len] = 0;
    return buf_take(&b);
}

bool call_builtin(Interp *it, int bi, Value *a, int n, Value *out) {
    if (bi >= GFX_FIRST) return gfx_builtin(it, bi, a, n, out);
    const char *name = WORDS[bi];
#define IS(s) (strcmp(name, s) == 0)
    if (IS("PRINT")) {
        Buf s;
        buf_init(&s);
        for (int i = 0; i < n; i++) {
            if (i) buf_putc(&s, ' ');
            write_value(&s, a[i], false, 0);
        }
        buf_putc(&s, '\n');
        out_write(s.p, s.len);
        buf_free(&s);
        if (it->ide) out_flush();
        *out = NULL_VALUE;
        return true;
    }
    if (IS("CLEAR")) {
        if (n) return fail("CLEAR wipes what's been printed, so it takes no inputs. To fill a window with a color, use CLEAR_WINDOW(color)");
        clear_output(it);
        *out = NULL_VALUE;
        return true;
    }
    if (IS("NUM")) return count(name, n, 1, 1) && to_num(a[0], out);
    if (IS("STR")) {
        if (!count(name, n, 1, 1)) return false;
        *out = text_take(display(a[0]));
        return true;
    }
    if (IS("ENG")) {
        if (!count(name, n, 1, 1)) return false;
        Number x;
        if (!number(name, a[0], &x)) return false;
        char *s = num_to_eng(x);
        num_release(x);
        if (!s) return false;
        *out = text_take(s);
        return true;
    }
    if (IS("HEX")) {
        if (!count(name, n, 1, 1)) return false;
        Number x;
        if (!number(name, a[0], &x)) return false;
        char *h = num_to_hex(x);
        if (!h) {
            char *s = num_str(x);
            fail("HEX only works on whole numbers, not %s", s);
            free(s);
            num_release(x);
            return false;
        }
        num_release(x);
        *out = text_take(h);
        return true;
    }
    if (IS("LEN")) {
        if (!count(name, n, 1, 1)) return false;
        size_t len;
        switch (a[0].t) {
        case V_TABLE: len = a[0].tb.tab->n; break;
        case V_TEXT: len = a[0].s->nchars; break;
        case V_NUM: {
            char *s = num_str(a[0].num);
            len = utf8_count(s, strlen(s));
            free(s);
            break;
        }
        case V_NULL: len = 0; break;
        default: return fail("LEN doesn't work on a %s", v_type(a[0]));
        }
        *out = v_int((int64_t)len);
        return true;
    }
    if (IS("ERROR")) {
        if (n == 0) return fail("ERROR was called");
        return fail_str(display(a[0]));
    }
    if (IS("INPUT")) {
        if (!count(name, n, 0, 1)) return false;
        char *prompt = n ? display(a[0]) : xstrdup("");
        if (it->repl) {
            // Quick-try reads code from input, so ask the editor for the answer instead.
            out_flush();
            Buf j;
            buf_init(&j);
            buf_putc(&j, MARK);
            buf_puts(&j, "{\"input\":");
            json_str(&j, prompt);
            buf_puts(&j, "}\n");
            out_write(j.p, j.len);
            buf_free(&j);
        } else {
            out_write(prompt, strlen(prompt));
        }
        free(prompt);
        out_flush();
        *out = text_take(read_line());
        return true;
    }
    if (IS("READ") || IS("READ_LINES")) {
        if (!count(name, n, 1, 1)) return false;
        char *p = display(a[0]);
        char *text, *err;
        size_t len;
        if (!read_file(p, &text, &len, &err)) {
            fail("couldn't read the file %s: %s", p, err);
            free(err);
            free(p);
            return false;
        }
        free(p);
        if (IS("READ")) {
            *out = v_text(str_new(text, len));
        } else {
            // Like Rust's str::lines(): split at \n, drop a \r before it, no last empty line.
            Table *t = table_new();
            size_t st = 0;
            for (size_t i = 0; i <= len; i++) {
                if (i == len || text[i] == '\n') {
                    if (i == len && st == len) break;
                    size_t e = i;
                    if (e > st && text[e - 1] == '\r') e--;
                    table_push(t, v_textn(text + st, e - st));
                    st = i + 1;
                }
            }
            *out = v_table(t);
        }
        free(text);
        return true;
    }
    if (IS("WRITE")) {
        if (!count(name, n, 2, 2)) return false;
        char *p = display(a[0]);
        char *data = display(a[1]);
        char *err;
        bool ok = write_file(p, data, strlen(data), &err);
        if (!ok) {
            fail("couldn't write the file %s: %s", p, err);
            free(err);
        }
        free(p);
        free(data);
        if (ok) *out = NULL_VALUE;
        return ok;
    }
    if (IS("EXISTS")) {
        if (!count(name, n, 1, 1)) return false;
        char *p = display(a[0]);
        *out = v_bool(path_exists(p));
        free(p);
        return true;
    }
    if (IS("TYPE")) {
        if (!count(name, n, 1, 1)) return false;
        *out = v_textc(v_type(a[0]));
        return true;
    }
    if (IS("RANDOM")) return random_cmd(it, a, n, out);
    if (IS("ROUND") || IS("FLOOR") || IS("CEIL")) {
        if (!count(name, n, 1, 2)) return false;
        Number x, r;
        if (!number(name, a[0], &x)) return false;
        long d;
        if (!decimals(opt(a, n, 1), &d)) {
            num_release(x);
            return false;
        }
        Rounding mode = IS("ROUND") ? ROUND_HALF : IS("FLOOR") ? ROUND_FLOOR : ROUND_CEIL;
        bool ok = num_round(x, d < 0 ? 0 : (size_t)d, mode, &r);
        num_release(x);
        if (ok) *out = v_num(r);
        return ok;
    }
    if (IS("TIME") || IS("DELTA_TIME")) {
        if (!count(name, n, 0, 1)) return false;
        double t;
        double now = now_seconds();
        if (IS("TIME")) {
            t = now - it->start;
        } else {
            t = now - it->last_delta;
            it->last_delta = now;
        }
        long d;
        if (!decimals(opt(a, n, 0), &d)) return false;
        Number r;
        bool ok = d >= 0 ? num_from_f64_rounded(t, (size_t)d, &r) : num_from_f64(t, &r);
        if (ok) *out = v_num(r);
        return ok;
    }
    if (IS("WAIT")) {
        if (!count(name, n, 1, 1)) return false;
        bool ok;
        double s = number_f64(name, a[0], &ok);
        if (!ok) return false;
        out_flush();
        if (it->ntasks) {
            // With tasks running, the others get to run meanwhile.
            if (!task_sleep(it, s)) return false;
        } else if (s > 0.0) {
            sleep_seconds(s);
        }
        *out = NULL_VALUE;
        return true;
    }
    if (IS("AWAIT")) {
        if (!count(name, n, 1, 1)) return false;
        return task_await(it, a[0], out);
    }
    if (IS("DONE")) {
        if (!count(name, n, 1, 1)) return false;
        bool d;
        if (!task_done(it, a[0], &d)) return false;
        *out = v_bool(d);
        return true;
    }
    if (IS("CANCEL")) {
        if (!count(name, n, 1, 1)) return false;
        if (!task_cancel(it, a[0])) return false;
        *out = NULL_VALUE;
        return true;
    }
    if (IS("UPPER") || IS("LOWER")) {
        if (!count(name, n, 1, 1)) return false;
        char *s = display(a[0]);
        char *r = case_map(s, IS("UPPER"));
        free(s);
        *out = text_take(r);
        return true;
    }
    if (IS("SPLIT")) {
        if (!count(name, n, 2, 2)) return false;
        if (a[0].t != V_TEXT) return fail("SPLIT needs text, not a %s", v_type(a[0]));
        Str *s = a[0].s;
        char *sep = display(a[1]);
        size_t sl = strlen(sep);
        Table *t = table_new();
        if (sl == 0) {
            for (size_t i = 0; i < s->len;) {
                size_t st = i;
                utf8_next(s->s, s->len, &i);
                table_push(t, v_textn(s->s + st, i - st));
            }
        } else {
            size_t st = 0, i = 0;
            while (i + sl <= s->len) {
                if (memcmp(s->s + i, sep, sl) == 0) {
                    table_push(t, v_textn(s->s + st, i - st));
                    i += sl;
                    st = i;
                } else {
                    i++;
                }
            }
            table_push(t, v_textn(s->s + st, s->len - st));
        }
        free(sep);
        *out = v_table(t);
        return true;
    }
    if (IS("JOIN")) {
        if (!count(name, n, 1, 2)) return false;
        Table *t = NULL;
        if (!table_arg(name, a[0], &t)) return false;
        Value *sepv = opt(a, n, 1);
        char *sep = sepv ? display(*sepv) : xstrdup("");
        Buf b;
        buf_init(&b);
        for (size_t i = 0; i < t->n; i++) {
            if (i) buf_puts(&b, sep);
            write_value(&b, t->items[i]->v, false, 0);
        }
        free(sep);
        *out = v_text(str_from_buf(&b));
        return true;
    }
    if (IS("FIND")) {
        if (!count(name, n, 2, 3)) return false;
        if (a[0].t == V_TEXT) {
            Str *s = a[0].s;
            char *part = display(a[1]);
            size_t pl = strlen(part);
            long at = -1;
            for (size_t i = 0; i + pl <= s->len; i++) {
                if (memcmp(s->s + i, part, pl) == 0) {
                    at = (long)i;
                    break;
                }
            }
            free(part);
            *out = at < 0 ? NULL_VALUE : v_int((int64_t)utf8_count(s->s, (size_t)at) + 1);
            return true;
        }
        if (a[0].t == V_TABLE) {
            size_t depth;
            if (!depth_arg(opt(a, n, 2), &depth)) return false;
            Values p = {0};
            if (!find_path(a[0].tb.tab, a[1], depth, &p)) {
                *out = NULL_VALUE;
            } else if (p.n == 1) {
                *out = p.v[0];
                free(p.v);
                return true;
            } else {
                Table *t = table_new();
                for (size_t i = 0; i < p.n; i++) table_push(t, p.v[i]);
                free(p.v);
                *out = v_table(t);
                return true;
            }
            values_free(&p);
            return true;
        }
        return fail("FIND searches text or a table, not a %s", v_type(a[0]));
    }
    if (IS("ABS")) {
        if (!count(name, n, 1, 1)) return false;
        Number x;
        if (!number(name, a[0], &x)) return false;
        *out = v_num(num_abs(x));
        num_release(x);
        return true;
    }
    if (IS("MIN") || IS("MAX")) {
        if (!count(name, n, 1, ANY)) return false;
        return extreme(name, a, (size_t)n, IS("MAX"), out);
    }
    if (IS("TABLE_MIN") || IS("TABLE_MAX")) {
        if (!count(name, n, 1, 2)) return false;
        Table *t = NULL;
        if (!table_arg(name, a[0], &t)) return false;
        size_t depth;
        if (!depth_arg(opt(a, n, 1), &depth)) return false;
        Values vals = {0};
        collect_values(t, depth, &vals);
        bool ok = extreme(name + 6, vals.v, vals.n, IS("TABLE_MAX"), out);
        values_free(&vals);
        return ok;
    }
    if (IS("ATAN") || IS("RAD_ATAN")) {
        if (!count(name, n, 1, 2)) return false;
        bool ok;
        double y = number_f64(name, a[0], &ok);
        if (!ok) return false;
        double r;
        if (n > 1) {
            double x = number_f64(name, a[1], &ok);
            if (!ok) return false;
            r = atan2(y, x);
        } else {
            r = atan(y);
        }
        Number res;
        ok = IS("ATAN") ? snap(r * (180.0 / PI_F), &res) : num_from_f64(r, &res);
        if (ok) *out = v_num(res);
        return ok;
    }
    if (IS("SIN") || IS("COS") || IS("TAN") || IS("CSC") || IS("SEC") || IS("COT") || IS("ASIN") || IS("ACOS") || IS("RAD_SIN") ||
        IS("RAD_COS") || IS("RAD_TAN") || IS("RAD_CSC") || IS("RAD_SEC") || IS("RAD_COT") || IS("RAD_ASIN") || IS("RAD_ACOS")) {
        if (!count(name, n, 1, 1)) return false;
        Number x, r;
        if (!number(name, a[0], &x)) return false;
        bool ok = trig(name, x, &r);
        num_release(x);
        if (ok) *out = v_num(r);
        return ok;
    }
    if (IS("PI")) {
        if (!count(name, n, 0, 1)) return false;
        long d;
        if (!decimals(opt(a, n, 0), &d)) return false;
        Number r;
        bool ok = d >= 0 ? num_pi((size_t)d, &r) : num_from_f64(PI_F, &r);
        if (ok) *out = v_num(r);
        return ok;
    }
    if (IS("SORT")) return sort_cmd(a, n, out);
    if (IS("ALPHA_SORT")) return alpha_sort(a, n, out);
    if (IS("KEYS")) {
        if (!count(name, n, 1, 1)) return false;
        Table *t = NULL;
        if (!table_arg(name, a[0], &t)) return false;
        Table *k = table_new();
        for (size_t i = 0; i < t->n; i++) table_push(k, v_int((int64_t)i + 1));
        for (size_t i = 0; i < t->named.n; i++) table_push(k, v_text(str_ref(t->named.e[i].key)));
        *out = v_table(k);
        return true;
    }
#undef IS
    return fail("%s isn't available", name);
}
