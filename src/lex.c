// Turns source text into tokens.

#include "jlb.h"

const char *const WORDS[] = {
    // keywords
    "IF", "ELIF", "ELSEIF", "ELSE", "WHILE", "REPEAT", "FOR", "IN", "TO", "STEP", "BREAK", "SKIP",
    "DEF", "RETURN", "LOCAL", "GLOBAL", "CONST", "START", "TRY", "FAIL", "GOTO", "POINT", "IMPORT", "LOAD", "END", "TRUE",
    "FALSE", "NULL", "UNDEFINE", "INSERT", "DROP",
    // built-in commands
    "PRINT", "NUM", "STR", "ENG", "HEX", "LEN", "ERROR", "INPUT", "READ", "READ_LINES", "WRITE",
    "EXISTS", "TYPE", "RANDOM", "ROUND", "FLOOR", "CEIL", "TIME", "DELTA_TIME", "WAIT", "UPPER",
    "LOWER", "SPLIT", "JOIN", "FIND", "ABS", "MIN", "MAX", "TABLE_MIN", "TABLE_MAX", "SIN", "COS",
    "TAN", "CSC", "SEC", "COT", "ASIN", "ACOS", "ATAN", "RAD_SIN", "RAD_COS", "RAD_TAN", "RAD_CSC",
    "RAD_SEC", "RAD_COT", "RAD_ASIN", "RAD_ACOS", "RAD_ATAN", "PI", "SORT", "ALPHA_SORT", "KEYS",
    "AWAIT", "DONE", "CANCEL", "CLEAR", "DELETE",
    // graphics (graphics.c): keep WINDOW first and DRAW_TO second
    "WINDOW", "DRAW_TO", "SHOW", "FPS", "WINDOW_OPEN", "WINDOW_CLOSE", "WINDOW_TITLE", "WINDOW_RESIZE", "WINDOW_SET",
    "STYLE", "RESET_STYLE", "RGB", "CLEAR_WINDOW", "PIXEL", "GET_PIXEL", "LINE", "LINES", "ARROW", "CURVE", "RECT", "CIRCLE",
    "ELLIPSE", "TRIANGLE", "POLYGON", "NGON", "STAR", "ARC", "PIE", "FLOOD", "TEXT", "TEXT_SIZE", "FONT", "IMAGE",
    "IMAGE_NEW", "PART", "DRAW", "SAVE_IMAGE", "SCREENSHOT", "CAMERA", "KEY_DOWN", "KEY_PRESSED", "KEY_RELEASED",
    "KEYS_DOWN", "TYPED", "MOUSE", "MOUSE_PRESSED", "MOUSE_RELEASED", "ON_KEY", "ON_KEY_UP", "ON_CLICK",
    "ON_MOUSE_MOVE", "ON_TYPE", "ON_RESIZE", "ON_CLOSE",
};
const int N_KEYWORDS = 31;
const int GFX_FIRST = 88; // WORDS[88] is "WINDOW" (checked in graphics.c)
const int N_WORDS = (int)(sizeof WORDS / sizeof WORDS[0]);

// Longest first, so "!===" wins over "!==" and so on (the same order as the Op enum).
const char *const OP_TEXT[OP_COUNT] = {
    "", "!===", "===", "!==", "!&&", "!||", "...", "!^=", "!%=", "<==", ">==", "==", "!=", "<=", ">=",
    "&&", "||", "!&", "!|", "!~", "!^", "!%", "^=", "%=", "+=", "-=", "*=", "/=", "@=", "=", "<",
    ">", "&", "|", "~", "^", "%", "+", "-", "*", "/", "@", "!", "(", ")", "[", "]", "{", "}", ",",
    ".", ":", ";",
};

int find_word(const char *w, size_t n) {
    // FUNC is the older name for DEF, and still works.
    if (n == 4 && memcmp(w, "FUNC", 4) == 0) w = "DEF", n = 3;
    for (int i = 0; i < N_WORDS; i++) {
        if (strlen(WORDS[i]) == n && memcmp(WORDS[i], w, n) == 0) return i;
    }
    return -1;
}

int word_id(const char *w) {
    return find_word(w, strlen(w));
}

static bool is_digit(uint32_t c) {
    return c >= '0' && c <= '9';
}
static bool is_alpha(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_alnum(uint32_t c) {
    return is_alpha(c) || is_digit(c);
}
static bool is_hex(uint32_t c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
/// Unicode whitespace, like Rust's char::is_whitespace.
static bool is_space(uint32_t c) {
    return (c >= 9 && c <= 13) || c == ' ' || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

static void push(Tokens *t, Token tok) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->t = xrealloc(t->t, t->cap * sizeof(Token));
    }
    t->t[t->n++] = tok;
}

void tokens_free(Tokens *t) {
    for (size_t i = 0; i < t->n; i++) {
        if (t->t[i].k == T_NUM) num_release(t->t[i].num);
        if (t->t[i].k == T_STR || t->t[i].k == T_NAME) str_unref(t->t[i].s);
    }
    free(t->t);
    t->t = NULL;
    t->n = t->cap = 0;
}

static char *snake(const char *w) {
    Buf b;
    buf_init(&b);
    for (size_t i = 0; w[i]; i++) {
        char ch = w[i];
        if (ch >= 'A' && ch <= 'Z') {
            if (i > 0 && !(b.len && b.p[b.len - 1] == '_')) buf_putc(&b, '_');
            buf_putc(&b, (char)(ch - 'A' + 'a'));
        } else {
            buf_putc(&b, ch);
        }
    }
    return buf_take(&b);
}

static bool lex_fail(SyntaxError *err, int line, size_t pos, const char *fmt, ...) {
    Buf b;
    buf_init(&b);
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(&b, fmt, ap);
    va_end(ap);
    err->msg = buf_take(&b);
    err->line = line;
    err->pos = (long)pos;
    return false;
}

static Token tok_of(TokKind k, int line, size_t pos) {
    Token t;
    memset(&t, 0, sizeof t);
    t.k = k;
    t.line = line;
    t.pos = pos;
    return t;
}

bool lex(const char *src, size_t len, Tokens *out, SyntaxError *err) {
    // Work in characters, like the Rust version.
    size_t n = 0;
    uint32_t *c = xmalloc((len + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < len;) c[n++] = utf8_next(src, len, &i);
#define AT(k) ((k) < n ? c[k] : 0)
    out->t = NULL;
    out->n = out->cap = 0;
    size_t i = 0;
    int line = 1;
    char stack[4096];
    size_t sp = 0;    // how deep in ( [ { we are
    size_t deep = 0;  // (beyond the stack's size)
    bool ok = true;
    Buf s;
    buf_init(&s);

    while (i < n && ok) {
        uint32_t ch = c[i];
        size_t start_pos = i;
        if (ch == '\n') {
            char top = sp > 0 && deep == 0 ? stack[sp - 1] : 0;
            bool in_brackets = top == '(' || top == '[';
            bool continues = false, fresh = out->n == 0;
            if (out->n) {
                Token *last = &out->t[out->n - 1];
                continues = last->k == T_OP && last->op != OP_RPAREN && last->op != OP_RBRACK && last->op != OP_RBRACE && last->op != OP_SEMI;
                fresh = last->k == T_NEWLINE;
            }
            if (!in_brackets && !continues && !fresh) push(out, tok_of(T_NEWLINE, line, i));
            line++;
            i++;
            continue;
        }
        if (is_space(ch)) {
            i++;
            continue;
        }
        // Comments: >>> ... <<< and >> to end of line.
        if (ch == '>' && AT(i + 1) == '>') {
            if (AT(i + 2) == '>') {
                int start = line;
                i += 3;
                for (;;) {
                    if (i >= n) {
                        ok = lex_fail(err, start, start_pos, "this >>> comment is never closed with <<<");
                        break;
                    }
                    if (c[i] == '<' && AT(i + 1) == '<' && AT(i + 2) == '<') {
                        i += 3;
                        break;
                    }
                    if (c[i] == '\n') line++;
                    i++;
                }
            } else {
                while (i < n && c[i] != '\n') i++;
            }
            continue;
        }
        // Double quotes: exactly as written.
        if (ch == '"') {
            int start = line;
            size_t j = i + 1;
            s.len = 0;
            while (j < n && c[j] != '"') {
                if (c[j] == '\n') line++;
                buf_utf8(&s, c[j]);
                j++;
            }
            if (j >= n) {
                ok = lex_fail(err, start, start_pos, "this text is never closed with \"");
                break;
            }
            Token t = tok_of(T_STR, start, start_pos);
            t.s = str_new(s.p ? s.p : "", s.len);
            push(out, t);
            i = j + 1;
            continue;
        }
        // Single quotes: backslash codes.
        if (ch == '\'') {
            int start = line;
            size_t j = i + 1;
            s.len = 0;
            while (j < n && c[j] != '\'') {
                if (c[j] == '\\') {
                    uint32_t e = AT(j + 1);
                    switch (e) {
                    case '\'': buf_putc(&s, '\''); break;
                    case '"': buf_putc(&s, '"'); break;
                    case 'n': buf_putc(&s, '\n'); break;
                    case 't': buf_putc(&s, '\t'); break;
                    case '\\': buf_putc(&s, '\\'); break;
                    default: {
                        Buf eb;
                        buf_init(&eb);
                        buf_utf8(&eb, e);
                        ok = lex_fail(err, line, start_pos, "\\%s isn't a backslash code (use \\' \\\" \\n \\t or \\\\)", e ? eb.p : "");
                        buf_free(&eb);
                    }
                    }
                    if (!ok) break;
                    j += 2;
                    continue;
                }
                if (c[j] == '\n') line++;
                buf_utf8(&s, c[j]);
                j++;
            }
            if (!ok) break;
            if (j >= n) {
                ok = lex_fail(err, start, start_pos, "this text is never closed with '");
                break;
            }
            Token t = tok_of(T_STR, start, start_pos);
            t.s = str_new(s.p ? s.p : "", s.len);
            push(out, t);
            i = j + 1;
            continue;
        }
        // Numbers.
        if (is_digit(ch)) {
            size_t st = i;
            Buf lit;
            buf_init(&lit);
            if (ch == '0' && (AT(i + 1) == 'x' || AT(i + 1) == 'X')) {
                i += 2;
                size_t hs = i;
                while (is_hex(AT(i))) i++;
                if (i == hs) {
                    buf_free(&lit);
                    ok = lex_fail(err, line, start_pos, "0x must be followed by hex digits");
                    break;
                }
            } else {
                if (ch == '0' && is_digit(AT(i + 1))) {
                    buf_free(&lit);
                    ok = lex_fail(err, line, start_pos, "numbers can't start with 0 (like 025)");
                    break;
                }
                while (is_digit(AT(i))) i++;
                if (AT(i) == '.' && is_digit(AT(i + 1))) {
                    i++;
                    while (is_digit(AT(i))) i++;
                }
                if ((AT(i) == 'e' || AT(i) == 'E') && (is_digit(AT(i + 1)) || ((AT(i + 1) == '+' || AT(i + 1) == '-') && is_digit(AT(i + 2))))) {
                    i += 2;
                    while (is_digit(AT(i))) i++;
                }
            }
            for (size_t k = st; k < i; k++) buf_putc(&lit, (char)c[k]);
            Number v;
            if (!num_parse_literal(lit.p, &v)) {
                error_free(take_error());
                buf_free(&lit);
                ok = lex_fail(err, line, start_pos, "that number is too big");
                break;
            }
            buf_free(&lit);
            if (is_alnum(AT(i)) || AT(i) == '_') {
                num_release(v);
                ok = lex_fail(err, line, start_pos, "names can't start with a digit");
                break;
            }
            Token t = tok_of(T_NUM, line, start_pos);
            t.num = v;
            push(out, t);
            continue;
        }
        // Names, keywords and built-ins.
        if (is_alpha(ch) || ch == '_') {
            size_t st = i;
            while (is_alnum(AT(i)) || AT(i) == '_') i++;
            size_t wl = i - st;
            char *word = xmalloc(wl + 1);
            bool upper = false, lower = false;
            for (size_t k = 0; k < wl; k++) {
                word[k] = (char)c[st + k];
                if (word[k] >= 'A' && word[k] <= 'Z') upper = true;
                if (word[k] >= 'a' && word[k] <= 'z') lower = true;
            }
            word[wl] = 0;
            if (upper && !lower) {
                int k = find_word(word, wl);
                if (k < 0) {
                    ok = lex_fail(err, line, start_pos, "%s isn't a command .jlb knows (your own names must be lowercase)", word);
                    free(word);
                    break;
                }
                Token t = tok_of(T_KW, line, start_pos);
                t.kw = k;
                push(out, t);
            } else if (upper) {
                char *up = xstrdup(word);
                for (char *p = up; *p; p++)
                    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
                if (find_word(up, wl) >= 0) {
                    ok = lex_fail(err, line, start_pos, "did you mean %s? Built-in commands are written in ALL CAPS", up);
                } else {
                    char *sn = snake(word);
                    ok = lex_fail(err, line, start_pos, "\"%s\" should be \"%s\" (your own names are lowercase with underscores)", word, sn);
                    free(sn);
                }
                free(up);
                free(word);
                break;
            } else {
                Token t = tok_of(T_NAME, line, start_pos);
                t.s = str_new(word, wl);
                push(out, t);
            }
            free(word);
            continue;
        }
        // Operators and punctuation.
        Op matched = OP_NONE;
        for (int o = 1; o < OP_COUNT; o++) {
            const char *txt = OP_TEXT[o];
            size_t ol = strlen(txt);
            if (i + ol > n) continue;
            bool m = true;
            for (size_t k = 0; k < ol; k++) {
                if (c[i + k] != (uint32_t)(unsigned char)txt[k]) {
                    m = false;
                    break;
                }
            }
            if (m) {
                matched = (Op)o;
                break;
            }
        }
        if (matched == OP_NE_BAD) {
            ok = lex_fail(err, line, start_pos, "!= isn't an operator in .jlb; use !== (or !=== for the strict version)");
            break;
        }
        if (matched == OP_NONE) {
            Buf cb;
            buf_init(&cb);
            buf_utf8(&cb, ch);
            ok = lex_fail(err, line, start_pos, "unexpected character '%s'", cb.p);
            buf_free(&cb);
            break;
        }
        switch (matched) {
        case OP_LPAREN:
        case OP_LBRACK:
        case OP_LBRACE:
            if (sp < sizeof stack) stack[sp++] = OP_TEXT[matched][0];
            else deep++;
            break;
        case OP_RPAREN:
        case OP_RBRACK:
        case OP_RBRACE:
            if (deep) deep--;
            else if (sp) sp--;
            break;
        default: break;
        }
        Token t = tok_of(T_OP, line, start_pos);
        t.op = matched;
        push(out, t);
        i += strlen(OP_TEXT[matched]);
    }
    buf_free(&s);
    free(c);
#undef AT
    if (!ok) {
        tokens_free(out);
        return false;
    }
    push(out, tok_of(T_NEWLINE, line, n));
    push(out, tok_of(T_EOF, line, n));
    return true;
}
