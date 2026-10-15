// Memory, growable buffers, UTF-8 and reference-counted text.

#include "jlb.h"

static void out_of_memory(void) {
    fputs("ERROR: the computer ran out of memory\n", stderr);
    exit(101);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

// ---------- buffers ----------

void buf_init(Buf *b) {
    b->p = NULL;
    b->len = b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->p);
    buf_init(b);
}

static void buf_grow(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 32;
    while (cap < b->len + extra + 1) cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void buf_put(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) {
    buf_put(b, s, strlen(s));
}

void buf_putc(Buf *b, char c) {
    buf_grow(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return;
    buf_grow(b, (size_t)n);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

char *buf_take(Buf *b) {
    if (!b->p) return xstrdup("");
    char *p = b->p;
    buf_init(b);
    return p;
}

void buf_utf8(Buf *b, uint32_t c) {
    char t[4];
    size_t n;
    if (c < 0x80) {
        t[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        t[0] = (char)(0xC0 | (c >> 6));
        t[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        t[0] = (char)(0xE0 | (c >> 12));
        t[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        t[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        t[0] = (char)(0xF0 | (c >> 18));
        t[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        t[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    buf_put(b, t, n);
}

void buf_pad(Buf *b, const char *s, size_t width) {
    size_t n = utf8_count(s, strlen(s));
    buf_puts(b, s);
    while (n++ < width) buf_putc(b, ' ');
}

// ---------- UTF-8 ----------

uint32_t utf8_next(const char *s, size_t len, size_t *i) {
    const unsigned char *u = (const unsigned char *)s;
    size_t k = *i;
    uint32_t c = u[k];
    size_t need;
    if (c < 0x80) {
        *i = k + 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        need = 1;
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
    } else {
        *i = k + 1;
        return 0xFFFD;
    }
    if (k + need >= len) {
        *i = k + 1;
        return 0xFFFD;
    }
    uint32_t cp = c & (0x3F >> need);
    for (size_t j = 1; j <= need; j++) {
        if (k + j >= len || (u[k + j] & 0xC0) != 0x80) {
            *i = k + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (u[k + j] & 0x3F);
    }
    *i = k + need + 1;
    return cp;
}

size_t utf8_count(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        utf8_next(s, len, &i);
        n++;
    }
    return n;
}

size_t utf8_offset(const char *s, size_t len, size_t k) {
    size_t i = 0;
    while (k > 0 && i < len) {
        utf8_next(s, len, &i);
        k--;
    }
    return i;
}

// ---------- text ----------

uint32_t hash_bytes(const char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 16777619u;
    }
    return h;
}

Str *str_new(const char *p, size_t len) {
    Str *s = xmalloc(sizeof(Str) + len + 1);
    s->rc = 1;
    s->len = len;
    if (len) memcpy(s->s, p, len);
    s->s[len] = 0;
    s->hash = hash_bytes(s->s, len);
    s->nchars = utf8_count(s->s, len);
    return s;
}

Str *str_c(const char *s) {
    return str_new(s, strlen(s));
}

Str *str_fmt(const char *fmt, ...) {
    Buf b;
    buf_init(&b);
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(&b, fmt, ap);
    va_end(ap);
    return str_from_buf(&b);
}

Str *str_from_buf(Buf *b) {
    Str *s = str_new(b->p ? b->p : "", b->len);
    buf_free(b);
    return s;
}

void str_unref(Str *s) {
    if (s && --s->rc == 0) free(s);
}

bool str_eq(const Str *a, const Str *b) {
    return a == b || (a->len == b->len && a->hash == b->hash && memcmp(a->s, b->s, a->len) == 0);
}

bool str_is(const Str *a, const char *c) {
    size_t n = strlen(c);
    return a->len == n && memcmp(a->s, c, n) == 0;
}
