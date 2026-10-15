// File paths, for the main file, IMPORT and LOAD.

#include "path.h"

#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define SEP '\\'
static bool is_sep(char c) {
    return c == '/' || c == '\\';
}
#else
#include <unistd.h>
#define SEP '/'
static bool is_sep(char c) {
    return c == '/';
}
#endif

/// dir + name, like Rust's Path::join (an absolute name replaces dir).
char *path_join(const char *dir, const char *name) {
    bool absolute = is_sep(name[0]);
#ifdef _WIN32
    if (name[0] && name[1] == ':') absolute = true;
#endif
    if (absolute || !dir[0]) return xstrdup(name);
    Buf b;
    buf_init(&b);
    buf_puts(&b, dir);
    if (!is_sep(dir[strlen(dir) - 1])) buf_putc(&b, SEP);
    buf_puts(&b, name);
    return buf_take(&b);
}

/// Everything before the last separator ("" when there's none).
char *path_parent(const char *p) {
    size_t n = strlen(p);
    while (n > 1 && is_sep(p[n - 1])) n--;
    size_t i = n;
    while (i > 0 && !is_sep(p[i - 1])) i--;
    if (i == 0) return xstrdup("");
    size_t end = i - 1;
    // Keep the root: "/x" has the parent "/".
    if (end == 0) return xstrndup(p, 1);
#ifdef _WIN32
    if (end == 2 && p[1] == ':') return xstrndup(p, 3);
#endif
    return xstrndup(p, end);
}

/// The last part of the path.
char *path_file_name(const char *p) {
    size_t n = strlen(p);
    while (n > 1 && is_sep(p[n - 1])) n--;
    size_t i = n;
    while (i > 0 && !is_sep(p[i - 1])) i--;
    return xstrndup(p + i, n - i);
}

/// The file name without its last extension.
char *path_stem(const char *p) {
    char *f = path_file_name(p);
    char *dot = strrchr(f, '.');
    if (dot && dot != f) *dot = 0;
    return f;
}

/// The full path with links resolved, or NULL when the file doesn't exist.
char *path_canonical(const char *p) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(p, &st) != 0) return NULL;
    char *full = _fullpath(NULL, p, 0);
    if (!full) return NULL;
    // Rust's canonicalize() gives \\?\C:\... paths on Windows.
    Buf b;
    buf_init(&b);
    buf_puts(&b, "\\\\?\\");
    for (char *c = full; *c; c++) buf_putc(&b, *c == '/' ? '\\' : *c);
    free(full);
    return buf_take(&b);
#else
    return realpath(p, NULL);
#endif
}

bool path_is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

bool path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/// An OS error, the way Rust writes it: "No such file or directory (os error 2)".
char *os_error(int err) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s (os error %d)", strerror(err), err);
    return buf_take(&b);
}

bool read_file(const char *path, char **text, size_t *len, char **error) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *error = os_error(errno);
        return false;
    }
    Buf b;
    buf_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(&b, chunk, n);
    bool bad = ferror(f);
    int e = errno;
    fclose(f);
    if (bad) {
        buf_free(&b);
        *error = os_error(e);
        return false;
    }
    // Like Rust's read_to_string: the file must be valid UTF-8.
    for (size_t i = 0; i < b.len;) {
        size_t st = i;
        uint32_t c = utf8_next(b.p, b.len, &i);
        if (c == 0xFFFD && !(i - st == 3 && (unsigned char)b.p[st] == 0xEF)) {
            buf_free(&b);
            *error = xstrdup("stream did not contain valid UTF-8");
            return false;
        }
    }
    *len = b.len;
    *text = buf_take(&b);
    return true;
}

bool write_file(const char *path, const char *text, size_t len, char **error) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        *error = os_error(errno);
        return false;
    }
    bool ok = fwrite(text, 1, len, f) == len;
    int e = errno;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        *error = os_error(e);
        return false;
    }
    return true;
}
