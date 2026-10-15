// Numbers.
//
// Whole numbers and fractions are exact, and grow to as many digits as they need:
// 0.1 + 0.2 is exactly 0.3, 1 / 3 * 3 is exactly 1, and 2 ^ 200 keeps every digit.
// Only answers that can't be written exactly (most roots and trig results) are
// approximate; those use 64-bit floating point.

#include <math.h>

#include "jlb.h"

#define BASE 1000000000u
/// Exact numbers can use up to about this many digits.
#define MAX_DIGITS 1000000
/// A fraction that can't be written as a plain decimal (like 1/3) stays an exact
/// fraction until its bottom number passes this many digits.
#define FRACTION_DIGITS 50
/// Decimals that go on forever show this many significant decimals.
#define SHOW_DIGITS 16

static bool too_big(void) {
    return fail("that number needs too many digits to store (the limit is a million)");
}

// ---------- unlimited whole numbers ----------

static Nat nat_zero(void) {
    Nat n = {NULL, 0};
    return n;
}

static Nat nat_alloc(size_t n) {
    Nat r;
    r.d = n ? xcalloc(n, sizeof(uint32_t)) : NULL;
    r.n = n;
    return r;
}

static void nat_free(Nat *n) {
    free(n->d);
    n->d = NULL;
    n->n = 0;
}

static Nat nat_copy(const Nat *a) {
    Nat r = nat_alloc(a->n);
    if (a->n) memcpy(r.d, a->d, a->n * sizeof(uint32_t));
    return r;
}

static void nat_norm(Nat *a) {
    while (a->n && a->d[a->n - 1] == 0) a->n--;
    if (a->n == 0) {
        free(a->d);
        a->d = NULL;
    }
}

static Nat nat_from_u64(uint64_t x) {
    Nat r = nat_alloc(3);
    size_t i = 0;
    while (x > 0) {
        r.d[i++] = (uint32_t)(x % BASE);
        x /= BASE;
    }
    r.n = i;
    nat_norm(&r);
    return r;
}

static Nat nat_one(void) {
    return nat_from_u64(1);
}

static bool nat_is_zero(const Nat *a) {
    return a->n == 0;
}

static bool nat_is_one(const Nat *a) {
    return a->n == 1 && a->d[0] == 1;
}

static bool nat_is_odd(const Nat *a) {
    return a->n && (a->d[0] & 1);
}

static int nat_cmp(const Nat *a, const Nat *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (size_t i = a->n; i-- > 0;) {
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

static bool nat_eq(const Nat *a, const Nat *b) {
    return nat_cmp(a, b) == 0;
}

/// Fits in 64 bits?
static bool nat_to_u64(const Nat *a, uint64_t *out) {
    if (a->n > 3) return false;
    unsigned __int128 r = 0;
    for (size_t i = a->n; i-- > 0;) r = r * BASE + a->d[i];
    if (r > UINT64_MAX) return false;
    *out = (uint64_t)r;
    return true;
}

static int ilog10_u32(uint32_t x) {
    int k = 0;
    while (x >= 10) {
        x /= 10;
        k++;
    }
    return k;
}

/// How many decimal digits it has (0 has none).
static size_t nat_digits(const Nat *a) {
    if (!a->n) return 0;
    return (a->n - 1) * 9 + (size_t)ilog10_u32(a->d[a->n - 1]) + 1;
}

/// Roughly log10 of the number (it must not be 0).
static double nat_approx_log10(const Nat *a) {
    size_t n = a->n;
    if (n == 1) return log10((double)a->d[0]);
    double top = (double)a->d[n - 1] * (double)BASE + (double)a->d[n - 2];
    return log10(top) + (double)((n - 2) * 9);
}

static Nat nat_add(const Nat *x, const Nat *y) {
    const Nat *a = x->n >= y->n ? x : y;
    const Nat *b = x->n >= y->n ? y : x;
    Nat r = nat_alloc(a->n + 1);
    uint64_t c = 0;
    for (size_t i = 0; i < a->n; i++) {
        uint64_t s = (uint64_t)a->d[i] + (i < b->n ? b->d[i] : 0) + c;
        r.d[i] = (uint32_t)(s % BASE);
        c = s / BASE;
    }
    r.d[a->n] = (uint32_t)c;
    nat_norm(&r);
    return r;
}

/// a - b, where a >= b.
static Nat nat_sub(const Nat *a, const Nat *b) {
    Nat r = nat_alloc(a->n);
    int64_t borrow = 0;
    for (size_t i = 0; i < a->n; i++) {
        int64_t s = (int64_t)a->d[i] - (i < b->n ? (int64_t)b->d[i] : 0) - borrow;
        if (s < 0) {
            s += BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r.d[i] = (uint32_t)s;
    }
    nat_norm(&r);
    return r;
}

static Nat nat_mul_small(const Nat *a, uint32_t m) {
    if (m == 0 || nat_is_zero(a)) return nat_zero();
    Nat r = nat_alloc(a->n + 2);
    uint64_t c = 0;
    size_t i;
    for (i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + c;
        r.d[i] = (uint32_t)(t % BASE);
        c = t / BASE;
    }
    while (c > 0) {
        r.d[i++] = (uint32_t)(c % BASE);
        c /= BASE;
    }
    nat_norm(&r);
    return r;
}

static Nat nat_mul(const Nat *a, const Nat *b) {
    if (nat_is_zero(a) || nat_is_zero(b)) return nat_zero();
    if (b->n == 1) return nat_mul_small(a, b->d[0]);
    if (a->n == 1) return nat_mul_small(b, a->d[0]);
    size_t n = a->n + b->n;
    uint64_t *r = xcalloc(n, sizeof(uint64_t));
    for (size_t i = 0; i < a->n; i++) {
        uint64_t x = a->d[i];
        if (x == 0) continue;
        uint64_t c = 0;
        for (size_t j = 0; j < b->n; j++) {
            uint64_t t = r[i + j] + x * b->d[j] + c;
            r[i + j] = t % BASE;
            c = t / BASE;
        }
        size_t k = i + b->n;
        while (c > 0) {
            uint64_t t = r[k] + c;
            r[k] = t % BASE;
            c = t / BASE;
            k++;
        }
    }
    Nat out = nat_alloc(n);
    for (size_t i = 0; i < n; i++) out.d[i] = (uint32_t)r[i];
    free(r);
    nat_norm(&out);
    return out;
}

static Nat nat_divmod_small(const Nat *a, uint32_t d, uint32_t *rem) {
    Nat q = nat_alloc(a->n);
    uint64_t r = 0;
    for (size_t i = a->n; i-- > 0;) {
        uint64_t cur = r * BASE + a->d[i];
        q.d[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
    nat_norm(&q);
    if (rem) *rem = (uint32_t)r;
    return q;
}

/// Quotient and remainder (Knuth's long division). `o` must not be 0.
static void nat_divmod(const Nat *self, const Nat *o, Nat *qo, Nat *ro) {
    if (nat_cmp(self, o) < 0) {
        if (qo) *qo = nat_zero();
        if (ro) *ro = nat_copy(self);
        return;
    }
    if (o->n == 1) {
        uint32_t r;
        Nat q = nat_divmod_small(self, o->d[0], &r);
        if (qo) *qo = q;
        else nat_free(&q);
        if (ro) *ro = nat_from_u64(r);
        return;
    }
    size_t n = o->n;
    uint32_t d = (uint32_t)(BASE / ((uint64_t)o->d[n - 1] + 1));
    Nat vv = nat_mul_small(o, d);
    Nat uu = nat_mul_small(self, d);
    // u needs self->n + 1 parts.
    size_t ulen = uu.n == self->n ? uu.n + 1 : uu.n;
    uint32_t *u = xcalloc(ulen, sizeof(uint32_t));
    memcpy(u, uu.d, uu.n * sizeof(uint32_t));
    nat_free(&uu);
    uint32_t *v = xcalloc(n, sizeof(uint32_t));
    memcpy(v, vv.d, vv.n * sizeof(uint32_t));
    nat_free(&vv);
    size_t m = ulen - n - 1;
    Nat q = nat_alloc(m + 1);
    uint64_t vt = v[n - 1], vs = v[n - 2];
    for (size_t jj = m + 1; jj-- > 0;) {
        size_t j = jj;
        uint64_t num = (uint64_t)u[j + n] * BASE + u[j + n - 1];
        uint64_t qhat = num / vt;
        uint64_t rhat = num % vt;
        while (qhat >= BASE || qhat * vs > rhat * BASE + u[j + n - 2]) {
            qhat -= 1;
            rhat += vt;
            if (rhat >= BASE) break;
        }
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t p = qhat * v[i] + carry;
            carry = p / BASE;
            int64_t t = (int64_t)u[i + j] - (int64_t)(p % BASE) - borrow;
            if (t < 0) {
                t += BASE;
                borrow = 1;
            } else {
                borrow = 0;
            }
            u[i + j] = (uint32_t)t;
        }
        int64_t t = (int64_t)u[j + n] - (int64_t)carry - borrow;
        if (t < 0) {
            // qhat was one too big: add the divisor back.
            u[j + n] = (uint32_t)(t + BASE);
            qhat -= 1;
            uint64_t c = 0;
            for (size_t i = 0; i < n; i++) {
                uint64_t s = (uint64_t)u[i + j] + v[i] + c;
                u[i + j] = (uint32_t)(s % BASE);
                c = s / BASE;
            }
            u[j + n] = (uint32_t)(((uint64_t)u[j + n] + c) % BASE);
        } else {
            u[j + n] = (uint32_t)t;
        }
        q.d[j] = (uint32_t)qhat;
    }
    nat_norm(&q);
    if (ro) {
        Nat rem = nat_alloc(n);
        memcpy(rem.d, u, n * sizeof(uint32_t));
        nat_norm(&rem);
        *ro = nat_divmod_small(&rem, d, NULL);
        nat_free(&rem);
    }
    free(u);
    free(v);
    if (qo) *qo = q;
    else nat_free(&q);
}

static Nat nat_div(const Nat *a, const Nat *b) {
    Nat q;
    nat_divmod(a, b, &q, NULL);
    return q;
}

static Nat nat_mod(const Nat *a, const Nat *b) {
    Nat r;
    nat_divmod(a, b, NULL, &r);
    return r;
}

static Nat nat_pow(const Nat *self, uint64_t e) {
    Nat base = nat_copy(self);
    Nat r = nat_one();
    while (e > 0) {
        if (e & 1) {
            Nat t = nat_mul(&r, &base);
            nat_free(&r);
            r = t;
        }
        e >>= 1;
        if (e > 0) {
            Nat t = nat_mul(&base, &base);
            nat_free(&base);
            base = t;
        }
    }
    nat_free(&base);
    return r;
}

static const uint32_t POW10[10] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

static Nat nat_pow10(size_t k) {
    Nat r = nat_alloc(k / 9 + 1);
    r.d[k / 9] = POW10[k % 9];
    return r;
}

static Nat nat_mul_pow10(const Nat *a, size_t k) {
    if (nat_is_zero(a)) return nat_zero();
    Nat t = nat_mul_small(a, POW10[k % 9]);
    Nat r = nat_alloc(k / 9 + t.n);
    memcpy(r.d + k / 9, t.d, t.n * sizeof(uint32_t));
    nat_free(&t);
    return r;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static Nat nat_gcd(const Nat *x, const Nat *y) {
    Nat a = nat_copy(x), b = nat_copy(y);
    while (!nat_is_zero(&b)) {
        uint64_t p, q;
        if (nat_to_u64(&a, &p) && nat_to_u64(&b, &q)) {
            nat_free(&a);
            nat_free(&b);
            return nat_from_u64(gcd_u64(p, q));
        }
        Nat r = nat_mod(&a, &b);
        nat_free(&a);
        a = b;
        b = r;
    }
    nat_free(&b);
    return a;
}

/// Divides out every factor `p` (with `chunk` = p^chunk_pow to go faster).
static Nat nat_strip(const Nat *self, uint32_t chunk, size_t chunk_pow, uint32_t p, size_t *count) {
    Nat n = nat_copy(self);
    size_t c = 0;
    uint32_t ds[2] = {chunk, p};
    size_t ks[2] = {chunk_pow, 1};
    for (int s = 0; s < 2; s++) {
        for (;;) {
            uint32_t r;
            Nat q = nat_divmod_small(&n, ds[s], &r);
            if (r != 0) {
                nat_free(&q);
                break;
            }
            nat_free(&n);
            n = q;
            c += ks[s];
            if (nat_is_zero(&n)) break;
        }
    }
    *count = c;
    return n;
}

/// From decimal digits (all 0-9).
static Nat nat_parse_dec(const char *s, size_t len) {
    Nat v = nat_alloc(len / 9 + 1);
    size_t k = 0;
    size_t end = len;
    while (end > 0) {
        size_t st = end >= 9 ? end - 9 : 0;
        uint32_t x = 0;
        for (size_t i = st; i < end; i++) x = x * 10 + (uint32_t)(s[i] - '0');
        v.d[k++] = x;
        end = st;
    }
    v.n = k;
    nat_norm(&v);
    return v;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/// From hex digits.
static Nat nat_parse_hex(const char *s) {
    Nat n = nat_zero();
    for (; *s; s++) {
        Nat t = nat_mul_small(&n, 16);
        Nat d = nat_from_u64((uint64_t)hexval(*s));
        nat_free(&n);
        n = nat_add(&t, &d);
        nat_free(&t);
        nat_free(&d);
    }
    return n;
}

static void nat_write_hex(Buf *b, const Nat *self) {
    if (nat_is_zero(self)) {
        buf_putc(b, '0');
        return;
    }
    uint32_t *parts = NULL;
    size_t np = 0, cap = 0;
    Nat n = nat_copy(self);
    while (!nat_is_zero(&n)) {
        uint32_t r;
        Nat q = nat_divmod_small(&n, 1u << 28, &r);
        if (np == cap) {
            cap = cap ? cap * 2 : 8;
            parts = xrealloc(parts, cap * sizeof(uint32_t));
        }
        parts[np++] = r;
        nat_free(&n);
        n = q;
    }
    buf_printf(b, "%X", parts[np - 1]);
    for (size_t i = np - 1; i-- > 0;) buf_printf(b, "%07X", parts[i]);
    free(parts);
}

static void nat_write(Buf *b, const Nat *a) {
    if (!a->n) {
        buf_putc(b, '0');
        return;
    }
    buf_printf(b, "%u", a->d[a->n - 1]);
    for (size_t i = a->n - 1; i-- > 0;) buf_printf(b, "%09u", a->d[i]);
}

static Nat nat_add_one(const Nat *a) {
    Nat one = nat_one();
    Nat r = nat_add(a, &one);
    nat_free(&one);
    return r;
}

/// Floor of the q-th root.
static Nat nth_root_floor(const Nat *n, uint64_t q) {
    // Start a little above the real root, then Newton's method walks down to it.
    double l = nat_approx_log10(n) / (double)q;
    Nat x;
    if (l < 15.0) {
        x = nat_from_u64((uint64_t)ceil(pow(10.0, l) * (1.0 + 1e-9)) + 1);
    } else {
        double e = floor(l);
        double m = pow(10.0, l - e) * 1e15 * (1.0 + 1e-9);
        Nat t = nat_from_u64((uint64_t)ceil(m) + 1);
        x = nat_mul_pow10(&t, (size_t)e - 15);
        nat_free(&t);
    }
    uint32_t q32 = (uint32_t)q;
    for (;;) {
        Nat p = nat_pow(&x, q - 1);
        Nat t = nat_div(n, &p);
        nat_free(&p);
        Nat a = nat_mul_small(&x, q32 - 1);
        Nat s = nat_add(&a, &t);
        Nat y = nat_divmod_small(&s, q32, NULL);
        nat_free(&t);
        nat_free(&a);
        nat_free(&s);
        if (nat_cmp(&y, &x) >= 0) {
            nat_free(&y);
            break;
        }
        nat_free(&x);
        x = y;
    }
    for (;;) {
        if (nat_is_zero(&x)) break;
        Nat p = nat_pow(&x, q);
        int c = nat_cmp(&p, n);
        nat_free(&p);
        if (c <= 0) break;
        Nat one = nat_one();
        Nat t = nat_sub(&x, &one);
        nat_free(&one);
        nat_free(&x);
        x = t;
    }
    for (;;) {
        Nat y = nat_add_one(&x);
        Nat p = nat_pow(&y, q);
        int c = nat_cmp(&p, n);
        nat_free(&p);
        if (c <= 0) {
            nat_free(&x);
            x = y;
        } else {
            nat_free(&y);
            break;
        }
    }
    return x;
}

/// The q-th root, if it's a whole number.
static bool exact_root(const Nat *n, uint64_t q, Nat *out) {
    if (nat_is_zero(n) || nat_is_one(n) || q == 1) {
        *out = nat_copy(n);
        return true;
    }
    // Any root of more than 2 bits' worth is below 2, so it can't be whole.
    if (q > UINT32_MAX || (double)q > nat_approx_log10(n) * 3.32192809488736234787 + 1.0) return false;
    Nat r = nth_root_floor(n, q);
    Nat p = nat_pow(&r, q);
    bool ok = nat_eq(&p, n);
    nat_free(&p);
    if (ok) {
        *out = r;
        return true;
    }
    nat_free(&r);
    return false;
}

/// If n/den can be written as a plain decimal, how many decimals it needs (-1 if not).
static long plain_decimals(const Nat *den) {
    size_t twos, fives;
    Nat a = nat_strip(den, 1u << 29, 29, 2, &twos);
    Nat b = nat_strip(&a, 244140625u, 12, 5, &fives);
    bool one = nat_is_one(&b);
    nat_free(&a);
    nat_free(&b);
    if (!one) return -1;
    return (long)(twos > fives ? twos : fives);
}

/// Adds signed magnitudes (takes nothing; gives a new Nat).
static Nat signed_add(bool an, const Nat *a, bool bn, const Nat *b, bool *neg) {
    if (an == bn) {
        *neg = an;
        return nat_add(a, b);
    }
    if (nat_cmp(a, b) < 0) {
        *neg = bn;
        return nat_sub(b, a);
    }
    *neg = an;
    return nat_sub(a, b);
}

// ---------- numbers ----------

Number num_retain(Number n) {
    if (n.k == N_BIG) n.r->rc++;
    return n;
}

void num_release(Number n) {
    if (n.k == N_BIG && --n.r->rc == 0) {
        nat_free(&n.r->num);
        nat_free(&n.r->den);
        free(n.r);
    }
}

/// Takes num and den.
static Number big(bool neg, Nat num, Nat den, bool plain) {
    Ratio *r = xmalloc(sizeof(Ratio));
    r->rc = 1;
    r->neg = neg;
    r->num = num;
    r->den = den;
    r->plain = plain;
    Number n;
    n.k = N_BIG;
    n.r = r;
    return n;
}

static bool round_ratio(bool neg, const Nat *num, const Nat *den, size_t p, Rounding mode, Number *out);

/// Position of the first non-zero digit after the decimal point (1 = tenths).
static size_t first_decimal(const Nat *num, const Nat *den) {
    Nat frac = nat_mod(num, den);
    if (nat_is_zero(&frac)) {
        nat_free(&frac);
        return 1;
    }
    size_t dd = nat_digits(den), fd = nat_digits(&frac);
    size_t p = dd > fd ? dd - fd : 0;
    Nat t = nat_mul_pow10(&frac, p);
    bool less = nat_cmp(&t, den) < 0;
    nat_free(&t);
    nat_free(&frac);
    if (less) return p + 1;
    return p > 1 ? p : 1;
}

/// Builds an exact number from a fraction that's already in lowest terms (takes num and den).
/// `plain`: 1 known plain, -1 work it out.
static bool finish(bool neg, Nat num, Nat den, int plain, Number *out) {
    if (nat_is_zero(&num)) {
        nat_free(&num);
        nat_free(&den);
        *out = num_int(0);
        return true;
    }
    if (nat_digits(&num) > MAX_DIGITS || nat_digits(&den) > MAX_DIGITS) {
        nat_free(&num);
        nat_free(&den);
        return too_big();
    }
    if (nat_is_one(&den)) {
        uint64_t u;
        if (nat_to_u64(&num, &u)) {
            if (u <= (uint64_t)INT64_MAX) {
                nat_free(&num);
                nat_free(&den);
                *out = num_int(neg ? -(int64_t)u : (int64_t)u);
                return true;
            }
            if (neg && u == (uint64_t)1 << 63) {
                nat_free(&num);
                nat_free(&den);
                *out = num_int(INT64_MIN);
                return true;
            }
        }
        *out = big(neg, num, den, true);
        return true;
    }
    bool pl = plain == 1 ? true : plain_decimals(&den) >= 0;
    if (!pl && nat_digits(&den) > FRACTION_DIGITS) {
        size_t p = first_decimal(&num, &den) + FRACTION_DIGITS - 1;
        bool ok = round_ratio(neg, &num, &den, p, ROUND_HALF, out);
        nat_free(&num);
        nat_free(&den);
        return ok;
    }
    *out = big(neg, num, den, pl);
    return true;
}

/// a / g, skipping the work when g is 1.
static Nat cut(const Nat *a, const Nat *g) {
    return nat_is_one(g) ? nat_copy(a) : nat_div(a, g);
}

/// Builds an exact number from any fraction (takes num and den; den not 0).
static bool make(bool neg, Nat num, Nat den, int plain, Number *out) {
    if (nat_is_zero(&num)) {
        nat_free(&num);
        nat_free(&den);
        *out = num_int(0);
        return true;
    }
    Nat g = nat_gcd(&num, &den);
    Nat a = cut(&num, &g), b = cut(&den, &g);
    nat_free(&num);
    nat_free(&den);
    nat_free(&g);
    return finish(neg, a, b, plain, out);
}

/// Rounds to `p` decimal places.
static bool round_ratio(bool neg, const Nat *num, const Nat *den, size_t p, Rounding mode, Number *out) {
    if (p > MAX_DIGITS) return too_big();
    Nat t = nat_mul_pow10(num, p);
    Nat q, rem;
    nat_divmod(&t, den, &q, &rem);
    nat_free(&t);
    bool up = false;
    if (!nat_is_zero(&rem)) {
        switch (mode) {
        case ROUND_HALF: {
            Nat r2 = nat_mul_small(&rem, 2);
            up = nat_cmp(&r2, den) >= 0;
            nat_free(&r2);
            break;
        }
        case ROUND_FLOOR: up = neg; break;
        case ROUND_CEIL: up = !neg; break;
        case ROUND_TRUNC: up = false; break;
        }
    }
    nat_free(&rem);
    if (up) {
        Nat t2 = nat_add_one(&q);
        nat_free(&q);
        q = t2;
    }
    return make(neg, q, nat_pow10(p), 1, out);
}

/// The exact value of a 64-bit float, as (negative, top, bottom).
static void exact_of_f64(double x, bool *neg, Nat *num, Nat *den) {
    uint64_t bits;
    memcpy(&bits, &x, 8);
    *neg = (bits >> 63) == 1;
    int64_t ex = (int64_t)((bits >> 52) & 0x7ff);
    uint64_t mant = bits & (((uint64_t)1 << 52) - 1);
    int64_t e;
    if (ex == 0) {
        e = -1074;
    } else {
        mant |= (uint64_t)1 << 52;
        e = ex - 1075;
    }
    if (mant == 0) {
        *neg = false;
        *num = nat_zero();
        *den = nat_one();
        return;
    }
    Nat two = nat_from_u64(2);
    if (e >= 0) {
        Nat m = nat_from_u64(mant);
        Nat p = nat_pow(&two, (uint64_t)e);
        *num = nat_mul(&m, &p);
        *den = nat_one();
        nat_free(&m);
        nat_free(&p);
        nat_free(&two);
        return;
    }
    int64_t tz = __builtin_ctzll(mant);
    if (tz > -e) tz = -e;
    mant >>= tz;
    e += tz;
    *num = nat_from_u64(mant);
    *den = nat_pow(&two, (uint64_t)(-e));
    nat_free(&two);
}

static bool float_result(double x, Number *out) {
    if (isnan(x)) return fail("that has no real answer");
    return num_from_f64(x, out);
}

static const char *PI_DIGITS =
    "3.14159265358979323846264338327950288419716939937510582097494459230781640628620899862803482534211706798214808651";

bool num_from_f64(double x, Number *out) {
    if (!isfinite(x)) return fail("that result is too big to store");
    if (x == trunc(x)) {
        if (fabs(x) < 9.2e18) {
            *out = num_int((int64_t)x);
            return true;
        }
        bool n;
        Nat a, b;
        exact_of_f64(x, &n, &a, &b);
        return finish(n, a, b, 1, out);
    }
    out->k = N_FLOAT;
    out->f = x;
    return true;
}

bool num_from_f64_rounded(double x, size_t d, Number *out) {
    Number t;
    if (!num_from_f64(x, &t)) return false;
    bool ok = num_round(t, d, ROUND_HALF, out);
    num_release(t);
    return ok;
}

/// Digits written in a program or in text: 25, 2.5, 1.5e-3, 0xFF (no sign).
bool num_parse_literal(const char *s, Number *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return finish(false, nat_parse_hex(s + 2), nat_one(), 1, out);
    const char *e = strpbrk(s, "eE");
    size_t mlen = e ? (size_t)(e - s) : strlen(s);
    int64_t exp = 0;
    if (e) {
        // Like Rust's parse::<i64>(): a sign and digits, or it's "too big".
        const char *p = e + 1;
        bool neg = false;
        if (*p == '+' || *p == '-') neg = *p++ == '-';
        bool ok = *p != 0;
        int64_t v = 0;
        for (; *p; p++) {
            if (*p < '0' || *p > '9' || v > (INT64_MAX - 9) / 10) {
                ok = false;
                break;
            }
            v = v * 10 + (*p - '0');
        }
        exp = ok ? (neg ? -v : v) : INT64_MAX;
    }
    const char *dot = memchr(s, '.', mlen);
    size_t iplen = dot ? (size_t)(dot - s) : mlen;
    size_t fplen = dot ? mlen - iplen - 1 : 0;
    char *digits = xmalloc(iplen + fplen + 1);
    memcpy(digits, s, iplen);
    if (dot) memcpy(digits + iplen, dot + 1, fplen);
    digits[iplen + fplen] = 0;
    const char *d = digits;
    while (*d == '0') d++;
    Nat n = nat_parse_dec(d, strlen(d));
    free(digits);
    if (nat_is_zero(&n)) {
        *out = num_int(0);
        return true;
    }
    int64_t scale;
    if (__builtin_sub_overflow(exp, (int64_t)fplen, &scale)) scale = exp < 0 ? INT64_MIN : INT64_MAX;
    uint64_t mag = scale < 0 ? (uint64_t)(-(scale + 1)) + 1 : (uint64_t)scale;
    if (mag > (uint64_t)MAX_DIGITS * 2) {
        nat_free(&n);
        return too_big();
    }
    if (scale >= 0) {
        Nat m = nat_mul_pow10(&n, (size_t)scale);
        nat_free(&n);
        return finish(false, m, nat_one(), 1, out);
    }
    return make(false, n, nat_pow10((size_t)(-scale)), 1, out);
}

static bool all_digits(const char *s, size_t n, bool hex) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        if (!ok) return false;
    }
    return true;
}

/// Text that counts as a number: 25, -25, 2.5, 1e3, 1.5e-3, 0xFF.
/// Not numbers: "025", " 25 ", "1,000", ".5", "".
bool num_parse_text(const char *s, size_t len, Number *out) {
    bool neg = false;
    if (len && s[0] == '-') {
        neg = true;
        s++;
        len--;
    }
    if (len == 0) return false;
    char *body = xstrndup(s, len);
    bool ok;
    Number v;
    if (len >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
        if (len == 2 || !all_digits(body + 2, len - 2, true)) {
            free(body);
            return false;
        }
        ok = num_parse_literal(body, &v);
    } else {
        size_t i;
        const unsigned char *b = (const unsigned char *)body;
        if (b[0] == '0') {
            i = 1;
        } else if (b[0] >= '0' && b[0] <= '9') {
            i = 0;
            while (i < len && b[i] >= '0' && b[i] <= '9') i++;
        } else {
            free(body);
            return false;
        }
        if (i < len && b[i] == '.') {
            i++;
            size_t st = i;
            while (i < len && b[i] >= '0' && b[i] <= '9') i++;
            if (i == st) {
                free(body);
                return false;
            }
        }
        if (i < len && (b[i] == 'e' || b[i] == 'E')) {
            i++;
            if (i < len && (b[i] == '+' || b[i] == '-')) i++;
            size_t st = i;
            while (i < len && b[i] >= '0' && b[i] <= '9') i++;
            if (i == st) {
                free(body);
                return false;
            }
        }
        if (i != len) {
            free(body);
            return false;
        }
        ok = num_parse_literal(body, &v);
    }
    free(body);
    if (!ok) {
        // Too big to be a number: it's just text (the error is dropped).
        error_free(take_error());
        return false;
    }
    if (neg) {
        Number t = num_neg(v);
        num_release(v);
        v = t;
    }
    *out = v;
    return true;
}

static void exact_parts(Number n, bool *neg, Nat *a, Nat *b) {
    switch (n.k) {
    case N_INT:
        *neg = n.i < 0;
        *a = nat_from_u64(n.i < 0 ? (uint64_t)0 - (uint64_t)n.i : (uint64_t)n.i);
        *b = nat_one();
        break;
    case N_BIG:
        *neg = n.r->neg;
        *a = nat_copy(&n.r->num);
        *b = nat_copy(&n.r->den);
        break;
    case N_FLOAT:
        exact_of_f64(n.f, neg, a, b);
        break;
    }
}

static bool num_plain(Number n) {
    return n.k == N_BIG ? n.r->plain : true;
}

bool num_is_float(Number n) {
    return n.k == N_FLOAT;
}

bool num_is_integer(Number n) {
    switch (n.k) {
    case N_INT: return true;
    case N_BIG: return nat_is_one(&n.r->den);
    default: return false;
    }
}

bool num_is_zero(Number n) {
    return n.k == N_INT && n.i == 0;
}

bool num_is_neg(Number n) {
    switch (n.k) {
    case N_INT: return n.i < 0;
    case N_BIG: return n.r->neg;
    default: return n.f < 0.0;
    }
}

bool num_is_int(Number n, int64_t v) {
    return n.k == N_INT && n.i == v;
}

int64_t num_to_i64_sat(Number n) {
    switch (n.k) {
    case N_INT: return n.i;
    case N_BIG: return n.r->neg ? INT64_MIN : INT64_MAX;
    default:
        // Like Rust's `as`: saturating, NaN is 0.
        if (isnan(n.f)) return 0;
        if (n.f >= 9223372036854775807.0) return INT64_MAX;
        if (n.f <= -9223372036854775808.0) return INT64_MIN;
        return (int64_t)n.f;
    }
}

double num_to_f64(Number n) {
    switch (n.k) {
    case N_INT: return (double)n.i;
    case N_FLOAT: return n.f;
    default: {
        Ratio *r = n.r;
        uint64_t a, b;
        double v;
        if (nat_to_u64(&r->num, &a) && nat_to_u64(&r->den, &b) && a < (uint64_t)1 << 53 && b < (uint64_t)1 << 53) {
            v = (double)a / (double)b;
        } else {
            int64_t k = 20 - ((int64_t)nat_digits(&r->num) - (int64_t)nat_digits(&r->den));
            Nat q;
            if (k >= 0) {
                Nat t = nat_mul_pow10(&r->num, (size_t)k);
                q = nat_div(&t, &r->den);
                nat_free(&t);
            } else {
                Nat t = nat_mul_pow10(&r->den, (size_t)(-k));
                q = nat_div(&r->num, &t);
                nat_free(&t);
            }
            Buf s;
            buf_init(&s);
            nat_write(&s, &q);
            buf_printf(&s, "e%lld", (long long)-k);
            nat_free(&q);
            v = strtod(s.p, NULL);
            buf_free(&s);
        }
        return r->neg ? -v : v;
    }
    }
}

int num_cmp(Number a, Number b) {
    if (a.k == N_INT && b.k == N_INT) return a.i < b.i ? -1 : a.i > b.i;
    if (a.k == N_FLOAT && b.k == N_FLOAT) return a.f < b.f ? -1 : a.f > b.f ? 1 : 0;
    bool an, bn;
    Nat x, ad, y, bd;
    exact_parts(a, &an, &x, &ad);
    exact_parts(b, &bn, &y, &bd);
    bool az = nat_is_zero(&x), bz = nat_is_zero(&y);
    int r;
    if (az && bz) {
        r = 0;
    } else {
        int sa = az ? 0 : an ? -1 : 1;
        int sb = bz ? 0 : bn ? -1 : 1;
        if (sa != sb) {
            r = sa < sb ? -1 : 1;
        } else {
            Nat p = nat_mul(&x, &bd), q = nat_mul(&y, &ad);
            int m = nat_cmp(&p, &q);
            nat_free(&p);
            nat_free(&q);
            r = an ? -m : m;
        }
    }
    nat_free(&x);
    nat_free(&ad);
    nat_free(&y);
    nat_free(&bd);
    return r;
}

Number num_neg(Number n) {
    switch (n.k) {
    case N_INT:
        if (n.i == INT64_MIN) return big(false, nat_from_u64((uint64_t)1 << 63), nat_one(), true);
        return num_int(-n.i);
    case N_FLOAT: {
        Number r;
        r.k = N_FLOAT;
        r.f = -n.f;
        return r;
    }
    default: {
        uint64_t u;
        if (!n.r->neg && nat_is_one(&n.r->den) && nat_to_u64(&n.r->num, &u) && u == (uint64_t)1 << 63) return num_int(INT64_MIN);
        return big(!n.r->neg, nat_copy(&n.r->num), nat_copy(&n.r->den), n.r->plain);
    }
    }
}

Number num_abs(Number n) {
    return num_is_neg(n) ? num_neg(n) : num_retain(n);
}

bool num_add(Number a, Number b, Number *out) {
    if (a.k == N_INT && b.k == N_INT) {
        int64_t c;
        if (!__builtin_add_overflow(a.i, b.i, &c)) {
            *out = num_int(c);
            return true;
        }
    } else if (a.k == N_FLOAT || b.k == N_FLOAT) {
        return float_result(num_to_f64(a) + num_to_f64(b), out);
    }
    int plain = num_plain(a) && num_plain(b) ? 1 : -1;
    bool an, bn;
    Nat x, ad, y, bd;
    exact_parts(a, &an, &x, &ad);
    exact_parts(b, &bn, &y, &bd);
    // Knuth's method: only small common factors ever need working out.
    Nat g = nat_gcd(&ad, &bd);
    bool ok;
    if (nat_is_one(&g)) {
        Nat p = nat_mul(&x, &bd), q = nat_mul(&y, &ad);
        bool n;
        Nat m = signed_add(an, &p, bn, &q, &n);
        nat_free(&p);
        nat_free(&q);
        ok = finish(n, m, nat_mul(&ad, &bd), plain, out);
    } else {
        Nat bdg = cut(&bd, &g), adg = cut(&ad, &g);
        Nat p = nat_mul(&x, &bdg), q = nat_mul(&y, &adg);
        bool n;
        Nat t = signed_add(an, &p, bn, &q, &n);
        Nat g2 = nat_gcd(&t, &g);
        Nat bdg2 = cut(&bd, &g2);
        Nat tg = cut(&t, &g2);
        ok = finish(n, tg, nat_mul(&adg, &bdg2), plain, out);
        nat_free(&bdg);
        nat_free(&adg);
        nat_free(&p);
        nat_free(&q);
        nat_free(&t);
        nat_free(&g2);
        nat_free(&bdg2);
    }
    nat_free(&g);
    nat_free(&x);
    nat_free(&ad);
    nat_free(&y);
    nat_free(&bd);
    return ok;
}

bool num_sub(Number a, Number b, Number *out) {
    Number nb = num_neg(b);
    bool ok = num_add(a, nb, out);
    num_release(nb);
    return ok;
}

bool num_mul(Number a, Number b, Number *out) {
    if (a.k == N_INT && b.k == N_INT) {
        int64_t c;
        if (!__builtin_mul_overflow(a.i, b.i, &c)) {
            *out = num_int(c);
            return true;
        }
    } else if (a.k == N_FLOAT || b.k == N_FLOAT) {
        return float_result(num_to_f64(a) * num_to_f64(b), out);
    }
    bool an, bn;
    Nat x, ad, y, bd;
    exact_parts(a, &an, &x, &ad);
    exact_parts(b, &bn, &y, &bd);
    bool ok;
    if (nat_digits(&x) + nat_digits(&y) > MAX_DIGITS + 2 || nat_digits(&ad) + nat_digits(&bd) > MAX_DIGITS + 2) {
        ok = too_big();
    } else {
        // Cancel common factors first, so the result is already in lowest terms.
        Nat g1 = nat_gcd(&x, &bd), g2 = nat_gcd(&y, &ad);
        int plain = num_plain(a) && num_plain(b) ? 1 : -1;
        Nat p1 = cut(&x, &g1), p2 = cut(&y, &g2), q1 = cut(&ad, &g2), q2 = cut(&bd, &g1);
        ok = finish(an != bn, nat_mul(&p1, &p2), nat_mul(&q1, &q2), plain, out);
        nat_free(&g1);
        nat_free(&g2);
        nat_free(&p1);
        nat_free(&p2);
        nat_free(&q1);
        nat_free(&q2);
    }
    nat_free(&x);
    nat_free(&ad);
    nat_free(&y);
    nat_free(&bd);
    return ok;
}

bool num_div(Number a, Number b, Number *out) {
    if (num_is_zero(b)) return fail("can't divide by zero");
    if (a.k == N_INT && b.k == N_INT) {
        if (!(a.i == INT64_MIN && b.i == -1) && a.i % b.i == 0) {
            *out = num_int(a.i / b.i);
            return true;
        }
    } else if (a.k == N_FLOAT || b.k == N_FLOAT) {
        return float_result(num_to_f64(a) / num_to_f64(b), out);
    }
    bool an, bn;
    Nat x, ad, y, bd;
    exact_parts(a, &an, &x, &ad);
    exact_parts(b, &bn, &y, &bd);
    bool ok;
    if (nat_digits(&x) + nat_digits(&bd) > MAX_DIGITS + 2 || nat_digits(&ad) + nat_digits(&y) > MAX_DIGITS + 2) {
        ok = too_big();
    } else {
        Nat g1 = nat_gcd(&x, &y), g2 = nat_gcd(&ad, &bd);
        Nat p1 = cut(&x, &g1), p2 = cut(&bd, &g2), q1 = cut(&ad, &g2), q2 = cut(&y, &g1);
        ok = finish(an != bn, nat_mul(&p1, &p2), nat_mul(&q1, &q2), -1, out);
        nat_free(&g1);
        nat_free(&g2);
        nat_free(&p1);
        nat_free(&p2);
        nat_free(&q1);
        nat_free(&q2);
    }
    nat_free(&x);
    nat_free(&ad);
    nat_free(&y);
    nat_free(&bd);
    return ok;
}

/// `!%`: how many whole times `b` fits (rounded toward zero).
bool num_quot(Number a, Number b, Number *out) {
    if (num_is_zero(b)) return fail("can't divide by zero");
    if (a.k == N_INT && b.k == N_INT) {
        if (!(a.i == INT64_MIN && b.i == -1)) {
            *out = num_int(a.i / b.i);
            return true;
        }
    } else if (a.k == N_FLOAT || b.k == N_FLOAT) {
        return float_result(trunc(num_to_f64(a) / num_to_f64(b)), out);
    }
    Number d;
    if (!num_div(a, b, &d)) return false;
    bool ok = num_round(d, 0, ROUND_TRUNC, out);
    num_release(d);
    return ok;
}

/// `%`: what's left over, with the sign of the left side.
bool num_rem(Number a, Number b, Number *out) {
    if (num_is_zero(b)) return fail("can't divide by zero");
    if (a.k == N_INT && b.k == N_INT) {
        *out = num_int((a.i == INT64_MIN && b.i == -1) ? 0 : a.i % b.i);
        return true;
    }
    if (a.k == N_FLOAT || b.k == N_FLOAT) return float_result(fmod(num_to_f64(a), num_to_f64(b)), out);
    Number q, m;
    if (!num_quot(a, b, &q)) return false;
    bool ok = num_mul(b, q, &m);
    num_release(q);
    if (!ok) return false;
    ok = num_sub(a, m, out);
    num_release(m);
    return ok;
}

/// Rounds to `d` decimal places. The result is always exact.
bool num_round(Number n, size_t d, Rounding mode, Number *out) {
    switch (n.k) {
    case N_INT: *out = n; return true;
    case N_BIG:
        if (nat_is_one(&n.r->den)) {
            *out = num_retain(n);
            return true;
        }
        return round_ratio(n.r->neg, &n.r->num, &n.r->den, d, mode, out);
    default: {
        bool neg;
        Nat a, b;
        exact_of_f64(n.f, &neg, &a, &b);
        bool ok = round_ratio(neg, &a, &b, d, mode, out);
        nat_free(&a);
        nat_free(&b);
        return ok;
    }
    }
}

/// Roughly log10 of the size (not 0).
static double num_approx_log10(Number n) {
    if (n.k == N_FLOAT) return log10(fabs(n.f));
    bool neg;
    Nat a, b;
    exact_parts(n, &neg, &a, &b);
    double r = nat_approx_log10(&a) - nat_approx_log10(&b);
    nat_free(&a);
    nat_free(&b);
    return r;
}

/// `^`
bool num_pow(Number self, Number e, Number *out) {
    if (num_is_float(self) || num_is_float(e)) {
        double r = pow(num_to_f64(self), num_to_f64(e));
        if (isnan(r)) return fail("that power has no real answer");
        return float_result(r, out);
    }
    bool en;
    Nat ep, eq;
    exact_parts(e, &en, &ep, &eq);
    bool ok = true;
    if (num_is_zero(self)) {
        if (nat_is_zero(&ep)) *out = num_int(1);
        else if (en) ok = fail("can't divide by zero");
        else *out = num_int(0);
        nat_free(&ep);
        nat_free(&eq);
        return ok;
    }
    bool bn;
    Nat bp, bq;
    exact_parts(self, &bn, &bp, &bq);
    Nat rp, rq;
    // A fractional power is a root first: x ^ (p/q) = (q-th root of x) ^ p.
    if (nat_is_one(&eq)) {
        rp = bp;
        rq = bq;
    } else {
        if (bn && !nat_is_odd(&eq)) {
            nat_free(&ep);
            nat_free(&eq);
            nat_free(&bp);
            nat_free(&bq);
            return fail("that power has no real answer");
        }
        uint64_t q;
        bool have = false;
        if (nat_to_u64(&eq, &q)) {
            if (exact_root(&bp, q, &rp)) {
                if (exact_root(&bq, q, &rq)) have = true;
                else nat_free(&rp);
            }
        }
        if (!have) {
            // Not exact: work it out approximately.
            Number a = num_abs(self);
            double ev = num_to_f64(e);
            double af = num_to_f64(a);
            double m = isfinite(af) && af > 1e-300 ? pow(af, ev) : pow(10.0, num_approx_log10(a) * ev);
            num_release(a);
            bool odd = nat_is_odd(&ep);
            nat_free(&ep);
            nat_free(&eq);
            nat_free(&bp);
            nat_free(&bq);
            return float_result(bn && odd ? -m : m, out);
        }
        nat_free(&bp);
        nat_free(&bq);
    }
    bool neg = bn && nat_is_odd(&ep);
    nat_free(&eq);
    if (nat_is_one(&rp) && nat_is_one(&rq)) {
        nat_free(&ep);
        nat_free(&rp);
        nat_free(&rq);
        *out = num_int(neg ? -1 : 1);
        return true;
    }
    uint64_t p;
    if (!nat_to_u64(&ep, &p)) {
        nat_free(&ep);
        nat_free(&rp);
        nat_free(&rq);
        return too_big();
    }
    nat_free(&ep);
    double sp = nat_is_one(&rp) ? 0.0 : nat_approx_log10(&rp) * (double)p;
    double sq = nat_is_one(&rq) ? 0.0 : nat_approx_log10(&rq) * (double)p;
    if (sp > MAX_DIGITS || sq > MAX_DIGITS) {
        nat_free(&rp);
        nat_free(&rq);
        return too_big();
    }
    Nat num = nat_pow(&rp, p), den = nat_pow(&rq, p);
    nat_free(&rp);
    nat_free(&rq);
    if (en) return finish(neg, den, num, -1, out);
    return finish(neg, num, den, -1, out);
}

/// `!^`: the n-th root.
bool num_root(Number self, Number n, Number *out) {
    if (num_is_zero(n)) return fail("can't take a 0th root");
    bool odd_whole;
    switch (n.k) {
    case N_INT: odd_whole = n.i % 2 != 0; break;
    case N_BIG: odd_whole = nat_is_one(&n.r->den) && nat_is_odd(&n.r->num); break;
    default: odd_whole = false;
    }
    if (num_is_neg(self) && !odd_whole) return fail("an even root of a negative number has no real answer");
    if (num_is_float(self) || num_is_float(n)) {
        double x = num_to_f64(self), k = num_to_f64(n);
        double r = x < 0.0 ? -pow(-x, 1.0 / k) : pow(x, 1.0 / k);
        return float_result(r, out);
    }
    Number inv;
    if (!num_div(num_int(1), n, &inv)) return false;
    bool ok = num_pow(self, inv, out);
    num_release(inv);
    return ok;
}

/// Digits after the decimal point, as shown.
size_t num_decimal_places(Number n) {
    char *s = num_str(n);
    char *e = strchr(s, 'e');
    size_t r;
    if (e) {
        long long exp = strtoll(e + 1, NULL, 10);
        char *dot = memchr(s, '.', (size_t)(e - s));
        long long md = dot ? (long long)(e - dot - 1) : 0;
        long long v = md - exp;
        r = v > 0 ? (size_t)v : 0;
    } else {
        char *dot = strchr(s, '.');
        r = dot ? strlen(dot + 1) : 0;
    }
    free(s);
    return r;
}

char *num_to_hex(Number n) {
    if (!num_is_integer(n)) return NULL;
    bool neg;
    Nat a, b;
    exact_parts(n, &neg, &a, &b);
    Buf o;
    buf_init(&o);
    if (neg) buf_putc(&o, '-');
    buf_puts(&o, "0x");
    nat_write_hex(&o, &a);
    nat_free(&a);
    nat_free(&b);
    return buf_take(&o);
}

static bool ten_to(int64_t k, Number *out) {
    return num_pow(num_int(10), num_int(k), out);
}

static int64_t div_euclid3(int64_t e) {
    int64_t q = e / 3;
    if (e % 3 < 0) q -= 1;
    return q * 3;
}

/// Engineering notation: the exponent is a multiple of 3.
char *num_to_eng(Number self) {
    if (num_is_zero(self)) return xstrdup("0e0");
    Buf o;
    buf_init(&o);
    if (self.k == N_FLOAT) {
        double x = self.f;
#define RT(v) (round((v) * 1e10) / 1e10)
        int32_t e = (int32_t)div_euclid3((int64_t)(int32_t)floor(log10(fabs(x))));
        double m = RT(x / pow(10.0, e));
        if (fabs(m) >= 1000.0) {
            m = RT(m / 1000.0);
            e += 3;
        }
#undef RT
        Number mn;
        if (!num_from_f64(m, &mn)) return NULL;
        num_write(&o, mn);
        num_release(mn);
        buf_printf(&o, "e%d", e);
        return buf_take(&o);
    }
    bool neg;
    Nat a, b;
    exact_parts(self, &neg, &a, &b);
    // floor(log10 |x|), exactly.
    int64_t e = (int64_t)nat_digits(&a) - (int64_t)nat_digits(&b);
    nat_free(&a);
    nat_free(&b);
    Number abs = num_abs(self);
    Number t;
    for (;;) {
        if (!ten_to(e, &t)) goto bad;
        int c = num_cmp(abs, t);
        num_release(t);
        if (c >= 0) break;
        e -= 1;
    }
    for (;;) {
        if (!ten_to(e + 1, &t)) goto bad;
        int c = num_cmp(abs, t);
        num_release(t);
        if (c < 0) break;
        e += 1;
    }
    int64_t e3 = div_euclid3(e);
    Number m;
    if (!ten_to(e3, &t)) goto bad;
    bool ok = num_div(self, t, &m);
    num_release(t);
    if (!ok) goto bad;
    num_write(&o, m);
    num_release(m);
    buf_printf(&o, "e%lld", (long long)e3);
    num_release(abs);
    return buf_take(&o);
bad:
    num_release(abs);
    buf_free(&o);
    return NULL;
}

/// PI rounded to `d` decimals (up to 100).
bool num_pi(size_t d, Number *out) {
    if (d > 100) return fail("PI only knows 100 decimal places");
    Buf digits;
    buf_init(&digits);
    for (const char *p = PI_DIGITS; *p; p++)
        if (*p != '.') buf_putc(&digits, *p);
    Nat n = nat_parse_dec(digits.p, digits.len);
    Nat den = nat_pow10(digits.len - 1);
    bool ok = round_ratio(false, &n, &den, d, ROUND_HALF, out);
    nat_free(&n);
    nat_free(&den);
    buf_free(&digits);
    return ok;
}

// ---------- writing numbers ----------

/// Rust's shortest round-trip digits for a double: sets digits (no dot) and the
/// decimal exponent of the first digit.
static void shortest(double x, char *digits, int *exp10) {
    char buf[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, x);
        if (strtod(buf, NULL) == x) break;
    }
    // buf is like "1.2345e-07" (no sign: x > 0 here).
    char *e = strchr(buf, 'e');
    int k = 0;
    for (char *c = buf; c < e; c++)
        if (*c >= '0' && *c <= '9') digits[k++] = *c;
    // Drop trailing zeros (they come from %.*e with p = 1 only for "0").
    while (k > 1 && digits[k - 1] == '0') k--;
    digits[k] = 0;
    *exp10 = atoi(e + 1);
}

static void write_float(Buf *b, double x) {
    if (x < 0) {
        buf_putc(b, '-');
        x = -x;
    }
    char d[40];
    int e;
    shortest(x, d, &e);
    int nd = (int)strlen(d);
    if (!(x >= 1e-6 && x < 1e16)) {
        // Like Rust's {:e}: 1e-7, 1.5e20.
        buf_putc(b, d[0]);
        if (nd > 1) {
            buf_putc(b, '.');
            buf_puts(b, d + 1);
        }
        buf_printf(b, "e%d", e);
        return;
    }
    // Positional, like Rust's {}.
    if (e < 0) {
        buf_puts(b, "0.");
        for (int i = 0; i < -e - 1; i++) buf_putc(b, '0');
        buf_puts(b, d);
    } else if (e + 1 >= nd) {
        buf_puts(b, d);
        for (int i = 0; i < e + 1 - nd; i++) buf_putc(b, '0');
    } else {
        buf_put(b, d, (size_t)e + 1);
        buf_putc(b, '.');
        buf_puts(b, d + e + 1);
    }
}

static void write_ratio(Buf *b, const Ratio *r) {
    const char *sign = r->neg ? "-" : "";
    Nat ip, frac;
    nat_divmod(&r->num, &r->den, &ip, &frac);
    if (nat_is_zero(&frac)) {
        buf_puts(b, sign);
        nat_write(b, &ip);
        nat_free(&ip);
        nat_free(&frac);
        return;
    }
    size_t p;
    Nat q;
    long k = r->plain ? plain_decimals(&r->den) : -1;
    if (k >= 0) {
        // Ends: show every digit.
        p = (size_t)k;
        Nat t = nat_mul_pow10(&frac, p);
        q = nat_div(&t, &r->den);
        nat_free(&t);
    } else {
        // Goes on forever: show SHOW_DIGITS significant decimals, rounded.
        p = first_decimal(&r->num, &r->den) + SHOW_DIGITS - 1;
        Nat t = nat_mul_pow10(&frac, p);
        Nat rem;
        nat_divmod(&t, &r->den, &q, &rem);
        nat_free(&t);
        Nat r2 = nat_mul_small(&rem, 2);
        if (nat_cmp(&r2, &r->den) >= 0) {
            Nat t2 = nat_add_one(&q);
            nat_free(&q);
            q = t2;
        }
        nat_free(&r2);
        nat_free(&rem);
        Nat pp = nat_pow10(p);
        if (nat_eq(&q, &pp)) {
            Nat t2 = nat_add_one(&ip);
            nat_free(&ip);
            ip = t2;
            nat_free(&q);
            q = nat_zero();
        }
        nat_free(&pp);
    }
    Buf ds;
    buf_init(&ds);
    nat_write(&ds, &q);
    // Pad to p digits on the left, then trim trailing zeros.
    Buf full;
    buf_init(&full);
    for (size_t i = ds.len; i < p; i++) buf_putc(&full, '0');
    buf_put(&full, ds.p, ds.len);
    while (full.len && full.p[full.len - 1] == '0') full.p[--full.len] = 0;
    buf_puts(b, sign);
    nat_write(b, &ip);
    if (full.len) {
        buf_putc(b, '.');
        buf_put(b, full.p, full.len);
    }
    buf_free(&ds);
    buf_free(&full);
    nat_free(&ip);
    nat_free(&frac);
    nat_free(&q);
}

void num_write(Buf *b, Number n) {
    switch (n.k) {
    case N_INT: buf_printf(b, "%lld", (long long)n.i); break;
    case N_FLOAT: write_float(b, n.f); break;
    default: write_ratio(b, n.r);
    }
}

char *num_str(Number n) {
    Buf b;
    buf_init(&b);
    num_write(&b, n);
    return buf_take(&b);
}
