// jlb-c: the C interpreter for the .jlb language.
// Usage: jlb-c [--pause | --ide] <file.jlb>, jlb-c --check <file.jlb | ->, jlb-c --repl
// A program whose first line is `>> @interpreter rust` is handed to jlb (next to jlb-c).

#include <ctype.h>
#include <locale.h>

#include "jlb.h"
#include "path.h"

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/// True when jlb-c has a console window all to itself, which on Windows means it
/// was started by double-clicking a .jlb file rather than from a terminal.
static bool own_console_window(void) {
#ifdef _WIN32
    DWORD ids[4];
    return GetConsoleProcessList(ids, 4) == 1;
#else
    return false;
#endif
}

static void wait_for_enter(void) {
    fputs("\nPress Enter to close...", stderr);
    fflush(stderr);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
    }
}

typedef struct {
    int mode; // 0 run, 1 repl
    char *path;
    bool ide;
    int code;
} Job;

static void run_job(Job *j) {
    if (j->mode == 1) {
        j->code = run_repl();
        return;
    }
    Interp it;
    interp_init(&it);
    it.ide = j->ide;
    j->code = run_main(&it, j->path);
    // The program is over: the operating system frees everything at once.
}

// A large stack lets .jlb programs recurse deeply; it is only reserved, not used up front.
#define STACK_SIZE ((size_t)256 * 1024 * 1024)

#ifdef _WIN32
static unsigned __stdcall thread_main(void *p) {
    run_job(p);
    return 0;
}
#else
static void *thread_main(void *p) {
    run_job(p);
    return NULL;
}
#endif

static int run_big_stack(Job *j) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_beginthreadex(NULL, (unsigned)STACK_SIZE, thread_main, j, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) {
        run_job(j);
        return j->code;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);
    pthread_t t;
    if (pthread_create(&t, &attr, thread_main, j) != 0) {
        run_job(j);
    } else {
        pthread_join(t, NULL);
    }
    pthread_attr_destroy(&attr);
#endif
    return j->code;
}

static void usage(void) {
    fputs("Usage: jlb-c <file.jlb>\n", stderr);
    fputs("       jlb-c --pause <file.jlb>   wait for Enter when the program ends\n", stderr);
    fputs("       jlb-c --check <file.jlb>   check the syntax only (- reads the code from input)\n", stderr);
    fputs("       jlb-c --ide <file.jlb>     run for an editor (live output, JSON crash report)\n", stderr);
    fputs("       jlb-c --repl               start a Quick-try session for an editor\n", stderr);
}

/// The interpreter a program's first line asks for, like `>> @interpreter rust`
/// (lowercased into out). False when it doesn't ask.
static bool asked_interpreter(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[512];
    bool got = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!got) return false;
    char *p = line;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, ">>", 2) != 0 || p[2] == '>') return false; // not a >>> comment
    p += 2;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "@interpreter", 12) != 0) return false;
    p += 12;
    if (*p != ' ' && *p != '\t') return false;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (*p && *p != '\r' && *p != '\n' && n + 1 < cap) out[n++] = (char)tolower((unsigned char)*p++);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    out[n] = 0;
    return true;
}

/// Runs the program with jlb (the Rust interpreter) instead when its first line asks
/// for it. Gives back the exit code to finish with, or -1 to run it here.
static int hand_off(const char *path, bool for_ide) {
    char asked[64];
    if (!asked_interpreter(path, asked, sizeof asked) || strcmp(asked, "c") == 0) return -1;
    if (strcmp(asked, "rust") != 0) {
        fprintf(stderr, "ERROR in %s, line 1: there's no interpreter called %s. Use >> @interpreter rust or >> @interpreter c\n", path, asked);
        return 1;
    }
    // Already handed over once: run here rather than going back and forth.
    if (getenv("JLB_HANDED_OFF")) return -1;
#ifdef _WIN32
    const char *name = "jlb.exe";
    char self[4096];
    DWORD len = GetModuleFileNameA(NULL, self, sizeof self);
    if (len == 0 || len >= sizeof self) self[0] = 0;
#else
    const char *name = "jlb";
    char self[4096];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    self[len > 0 ? len : 0] = 0;
#endif
    char *dir = self[0] ? path_parent(self) : NULL;
    char *exe = dir ? path_join(dir, name) : NULL;
    free(dir);
    if (!exe || !path_is_file(exe)) {
        fprintf(stderr, "ERROR in %s, line 1: this program asks for the Rust interpreter (>> @interpreter rust), but %s isn't in the same folder as jlb-c\n", path, name);
        free(exe);
        return 2;
    }
    fflush(stdout);
#ifdef _WIN32
    _putenv("JLB_HANDED_OFF=1");
    // Each part in quotes, with quotes and the backslashes before them escaped.
    Buf cmd;
    buf_init(&cmd);
    const char *parts[3] = {exe, for_ide ? "--ide" : NULL, path};
    for (int k = 0; k < 3; k++) {
        if (!parts[k]) continue;
        if (cmd.len) buf_putc(&cmd, ' ');
        buf_putc(&cmd, '"');
        size_t slashes = 0;
        for (const char *c = parts[k]; *c; c++) {
            if (*c == '\\') {
                slashes++;
                continue;
            }
            for (size_t b = 0; b < slashes * (*c == '"' ? 2 : 1); b++) buf_putc(&cmd, '\\');
            slashes = 0;
            if (*c == '"') buf_putc(&cmd, '\\');
            buf_putc(&cmd, *c);
        }
        for (size_t b = 0; b < slashes * 2; b++) buf_putc(&cmd, '\\');
        buf_putc(&cmd, '"');
    }
        STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    int code = 2;
    char *line = buf_take(&cmd);
    if (CreateProcessA(exe, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ec = 1;
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        code = (int)ec;
    } else {
        fprintf(stderr, "ERROR: couldn't start the Rust interpreter\n");
    }
    free(line);
    free(exe);
    return code;
#else
    setenv("JLB_HANDED_OFF", "1", 1);
    char *args[4];
    int na = 0;
    args[na++] = exe;
    if (for_ide) args[na++] = "--ide";
    args[na++] = (char *)path;
    args[na] = NULL;
    execv(exe, args);
    fprintf(stderr, "ERROR: couldn't start the Rust interpreter\n");
    free(exe);
    return 2;
#endif
}

int main(int argc, char **argv) {
    // UPPER and LOWER work on any letters, not just A to Z.
    if (!setlocale(LC_CTYPE, "C.UTF-8")) setlocale(LC_CTYPE, "");
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Lines end with \n alone, as in the Rust interpreter (and editors expect).
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    static char outbuf[1 << 16];
    setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);

    int i = 1;
    bool pause = false, for_ide = false;
    if (i < argc && strcmp(argv[i], "--pause") == 0) {
        pause = true;
        i++;
    } else if (i < argc && strcmp(argv[i], "--ide") == 0) {
        for_ide = true;
        i++;
    } else if (i < argc && strcmp(argv[i], "--check") == 0) {
        int code = run_check(i + 1 < argc ? argv[i + 1] : "-");
        fflush(stdout);
        return code;
    } else if (i < argc && strcmp(argv[i], "--repl") == 0) {
        Job j = {1, NULL, true, 0};
        return run_big_stack(&j);
    } else if (i < argc && strcmp(argv[i], "--debug") == 0) {
        fputs("ERROR: jlb-c doesn't have a --debug mode yet; the Rust interpreter (jlb) does\n", stderr);
        return 2;
    }
    if (i >= argc) {
        fputs("Usage: jlb-c <file.jlb>\n", stderr);
        return 2;
    }
    const char *first = argv[i];
    if (strcmp(first, "--help") == 0 || strcmp(first, "-h") == 0) {
        usage();
        return 0;
    }
    if (strcmp(first, "--version") == 0) {
        printf("jlb-c %s\n", JLB_VERSION);
        fflush(stdout);
        return 0;
    }
    char *path = xstrdup(first);
    bool double_clicked = !for_ide && own_console_window();
    pause = pause || double_clicked;
    if (!path_is_file(path)) {
        fprintf(stderr, "ERROR: couldn't find the file %s\n", path);
        if (pause) wait_for_enter();
        return 2;
    }
    // A double-clicked program runs inside its own folder, so READ and WRITE find files next to it.
    if (double_clicked) {
        char *abs = path_canonical(path);
        if (abs) {
            char *dir = path_parent(abs);
#ifdef _WIN32
            _chdir(strncmp(dir, "\\\\?\\", 4) == 0 ? dir + 4 : dir);
#else
            if (chdir(dir) != 0) {
            }
#endif
            free(dir);
            free(path);
            path = abs;
        }
    }
    // `>> @interpreter rust` on the first line hands the program to jlb.
    int handed = hand_off(path, for_ide);
    if (handed >= 0) {
        if (pause) wait_for_enter();
        free(path);
        return handed;
    }
    Job j = {0, path, for_ide, 0};
    int code = run_big_stack(&j);
    fflush(stdout);
    if (pause) wait_for_enter();
    return code;
}
