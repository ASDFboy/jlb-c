// Modes used by editors and the JLB IDE. Every reply is one line of JSON, so any
// editor can read it without guessing. They match the Rust interpreter's modes.
//
//   jlb-c --check <file | ->   Check the syntax without running anything.
//   jlb-c --ide <file>         Run a program for an editor: output is sent as soon as it's
//                              printed, and a crash also prints a JSON report.
//   jlb-c --repl               A Quick-try session that keeps its variables between snippets.

#include "jlb.h"
#include "path.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

/// Marks a line meant for the editor rather than the person (ASCII "record separator").
const char MARK = '\x1e';

void json_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else buf_putc(b, (char)*p);
        }
    }
    buf_putc(b, '"');
}

/// 1-based column of a character position.
static size_t column(const char *src, size_t len, size_t pos) {
    size_t i = 0, k = 0, last_nl = (size_t)-1;
    while (i < len && k < pos) {
        uint32_t c = utf8_next(src, len, &i);
        if (c == '\n') last_nl = k;
        k++;
    }
    return last_nl == (size_t)-1 ? pos + 1 : pos - last_nl;
}

/// The first syntax problem (false when there is none).
static bool first_problem(const char *src, size_t len, SyntaxError *err) {
    Tokens t;
    if (!lex(src, len, &t, err)) return true;
    Block b;
    bool ok = parse_program(&t, &b, err);
    tokens_free(&t);
    return !ok;
}

/// How a line's brackets balance, ignoring text in quotes and comments.
static void bracket_shape(const char *line, int *braces, bool *starts_closed, int *others) {
    *braces = 0;
    *others = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    *starts_closed = *p == '}';
    char quote = 0;
    for (size_t i = 0; line[i]; i++) {
        char c = line[i];
        if (quote) {
            if (quote == '\'' && c == '\\') {
                if (line[i + 1]) i++;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        switch (c) {
        case '"':
        case '\'': quote = c; break;
        case '>':
            if (line[i + 1] == '>') return;
            break;
        case '{': (*braces)++; break;
        case '}': (*braces)--; break;
        case '(':
        case '[': (*others)++; break;
        case ')':
        case ']': (*others)--; break;
        }
    }
}

/// A stand-in for a broken line that keeps the blocks around it in shape, so
/// checking can carry on past it. NULL when there's no safe stand-in.
static char *stand_in(const char *line) {
    int braces, others;
    bool starts_closed;
    bracket_shape(line, &braces, &starts_closed, &others);
    if (others != 0) return NULL; // a list or call that runs over several lines
    Buf b;
    buf_init(&b);
    size_t ind = 0;
    while (line[ind] == ' ' || line[ind] == '\t') ind++;
    buf_put(&b, line, ind);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) n--;
    bool opens_again = starts_closed && n > 0 && line[n - 1] == '{';
    if (opens_again) {
        // "} ELIF ... {" or "} FAIL e {": close the block above and open the next.
        buf_puts(&b, strstr(line, "FAIL") ? "} FAIL e_ {" : "} ELIF TRUE {");
    } else if (braces > 0) {
        for (int i = 0; i < braces; i++) buf_puts(&b, "IF TRUE {");
    } else if (braces < 0) {
        for (int i = 0; i < -braces; i++) buf_putc(&b, '}');
    }
    return buf_take(&b);
}

typedef struct {
    char *msg;
    int line;
    long col; // -1 = null
} Problem;

/// `{"ok":true}`, or the syntax problems: the first one's message, line and
/// column, plus every problem found (up to 10) in "errors".
static char *check(const char *src) {
    // The lines, as separate strings.
    char **lines = NULL;
    size_t nl = 0;
    const char *s = src;
    for (;;) {
        const char *e = strchr(s, '\n');
        lines = xrealloc(lines, (nl + 1) * sizeof(char *));
        lines[nl++] = e ? xstrndup(s, (size_t)(e - s)) : xstrdup(s);
        if (!e) break;
        s = e + 1;
    }
    Problem found[10];
    int nf = 0;
    while (nf < 10) {
        Buf text;
        buf_init(&text);
        for (size_t i = 0; i < nl; i++) {
            if (i) buf_putc(&text, '\n');
            buf_puts(&text, lines[i]);
        }
        SyntaxError se;
        bool bad = first_problem(text.p ? text.p : "", text.len, &se);
        if (!bad) {
            buf_free(&text);
            break;
        }
        long col = se.pos >= 0 ? (long)column(text.p, text.len, (size_t)se.pos) : -1;
        buf_free(&text);
        // The same line failing again means the stand-in didn't help: stop there.
        bool again = false;
        for (int i = 0; i < nf; i++)
            if (found[i].line == se.line) again = true;
        if (again) {
            free(se.msg);
            break;
        }
        bool done = se.line == 0 || (size_t)se.line > nl || strstr(se.msg, "never closed");
        found[nf].msg = se.msg;
        found[nf].line = se.line;
        found[nf].col = col;
        nf++;
        if (done) break;
        char *st = stand_in(lines[se.line - 1]);
        if (!st) break;
        free(lines[se.line - 1]);
        lines[se.line - 1] = st;
    }
    for (size_t i = 0; i < nl; i++) free(lines[i]);
    free(lines);
    if (nf == 0) return xstrdup("{\"ok\":true}");
    Buf o;
    buf_init(&o);
    buf_puts(&o, "{\"ok\":false,\"message\":");
    json_str(&o, found[0].msg);
    buf_printf(&o, ",\"line\":%d,\"col\":", found[0].line);
    if (found[0].col >= 0) buf_printf(&o, "%ld", found[0].col);
    else buf_puts(&o, "null");
    buf_puts(&o, ",\"errors\":[");
    for (int i = 0; i < nf; i++) {
        if (i) buf_putc(&o, ',');
        buf_puts(&o, "{\"message\":");
        json_str(&o, found[i].msg);
        buf_printf(&o, ",\"line\":%d,\"col\":", found[i].line);
        if (found[i].col >= 0) buf_printf(&o, "%ld", found[i].col);
        else buf_puts(&o, "null");
        buf_putc(&o, '}');
        free(found[i].msg);
    }
    buf_puts(&o, "]}");
    return buf_take(&o);
}

static char *read_all_stdin(size_t *len) {
    Buf b;
    buf_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) buf_put(&b, chunk, n);
    *len = b.len;
    return buf_take(&b);
}

int run_check(const char *target) {
    char *src;
    size_t len;
    if (strcmp(target, "-") == 0) {
        src = read_all_stdin(&len);
    } else {
        char *err;
        if (!read_file(target, &src, &len, &err)) {
            Buf m, o;
            buf_init(&m);
            buf_init(&o);
            buf_printf(&m, "couldn't open %s: %s", target, err);
            buf_puts(&o, "{\"ok\":false,\"message\":");
            json_str(&o, m.p);
            buf_puts(&o, ",\"line\":1,\"col\":null}");
            printf("%s\n", o.p);
            buf_free(&m);
            buf_free(&o);
            free(err);
            return 2;
        }
    }
    char *out = check(src);
    free(src);
    printf("%s\n", out);
    int code = strncmp(out, "{\"ok\":true", 10) == 0 ? 0 : 1;
    free(out);
    return code;
}

static void vars_json(Buf *b, const VarList *vars) {
    buf_putc(b, '[');
    for (size_t i = 0; i < vars->n; i++) {
        if (i) buf_putc(b, ',');
        buf_puts(b, "{\"name\":");
        json_str(b, vars->v[i].name->s);
        buf_puts(b, ",\"value\":");
        char *r = repr(*vars->v[i].value);
        json_str(b, r);
        free(r);
        buf_printf(b, ",\"type\":\"%s\"}", v_type(*vars->v[i].value));
    }
    buf_putc(b, ']');
}

/// A crash report as one line of JSON.
char *report_json(const ErrorInfo *e) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"message\":");
    json_str(&b, e->msg);
    buf_puts(&b, ",\"file\":");
    json_str(&b, e->file);
    buf_puts(&b, ",\"path\":");
    json_str(&b, e->path);
    buf_printf(&b, ",\"line\":%d,\"traced\":%s,\"frames\":[", e->line, e->traced ? "true" : "false");
    for (size_t i = 0; i < e->nframes; i++) {
        if (i) buf_putc(&b, ',');
        buf_puts(&b, "{\"name\":");
        json_str(&b, e->frames[i].name);
        buf_printf(&b, ",\"line\":%d,\"path\":", e->frames[i].line);
        json_str(&b, e->frame_paths[i]);
        buf_puts(&b, ",\"vars\":");
        vars_json(&b, &e->frames[i].vars);
        buf_putc(&b, '}');
    }
    buf_puts(&b, "],\"outside\":");
    vars_json(&b, &e->outside);
    buf_putc(&b, '}');
    return buf_take(&b);
}

/// Reads one line from standard input (without its line ending); NULL at the end.
static char *stdin_line(void) {
    Buf b;
    buf_init(&b);
    int c;
    bool any = false;
    while ((c = getchar()) != EOF) {
        any = true;
        if (c == '\n') break;
        buf_putc(&b, (char)c);
    }
    if (!any) {
        buf_free(&b);
        return NULL;
    }
    while (b.len && b.p[b.len - 1] == '\r') b.p[--b.len] = 0;
    return buf_take(&b);
}

/// Quick-try: each snippet ends with a line holding only the MARK character.
/// After running it, jlb-c prints MARK followed by one line of JSON:
/// `{"ok":true,"value":"5","type":"NUMBER"}` for an expression, `{"ok":true}` for
/// statements, or `{"ok":false,"error":{...crash report...}}`.
/// A line holding only MARK + "RESET" clears every variable.
/// INPUT prints MARK + `{"input":"the prompt"}` and reads its answer as the next line.
int run_repl(void) {
    char cwd[4096];
    const char *dir = ".";
#ifdef _WIN32
    if (_getcwd(cwd, sizeof cwd)) dir = cwd;
#else
    if (getcwd(cwd, sizeof cwd)) dir = cwd;
#endif
    Interp it;
    interp_init(&it);
    it.ide = true;
    it.repl = true;
    start_session(&it, dir);
    Buf chunk;
    buf_init(&chunk);
    char mark[2] = {MARK, 0};
    char reset[7] = {MARK, 'R', 'E', 'S', 'E', 'T', 0};
    for (;;) {
        char *line = stdin_line();
        if (!line) break;
        if (strcmp(line, reset) == 0) {
            interp_free(&it);
            interp_init(&it);
            it.ide = true;
            it.repl = true;
            start_session(&it, dir);
            chunk.len = 0;
            printf("%c{\"ok\":true}\n", MARK);
            fflush(stdout);
            free(line);
            continue;
        }
        if (strcmp(line, mark) != 0) {
            buf_puts(&chunk, line);
            buf_putc(&chunk, '\n');
            free(line);
            continue;
        }
        free(line);
        Value v;
        int r = run_snippet(&it, chunk.p ? chunk.p : "", &v);
        Buf reply;
        buf_init(&reply);
        if (r == 1) {
            char *s = repr(v);
            buf_puts(&reply, "{\"ok\":true,\"value\":");
            json_str(&reply, s);
            buf_printf(&reply, ",\"type\":\"%s\"}", v_type(v));
            free(s);
            v_release(v);
        } else if (r == 0) {
            buf_puts(&reply, "{\"ok\":true}");
        } else if (g_fault == FAULT_END) {
            g_fault = FAULT_NONE;
            buf_puts(&reply, "{\"ok\":true,\"ended\":true}");
        } else {
            ErrorInfo *e = take_error();
            char *j = report_json(e);
            buf_printf(&reply, "{\"ok\":false,\"error\":%s}", j);
            free(j);
            error_free(e);
        }
        out_flush();
        chunk.len = 0;
        if (chunk.p) chunk.p[0] = 0;
        printf("%c%s\n", MARK, reply.p);
        fflush(stdout);
        buf_free(&reply);
    }
    buf_free(&chunk);
    interp_free(&it);
    return 0;
}
