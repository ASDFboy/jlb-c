// jlb-c: the C interpreter for the .jlb language.
// Usage: jlb-c [--pause | --ide] <file.jlb>, jlb-c --check <file.jlb | ->, jlb-c --repl

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
    Job j = {0, path, for_ide, 0};
    int code = run_big_stack(&j);
    fflush(stdout);
    if (pause) wait_for_enter();
    return code;
}
