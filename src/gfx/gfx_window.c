// Windows for the .jlb graphics core: Win32 on Windows, X11 on Linux (loaded only when a
// window opens, so programs without windows never need it), or headless (JLB_HEADLESS).
//
// Every window belongs to one GUI thread that owns the system's window and its events.
// The program draws into its own frame; gfx_window_show() copies it to the window's
// "front" picture and asks the GUI thread to paint it. Input goes the other way: the GUI
// thread queues events, and gfx_window_poll() takes them at the start of each frame. So a
// busy (or paused) program never makes its window stop responding.

#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
typedef CRITICAL_SECTION Mutex;
typedef CONDITION_VARIABLE Cond;
#define mutex_init(m) InitializeCriticalSection(m)
#define mutex_lock(m) EnterCriticalSection(m)
#define mutex_unlock(m) LeaveCriticalSection(m)
#define cond_init(c) InitializeConditionVariable(c)
#define cond_broadcast(c) WakeAllConditionVariable(c)
#else
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/time.h>
typedef pthread_mutex_t Mutex;
typedef pthread_cond_t Cond;
#define mutex_init(m) pthread_mutex_init(m, NULL)
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) pthread_mutex_unlock(m)
#define cond_init(c) pthread_cond_init(c, NULL)
#define cond_broadcast(c) pthread_cond_broadcast(c)
#if defined(__has_include)
#if __has_include(<X11/Xlib.h>)
#define HAVE_X11 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
// Xlib makes these macros; here they're the functions (looked up by name).
#undef RootWindow
#undef DefaultScreen
#undef DefaultVisual
#undef DefaultDepth
#undef DefaultGC
#undef DisplayWidth
#undef DisplayHeight
#undef ConnectionNumber
#endif
#endif
#endif

struct GfxWindow {
    // The program's side.
    GfxImage *frame;
    GfxCamera cam;
    GfxInput input;
    GfxEvent *evs;
    int nevs, evcap;
    char title[256];
    bool open;
    int w, h;       // drawing size
    double scale;   // pixels on screen per drawing pixel (to start with)
    // Shared with the GUI thread (under `lock`).
    uint32_t *front;
    int fw, fh;
    GfxEvent *queue;
    int nq, qcap;
    double mx, my;
    bool inside, closed, ask_close, resizable, fs_allowed, fullscreen;
    int cursor;
    uint8_t live_down[GFX_KEYS];
    int cw, ch;     // the window's size on screen (GUI thread)
#ifdef _WIN32
    HWND hwnd;
    HANDLE created;
    HICON icon;
    bool tracking;
    WINDOWPLACEMENT restore;
    LONG_PTR restore_style;
    WCHAR high; // the first half of a UTF-16 pair from WM_CHAR
#elif defined(HAVE_X11)
    Window xw;
    XImage *ximg;
    int xiw, xih;
    bool mapped;
    Cursor blank;
#endif
    bool gui_ok;
    char gui_err[GFX_ERR];
};

static Mutex lock;
static Cond news;
static int news_count;
static bool started;
static GfxWindow **wins;
static int nwins, wincap;

static void ensure_lock(void) {
    static bool done;
    if (!done) {
        mutex_init(&lock);
        cond_init(&news);
        done = true;
    }
}

static double now_secs(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}

bool gfx_headless(void) {
    const char *h = getenv("JLB_HEADLESS");
    return h && *h && strcmp(h, "0") != 0;
}

/// Queues an event from the GUI thread (lock held). Mouse moves in a row become one.
static void push_event(GfxWindow *w, GfxEvent e) {
    if (e.kind == GE_MOUSE_MOVE && w->nq && w->queue[w->nq - 1].kind == GE_MOUSE_MOVE) {
        w->queue[w->nq - 1] = e;
    } else {
        if (w->nq == w->qcap) {
            if (w->qcap >= 65536) return; // the program stopped reading: drop the extras
            w->qcap = w->qcap ? w->qcap * 2 : 64;
            w->queue = realloc(w->queue, (size_t)w->qcap * sizeof(GfxEvent));
            if (!w->queue) exit(70);
        }
        w->queue[w->nq++] = e;
    }
    news_count++;
    cond_broadcast(&news);
}

/// Where the picture sits in a window of size cw x ch: its scale and offset.
static void fit(const GfxWindow *w, int fw, int fh, int cw, int ch, double *s, int *ox, int *oy, int *dw, int *dh) {
    double sx = (double)cw / fw, sy = (double)ch / fh, k = sx < sy ? sx : sy;
    // Whole-number steps keep pixel art even, as long as it still fits.
    if (k >= 1) k = floor(k + 1e-9);
    if (k <= 0) k = 1;
    *s = k;
    *dw = (int)floor(fw * k + 0.5);
    *dh = (int)floor(fh * k + 0.5);
    *ox = (cw - *dw) / 2;
    *oy = (ch - *dh) / 2;
    (void)w;
}

/// A mouse position on screen to a pixel of the picture (bottom-left is 1, 1).
static void mouse_to_picture(GfxWindow *w, int x, int y) {
    double s;
    int ox, oy, dw, dh;
    fit(w, w->fw ? w->fw : w->w, w->fh ? w->fh : w->h, w->cw, w->ch, &s, &ox, &oy, &dw, &dh);
    int H = w->fh ? w->fh : w->h;
    w->mx = floor((x - ox) / s) + 1;
    w->my = H - floor((y - oy) / s);
}

static void key_event(GfxWindow *w, int code, bool down, bool repeat) {
    if (code < 0 || code >= GFX_KEYS) return;
    if (down && w->live_down[code]) repeat = true;
    w->live_down[code] = down;
    GfxEvent e = {0};
    e.kind = down ? GE_KEY_DOWN : GE_KEY_UP;
    e.key = code;
    e.repeat = repeat;
    e.x = w->mx, e.y = w->my;
    push_event(w, e);
}

/// Lets go of every key (the window lost focus, so their releases would go missing).
static void release_all(GfxWindow *w) {
    for (int k = 0; k < GFX_KEYS; k++) {
        if (w->live_down[k]) key_event(w, k, false, false);
    }
}

static void type_event(GfxWindow *w, unsigned cp) {
    if (cp < 32 || cp == 127 || cp > 0x10FFFF) return;
    GfxEvent e = {0};
    e.kind = GE_TYPE;
    char *o = e.text;
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) *o++ = (char)(0xC0 | cp >> 6), *o++ = (char)(0x80 | (cp & 63));
    else if (cp < 0x10000) *o++ = (char)(0xE0 | cp >> 12), *o++ = (char)(0x80 | (cp >> 6 & 63)), *o++ = (char)(0x80 | (cp & 63));
    else *o++ = (char)(0xF0 | cp >> 18), *o++ = (char)(0x80 | (cp >> 12 & 63)), *o++ = (char)(0x80 | (cp >> 6 & 63)), *o++ = (char)(0x80 | (cp & 63));
    push_event(w, e);
}

static void push_mouse(GfxWindow *w, GfxEventKind kind, int button) {
    GfxEvent e = {0};
    e.kind = kind;
    e.key = button;
    e.x = w->mx, e.y = w->my;
    push_event(w, e);
}

static void closed_by_user(GfxWindow *w) {
    if (w->ask_close) {
        GfxEvent e = {0};
        e.kind = GE_CLOSE;
        push_event(w, e);
    } else {
        w->closed = true;
        news_count++;
        cond_broadcast(&news);
    }
}

// ======================================================================== Win32
#ifdef _WIN32

#define WM_APP_CREATE (WM_APP + 1)
#define WM_APP_DESTROY (WM_APP + 2)
#define WM_APP_TITLE (WM_APP + 3)
#define WM_APP_SIZE (WM_APP + 4)
#define WM_APP_FLAGS (WM_APP + 5)
#define WM_APP_FULLSCREEN (WM_APP + 6)
#define WM_APP_ICON (WM_APP + 7)

static DWORD gui_thread_id;
static HANDLE gui_ready;
static const WCHAR CLASS_NAME[] = L"JlbWindow";

static WCHAR *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = malloc((size_t)(n > 0 ? n : 1) * sizeof(WCHAR));
    if (!w) exit(70);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    else w[0] = 0;
    return w;
}

static int vk_to_key(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (int)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (int)(vk - VK_NUMPAD0 + '0');
    if (vk >= VK_F1 && vk <= VK_F12) return GFX_KEY_F1 + (int)(vk - VK_F1);
    switch (vk) {
    case VK_SPACE: return ' ';
    case VK_RETURN: return GFX_KEY_ENTER;
    case VK_ESCAPE: return GFX_KEY_ESCAPE;
    case VK_TAB: return GFX_KEY_TAB;
    case VK_BACK: return GFX_KEY_BACKSPACE;
    case VK_DELETE: return GFX_KEY_DELETE;
    case VK_INSERT: return GFX_KEY_INSERT;
    case VK_LEFT: return GFX_KEY_LEFT;
    case VK_RIGHT: return GFX_KEY_RIGHT;
    case VK_UP: return GFX_KEY_UP;
    case VK_DOWN: return GFX_KEY_DOWN;
    case VK_HOME: return GFX_KEY_HOME;
    case VK_END: return GFX_KEY_END;
    case VK_PRIOR: return GFX_KEY_PAGEUP;
    case VK_NEXT: return GFX_KEY_PAGEDOWN;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return GFX_KEY_SHIFT;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return GFX_KEY_CTRL;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return GFX_KEY_ALT;
    case VK_OEM_MINUS: case VK_SUBTRACT: return '-';
    case VK_OEM_PLUS: return '=';
    case VK_ADD: return '+';
    case VK_MULTIPLY: return '*';
    case VK_DIVIDE: return '/';
    case VK_DECIMAL: return '.';
    case VK_OEM_4: return '[';
    case VK_OEM_6: return ']';
    case VK_OEM_5: return '\\';
    case VK_OEM_1: return ';';
    case VK_OEM_7: return '\'';
    case VK_OEM_COMMA: return ',';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2: return '/';
    case VK_OEM_3: return '`';
    }
    return -1;
}

static DWORD style_for(const GfxWindow *w) {
    DWORD s = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (w->resizable) s |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    return s;
}

static void set_fullscreen(GfxWindow *w, bool on) {
    if (on == w->fullscreen) return;
    if (on) {
        w->restore.length = sizeof w->restore;
        GetWindowPlacement(w->hwnd, &w->restore);
        w->restore_style = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(w->hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(w->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(w->hwnd, GWL_STYLE, (LONG_PTR)(style_for(w) | WS_VISIBLE));
        SetWindowPlacement(w->hwnd, &w->restore);
        SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    w->fullscreen = on;
}

static void client_size_for(GfxWindow *w, int *ww, int *wh) {
    RECT r = {0, 0, (LONG)ceil(w->w * w->scale), (LONG)ceil(w->h * w->scale)};
    AdjustWindowRect(&r, style_for(w), FALSE);
    *ww = r.right - r.left;
    *wh = r.bottom - r.top;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    GfxWindow *w = (GfxWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        mutex_lock(&lock);
        int cw = cr.right, ch = cr.bottom;
        if (w->front && cw > 0 && ch > 0) {
            double s;
            int ox, oy, dw, dh;
            fit(w, w->fw, w->fh, cw, ch, &s, &ox, &oy, &dw, &dh);
            BITMAPINFO bi = {0};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = w->fw;
            bi.bmiHeader.biHeight = -w->fh; // rows from the top
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, ox, oy, dw, dh, 0, 0, w->fw, w->fh, w->front, &bi, DIB_RGB_COLORS, SRCCOPY);
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            RECT bars[4] = {{0, 0, cw, oy}, {0, oy + dh, cw, ch}, {0, oy, ox, oy + dh}, {ox + dw, oy, cw, oy + dh}};
            for (int i = 0; i < 4; i++) {
                if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top) FillRect(dc, &bars[i], black);
            }
        }
        mutex_unlock(&lock);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        mutex_lock(&lock);
        w->cw = LOWORD(lp), w->ch = HIWORD(lp);
        if (wp != SIZE_MINIMIZED) {
            GfxEvent e = {0};
            e.kind = GE_RESIZE;
            e.w = w->cw, e.h = w->ch;
            push_event(w, e);
        }
        mutex_unlock(&lock);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            static const WORD shapes[] = {32512 /* arrow */, 32649 /* hand */, 32513 /* text */, 32515 /* cross */};
            SetCursor(w->cursor == 4 ? NULL : LoadCursorW(NULL, MAKEINTRESOURCEW(shapes[w->cursor < 0 || w->cursor > 3 ? 0 : w->cursor])));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        bool alt_enter = wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000);
        if ((wp == VK_F11 || alt_enter) && w->fs_allowed) {
            if (!(lp & (1 << 30))) set_fullscreen(w, !w->fullscreen);
            return 0;
        }
        mutex_lock(&lock);
        key_event(w, vk_to_key(wp), true, (lp & (1 << 30)) != 0);
        mutex_unlock(&lock);
        if (msg == WM_SYSKEYDOWN && wp != VK_MENU) break; // keep Alt+F4 working
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        mutex_lock(&lock);
        key_event(w, vk_to_key(wp), false, false);
        mutex_unlock(&lock);
        if (msg == WM_SYSKEYUP) break;
        return 0;
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        unsigned cp = c;
        if (c >= 0xD800 && c < 0xDC00) {
            w->high = c;
            return 0;
        }
        if (c >= 0xDC00 && c < 0xE000) {
            if (!w->high) return 0;
            cp = 0x10000 + ((unsigned)(w->high - 0xD800) << 10) + (c - 0xDC00);
            w->high = 0;
        }
        mutex_lock(&lock);
        type_event(w, cp);
        mutex_unlock(&lock);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!w->tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            w->tracking = true;
        }
        mutex_lock(&lock);
        mouse_to_picture(w, (short)LOWORD(lp), (short)HIWORD(lp));
        w->inside = true;
        push_mouse(w, GE_MOUSE_MOVE, -1);
        mutex_unlock(&lock);
        return 0;
    case WM_MOUSELEAVE:
        w->tracking = false;
        mutex_lock(&lock);
        w->inside = false;
        mutex_unlock(&lock);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? GFX_MOUSE_LEFT : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? GFX_MOUSE_RIGHT : GFX_MOUSE_MIDDLE;
        bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        if (down) SetCapture(hwnd);
        else ReleaseCapture();
        mutex_lock(&lock);
        mouse_to_picture(w, (short)LOWORD(lp), (short)HIWORD(lp));
        push_mouse(w, down ? GE_MOUSE_DOWN : GE_MOUSE_UP, b);
        mutex_unlock(&lock);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        mutex_lock(&lock);
        GfxEvent e = {0};
        e.kind = GE_WHEEL;
        e.wheel = (short)HIWORD(wp) / (double)WHEEL_DELTA;
        e.x = w->mx, e.y = w->my;
        push_event(w, e);
        mutex_unlock(&lock);
        return 0;
    }
    case WM_KILLFOCUS:
        mutex_lock(&lock);
        release_all(w);
        mutex_unlock(&lock);
        return 0;
    case WM_CLOSE: {
        mutex_lock(&lock);
        closed_by_user(w);
        bool now = w->closed;
        mutex_unlock(&lock);
        if (now) DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        mutex_lock(&lock);
        w->closed = true;
        news_count++;
        cond_broadcast(&news);
        mutex_unlock(&lock);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    case WM_APP_DESTROY:
        DestroyWindow(hwnd);
        return 0;
    case WM_APP_TITLE: {
        WCHAR *t = widen(w->title);
        SetWindowTextW(hwnd, t);
        free(t);
        return 0;
    }
    case WM_APP_SIZE: {
        if (w->fullscreen) return 0;
        int ww, wh;
        client_size_for(w, &ww, &wh);
        SetWindowPos(hwnd, NULL, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER);
        return 0;
    }
    case WM_APP_FLAGS:
        if (!w->fullscreen) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)(style_for(w) | WS_VISIBLE));
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        }
        if (!w->fs_allowed && w->fullscreen) set_fullscreen(w, false);
        return 0;
    case WM_APP_FULLSCREEN:
        set_fullscreen(w, wp != 0);
        return 0;
    case WM_APP_ICON:
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)w->icon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)w->icon);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void gui_create(GfxWindow *w) {
    int ww, wh;
    client_size_for(w, &ww, &wh);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    WCHAR *t = widen(w->title);
    HWND h = CreateWindowExW(0, CLASS_NAME, t, style_for(w), (sw - ww) / 2 > 0 ? (sw - ww) / 2 : 0, (sh - wh) / 2 > 0 ? (sh - wh) / 2 : 0, ww, wh,
                             NULL, NULL, GetModuleHandleW(NULL), NULL);
    free(t);
    if (!h) {
        snprintf(w->gui_err, GFX_ERR, "couldn't open a window (error %lu)", (unsigned long)GetLastError());
        w->gui_ok = false;
        return;
    }
    w->hwnd = h;
    RECT cr;
    GetClientRect(h, &cr);
    w->cw = cr.right, w->ch = cr.bottom;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)w);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    w->gui_ok = true;
}

static DWORD WINAPI gui_main(LPVOID arg) {
    (void)arg;
    // Real pixels, not pixels the system stretches (blurry) on high-resolution screens.
    HMODULE user = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SetAware)(void);
    SetAware aware = user ? (SetAware)(void *)GetProcAddress(user, "SetProcessDPIAware") : NULL;
    if (aware) aware();
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    MSG m;
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE); // makes this thread's message queue
    SetEvent(gui_ready);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!m.hwnd && m.message == WM_APP_CREATE) {
            GfxWindow *w = (GfxWindow *)m.lParam;
            gui_create(w);
            SetEvent(w->created);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static bool plat_open(GfxWindow *w, char *err) {
    if (!started) {
        timeBeginPeriod(1); // so waiting for the next frame is accurate
        gui_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE t = CreateThread(NULL, 0, gui_main, NULL, 0, &gui_thread_id);
        if (!t) {
            snprintf(err, GFX_ERR, "couldn't start the window thread");
            return false;
        }
        CloseHandle(t);
        WaitForSingleObject(gui_ready, INFINITE);
        started = true;
    }
    w->created = CreateEventW(NULL, TRUE, FALSE, NULL);
    PostThreadMessageW(gui_thread_id, WM_APP_CREATE, 0, (LPARAM)w);
    WaitForSingleObject(w->created, INFINITE);
    CloseHandle(w->created);
    if (!w->gui_ok) {
        snprintf(err, GFX_ERR, "%s", w->gui_err);
        return false;
    }
    return true;
}

static void plat_post(GfxWindow *w, UINT msg, WPARAM wp) {
    if (w->hwnd) PostMessageW(w->hwnd, msg, wp, 0);
}

static void plat_present(GfxWindow *w) {
    if (w->hwnd) InvalidateRect(w->hwnd, NULL, FALSE);
}
static void plat_close(GfxWindow *w) { plat_post(w, WM_APP_DESTROY, 0); }
static void plat_title(GfxWindow *w) { plat_post(w, WM_APP_TITLE, 0); }
static void plat_size(GfxWindow *w) { plat_post(w, WM_APP_SIZE, 0); }
static void plat_flags(GfxWindow *w) { plat_post(w, WM_APP_FLAGS, 0); }
static void plat_fullscreen(GfxWindow *w, bool on) { plat_post(w, WM_APP_FULLSCREEN, on); }
static void plat_cursor(GfxWindow *w) {
    POINT p;
    GetCursorPos(&p);
    SetCursorPos(p.x, p.y); // makes the window pick up the new cursor right away
    (void)w;
}
static void plat_icon(GfxWindow *w, const GfxImage *img) {
    BITMAPV5HEADER bh = {0};
    bh.bV5Size = sizeof bh;
    bh.bV5Width = img->w;
    bh.bV5Height = -img->h;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void *bits;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color) return;
    memcpy(bits, img->px, (size_t)img->w * img->h * 4);
    HBITMAP mask = CreateBitmap(img->w, img->h, 1, 1, NULL);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!icon) return;
    HICON old = w->icon;
    w->icon = icon;
    plat_post(w, WM_APP_ICON, 0);
    (void)old; // still in use until the window takes the new one; freed with the program
}

// ======================================================================== X11
#elif defined(HAVE_X11)

// libX11 is opened when the first window opens.
static struct {
    Display *(*OpenDisplay)(const char *);
    int (*Pending)(Display *);
    int (*NextEvent)(Display *, XEvent *);
    int (*EventsQueued)(Display *, int);
    int (*PeekEvent)(Display *, XEvent *);
    Window (*CreateSimpleWindow)(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
    int (*DestroyWindow)(Display *, Window);
    int (*MapWindow)(Display *, Window);
    int (*StoreName)(Display *, Window, const char *);
    int (*SelectInput)(Display *, Window, long);
    Atom (*InternAtom)(Display *, const char *, Bool);
    Status (*SetWMProtocols)(Display *, Window, Atom *, int);
    int (*LookupString)(XKeyEvent *, char *, int, KeySym *, XComposeStatus *);
    KeySym (*LookupKeysym)(XKeyEvent *, int);
    XImage *(*CreateImage)(Display *, Visual *, unsigned, int, int, char *, unsigned, unsigned, int, int);
    int (*PutImage)(Display *, Drawable, GC, XImage *, int, int, int, int, unsigned, unsigned);
    int (*Flush)(Display *);
    void (*SetWMNormalHints)(Display *, Window, XSizeHints *);
    XSizeHints *(*AllocSizeHints)(void);
    int (*Free)(void *);
    int (*ChangeProperty)(Display *, Window, Atom, Atom, int, int, const unsigned char *, int);
    Status (*SendEvent)(Display *, Window, Bool, long, XEvent *);
    Cursor (*CreateFontCursor)(Display *, unsigned);
    int (*DefineCursor)(Display *, Window, Cursor);
    Pixmap (*CreateBitmapFromData)(Display *, Drawable, const char *, unsigned, unsigned);
    Cursor (*CreatePixmapCursor)(Display *, Pixmap, Pixmap, XColor *, XColor *, unsigned, unsigned);
    int (*FreePixmap)(Display *, Pixmap);
    int (*ConnectionNumber)(Display *);
    int (*ResizeWindow)(Display *, Window, unsigned, unsigned);
    int (*DefaultScreen)(Display *);
    Window (*RootWindow)(Display *, int);
    Visual *(*DefaultVisual)(Display *, int);
    int (*DefaultDepth)(Display *, int);
    GC (*DefaultGC)(Display *, int);
    int (*DisplayWidth)(Display *, int);
    int (*DisplayHeight)(Display *, int);
    int (*FillRectangle)(Display *, Drawable, GC, int, int, unsigned, unsigned);
    int (*SetForeground)(Display *, GC, unsigned long);
    int (*MoveWindow)(Display *, Window, int, int);
    XErrorHandler (*SetErrorHandler)(XErrorHandler);
} X;

static Display *dpy;
static int wake_pipe[2];
static Atom wm_delete, net_wm_state, net_fullscreen, net_wm_name, utf8_string, net_wm_icon;
static Cursor cursors[4];
static int screen;

/// X errors (like drawing into a window something else just destroyed) are expected
/// now and then; Xlib's own handler would end the program.
static int x_error(Display *d, XErrorEvent *e) {
    (void)d, (void)e;
    return 0;
}

static bool load_x11(char *err) {
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(err, GFX_ERR, "can't open a window: libX11 isn't installed (it's needed for windows on Linux)");
        return false;
    }
#define L(name)                                                                       \
    if (!(*(void **)&X.name = dlsym(h, "X" #name))) {                                \
        snprintf(err, GFX_ERR, "can't open a window: libX11 has no X%s", #name);      \
        return false;                                                                 \
    }
    L(OpenDisplay) L(Pending) L(NextEvent) L(EventsQueued) L(PeekEvent) L(CreateSimpleWindow) L(DestroyWindow) L(MapWindow)
    L(StoreName) L(SelectInput) L(InternAtom) L(SetWMProtocols) L(LookupString) L(LookupKeysym) L(CreateImage) L(PutImage)
    L(Flush) L(SetWMNormalHints) L(AllocSizeHints) L(Free) L(ChangeProperty) L(SendEvent) L(CreateFontCursor) L(DefineCursor)
    L(CreateBitmapFromData) L(CreatePixmapCursor) L(FreePixmap) L(ConnectionNumber) L(ResizeWindow) L(DefaultScreen)
    L(RootWindow) L(DefaultVisual) L(DefaultDepth) L(DefaultGC) L(DisplayWidth) L(DisplayHeight) L(FillRectangle)
    L(SetForeground) L(MoveWindow) L(SetErrorHandler)
#undef L
    return true;
}

// Requests from the program to the GUI thread (under `lock`).
typedef enum { RQ_CREATE, RQ_DESTROY, RQ_TITLE, RQ_SIZE, RQ_FLAGS, RQ_FULLSCREEN, RQ_CURSOR, RQ_ICON, RQ_PAINT } ReqKind;
typedef struct {
    ReqKind kind;
    GfxWindow *w;
    int arg;
    GfxImage *img;
} Req;
static Req *reqs;
static int nreqs, reqcap;
static Cond req_done;
static int created_count;

static void request(ReqKind k, GfxWindow *w, int arg, GfxImage *img) {
    mutex_lock(&lock);
    if (nreqs == reqcap) {
        reqcap = reqcap ? reqcap * 2 : 32;
        reqs = realloc(reqs, (size_t)reqcap * sizeof(Req));
        if (!reqs) exit(70);
    }
    // One paint request per window is enough.
    bool dup = false;
    if (k == RQ_PAINT) {
        for (int i = 0; i < nreqs; i++) dup |= reqs[i].kind == RQ_PAINT && reqs[i].w == w;
    }
    if (!dup) reqs[nreqs++] = (Req){k, w, arg, img};
    mutex_unlock(&lock);
    char c = 1;
    ssize_t r = write(wake_pipe[1], &c, 1);
    (void)r;
}

static int keysym_to_key(KeySym k) {
    if (k >= XK_a && k <= XK_z) return (int)(k - XK_a + 'a');
    if (k >= XK_A && k <= XK_Z) return (int)(k - XK_A + 'a');
    if (k >= XK_0 && k <= XK_9) return (int)(k - XK_0 + '0');
    if (k >= XK_KP_0 && k <= XK_KP_9) return (int)(k - XK_KP_0 + '0');
    if (k >= XK_F1 && k <= XK_F12) return GFX_KEY_F1 + (int)(k - XK_F1);
    switch (k) {
    case XK_space: return ' ';
    case XK_Return: case XK_KP_Enter: return GFX_KEY_ENTER;
    case XK_Escape: return GFX_KEY_ESCAPE;
    case XK_Tab: case XK_ISO_Left_Tab: return GFX_KEY_TAB;
    case XK_BackSpace: return GFX_KEY_BACKSPACE;
    case XK_Delete: case XK_KP_Delete: return GFX_KEY_DELETE;
    case XK_Insert: return GFX_KEY_INSERT;
    case XK_Left: case XK_KP_Left: return GFX_KEY_LEFT;
    case XK_Right: case XK_KP_Right: return GFX_KEY_RIGHT;
    case XK_Up: case XK_KP_Up: return GFX_KEY_UP;
    case XK_Down: case XK_KP_Down: return GFX_KEY_DOWN;
    case XK_Home: return GFX_KEY_HOME;
    case XK_End: return GFX_KEY_END;
    case XK_Prior: return GFX_KEY_PAGEUP;
    case XK_Next: return GFX_KEY_PAGEDOWN;
    case XK_Shift_L: case XK_Shift_R: return GFX_KEY_SHIFT;
    case XK_Control_L: case XK_Control_R: return GFX_KEY_CTRL;
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R: return GFX_KEY_ALT;
    case XK_KP_Add: return '+';
    case XK_KP_Subtract: return '-';
    case XK_KP_Multiply: return '*';
    case XK_KP_Divide: return '/';
    case XK_KP_Decimal: return '.';
    }
    if (k > ' ' && k < 127) return (int)k; // punctuation keysyms are their characters
    return -1;
}

static void size_hints(GfxWindow *w) {
    XSizeHints *h = X.AllocSizeHints();
    if (!h) return;
    int cw = (int)ceil(w->w * w->scale), ch = (int)ceil(w->h * w->scale);
    if (!w->resizable && !w->fullscreen) {
        h->flags = PMinSize | PMaxSize;
        h->min_width = h->max_width = cw;
        h->min_height = h->max_height = ch;
    } else {
        h->flags = PMinSize;
        h->min_width = w->w < 64 ? w->w : 64;
        h->min_height = w->h < 64 ? w->h : 64;
    }
    X.SetWMNormalHints(dpy, w->xw, h);
    X.Free(h);
}

static void x_fullscreen(GfxWindow *w, bool on) {
    if (on && !w->resizable) {
        w->fullscreen = true; // let the window manager make it any size
        size_hints(w);
    }
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = w->xw;
    e.xclient.message_type = net_wm_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = on ? 1 : 0;
    e.xclient.data.l[1] = (long)net_fullscreen;
    e.xclient.data.l[3] = 1;
    X.SendEvent(dpy, X.RootWindow(dpy, screen), False, SubstructureNotifyMask | SubstructureRedirectMask, &e);
    w->fullscreen = on;
    if (!on) size_hints(w);
}

static void x_title(GfxWindow *w) {
    X.StoreName(dpy, w->xw, w->title);
    X.ChangeProperty(dpy, w->xw, net_wm_name, utf8_string, 8, PropModeReplace, (const unsigned char *)w->title, (int)strlen(w->title));
}

static void x_create(GfxWindow *w) {
    int cw = (int)ceil(w->w * w->scale), ch = (int)ceil(w->h * w->scale);
    Window root = X.RootWindow(dpy, screen);
    int sx = (X.DisplayWidth(dpy, screen) - cw) / 2, sy = (X.DisplayHeight(dpy, screen) - ch) / 2;
    w->xw = X.CreateSimpleWindow(dpy, root, sx > 0 ? sx : 0, sy > 0 ? sy : 0, (unsigned)cw, (unsigned)ch, 0, 0, 0);
    if (!w->xw) {
        snprintf(w->gui_err, GFX_ERR, "couldn't open a window");
        w->gui_ok = false;
        return;
    }
    w->cw = cw, w->ch = ch;
    X.SelectInput(dpy, w->xw, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                                  EnterWindowMask | LeaveWindowMask | StructureNotifyMask | ExposureMask | FocusChangeMask);
    X.SetWMProtocols(dpy, w->xw, &wm_delete, 1);
    x_title(w);
    size_hints(w);
    X.MapWindow(dpy, w->xw);
    X.MoveWindow(dpy, w->xw, sx > 0 ? sx : 0, sy > 0 ? sy : 0);
    X.Flush(dpy);
    w->gui_ok = true;
}

static void x_paint(GfxWindow *w) {
    if (!w->xw || w->closed) return;
    int cw = w->cw, ch = w->ch;
    if (cw < 1 || ch < 1) return;
    if (!w->ximg || w->xiw != cw || w->xih != ch) {
        if (w->ximg) w->ximg->f.destroy_image(w->ximg);
        char *data = malloc((size_t)cw * ch * 4);
        if (!data) return;
        w->ximg = X.CreateImage(dpy, X.DefaultVisual(dpy, screen), (unsigned)X.DefaultDepth(dpy, screen), ZPixmap, 0, data, (unsigned)cw,
                                (unsigned)ch, 32, 0);
        if (!w->ximg) {
            free(data);
            return;
        }
        w->xiw = cw, w->xih = ch;
    }
    uint32_t *dst = (uint32_t *)w->ximg->data;
    mutex_lock(&lock);
    if (w->front) {
        double s;
        int ox, oy, dw, dh;
        fit(w, w->fw, w->fh, cw, ch, &s, &ox, &oy, &dw, &dh);
        // Nearest pixel, row by row, with black around the picture.
        int *cols = malloc((size_t)cw * sizeof(int));
        if (cols) {
            for (int x = 0; x < cw; x++) {
                int u = x - ox;
                cols[x] = (u < 0 || u >= dw) ? -1 : (int)(u / s) < w->fw ? (int)(u / s) : w->fw - 1;
            }
            for (int y = 0; y < ch; y++) {
                uint32_t *row = dst + (size_t)y * (w->ximg->bytes_per_line / 4);
                int v = y - oy;
                if (v < 0 || v >= dh) {
                    memset(row, 0, (size_t)cw * 4);
                    continue;
                }
                int sv = (int)(v / s);
                if (sv >= w->fh) sv = w->fh - 1;
                const uint32_t *src = w->front + (size_t)sv * w->fw;
                for (int x = 0; x < cw; x++) row[x] = cols[x] < 0 ? 0 : src[cols[x]] & 0xFFFFFF;
            }
            free(cols);
        }
    }
    mutex_unlock(&lock);
    X.PutImage(dpy, w->xw, X.DefaultGC(dpy, screen), w->ximg, 0, 0, 0, 0, (unsigned)cw, (unsigned)ch);
    X.Flush(dpy);
}

static GfxWindow *by_xw(Window xw) {
    GfxWindow *found = NULL;
    mutex_lock(&lock);
    for (int i = 0; i < nwins && !found; i++) {
        if (wins[i]->xw == xw) found = wins[i];
    }
    mutex_unlock(&lock);
    return found;
}

static void x_event(XEvent *e) {
    GfxWindow *w = by_xw(e->xany.window);
    if (!w) return;
    switch (e->type) {
    case Expose:
        if (e->xexpose.count == 0) x_paint(w);
        break;
    case ConfigureNotify:
        if (e->xconfigure.width != w->cw || e->xconfigure.height != w->ch) {
            mutex_lock(&lock);
            w->cw = e->xconfigure.width, w->ch = e->xconfigure.height;
            GfxEvent ev = {0};
            ev.kind = GE_RESIZE;
            ev.w = w->cw, ev.h = w->ch;
            push_event(w, ev);
            mutex_unlock(&lock);
            x_paint(w);
        }
        break;
    case KeyPress:
    case KeyRelease: {
        KeySym ks = X.LookupKeysym(&e->xkey, 0);
        int code = keysym_to_key(ks);
        if (e->type == KeyRelease && X.EventsQueued(dpy, QueuedAfterReading)) {
            // Held keys repeat as release + press at the same moment: that's not a release.
            XEvent nx;
            X.PeekEvent(dpy, &nx);
            if (nx.type == KeyPress && nx.xkey.time == e->xkey.time && nx.xkey.keycode == e->xkey.keycode) break;
        }
        if (e->type == KeyPress && (code == GFX_KEY_F1 + 10 || (code == GFX_KEY_ENTER && (e->xkey.state & Mod1Mask))) && w->fs_allowed) {
            if (!w->live_down[code]) x_fullscreen(w, !w->fullscreen);
            mutex_lock(&lock);
            w->live_down[code] = 1;
            mutex_unlock(&lock);
            break;
        }
        mutex_lock(&lock);
        key_event(w, code, e->type == KeyPress, false);
        if (e->type == KeyPress) {
            char buf[16];
            KeySym k2;
            int n = X.LookupString(&e->xkey, buf, sizeof buf, &k2, NULL);
            // Latin-1 characters from the keyboard.
            for (int i = 0; i < n; i++) type_event(w, (unsigned char)buf[i]);
        }
        mutex_unlock(&lock);
        break;
    }
    case MotionNotify:
        mutex_lock(&lock);
        mouse_to_picture(w, e->xmotion.x, e->xmotion.y);
        w->inside = true;
        push_mouse(w, GE_MOUSE_MOVE, -1);
        mutex_unlock(&lock);
        break;
    case EnterNotify:
    case LeaveNotify:
        mutex_lock(&lock);
        w->inside = e->type == EnterNotify;
        mutex_unlock(&lock);
        break;
    case ButtonPress:
    case ButtonRelease: {
        unsigned b = e->xbutton.button;
        mutex_lock(&lock);
        mouse_to_picture(w, e->xbutton.x, e->xbutton.y);
        if (b == 4 || b == 5) {
            if (e->type == ButtonPress) {
                GfxEvent ev = {0};
                ev.kind = GE_WHEEL;
                ev.wheel = b == 4 ? 1 : -1;
                ev.x = w->mx, ev.y = w->my;
                push_event(w, ev);
            }
        } else if (b >= 1 && b <= 3) {
            int btn = b == 1 ? GFX_MOUSE_LEFT : b == 3 ? GFX_MOUSE_RIGHT : GFX_MOUSE_MIDDLE;
            push_mouse(w, e->type == ButtonPress ? GE_MOUSE_DOWN : GE_MOUSE_UP, btn);
        }
        mutex_unlock(&lock);
        break;
    }
    case FocusOut:
        mutex_lock(&lock);
        release_all(w);
        mutex_unlock(&lock);
        break;
    case DestroyNotify:
        // Gone (closed by something outside the program): it can't ask any more.
        if (e->xdestroywindow.window == w->xw) {
            w->xw = 0;
            mutex_lock(&lock);
            w->closed = true;
            news_count++;
            cond_broadcast(&news);
            mutex_unlock(&lock);
        }
        break;
    case ClientMessage:
        if ((Atom)e->xclient.data.l[0] == wm_delete) {
            mutex_lock(&lock);
            closed_by_user(w);
            bool now = w->closed;
            mutex_unlock(&lock);
            if (now) {
                X.DestroyWindow(dpy, w->xw);
                X.Flush(dpy);
                w->xw = 0;
            }
        }
        break;
    }
}

static void x_cursor(GfxWindow *w, int c) {
    if (c == 4) {
        if (!w->blank) {
            static const char none[1] = {0};
            Pixmap p = X.CreateBitmapFromData(dpy, w->xw, none, 1, 1);
            XColor black;
            memset(&black, 0, sizeof black);
            w->blank = X.CreatePixmapCursor(dpy, p, p, &black, &black, 0, 0);
            X.FreePixmap(dpy, p);
        }
        X.DefineCursor(dpy, w->xw, w->blank);
    } else {
        static const unsigned shapes[4] = {XC_left_ptr, XC_hand2, XC_xterm, XC_crosshair};
        if (c < 0 || c > 3) c = 0;
        if (!cursors[c]) cursors[c] = X.CreateFontCursor(dpy, shapes[c]);
        X.DefineCursor(dpy, w->xw, cursors[c]);
    }
}

static void x_icon(GfxWindow *w, GfxImage *img) {
    size_t n = (size_t)img->w * img->h;
    long *d = malloc((n + 2) * sizeof(long));
    if (!d) return;
    d[0] = img->w, d[1] = img->h;
    for (size_t i = 0; i < n; i++) d[i + 2] = (long)img->px[i];
    X.ChangeProperty(dpy, w->xw, net_wm_icon, 6 /* XA_CARDINAL */, 32, PropModeReplace, (const unsigned char *)d, (int)(n + 2));
    free(d);
}

static void *gui_main(void *arg) {
    (void)arg;
    int xfd = X.ConnectionNumber(dpy);
    for (;;) {
        // Requests from the program.
        mutex_lock(&lock);
        Req *todo = NULL;
        int n = nreqs;
        if (n) {
            todo = malloc((size_t)n * sizeof(Req));
            if (todo) memcpy(todo, reqs, (size_t)n * sizeof(Req));
            nreqs = 0;
        }
        mutex_unlock(&lock);
        for (int i = 0; i < n && todo; i++) {
            GfxWindow *w = todo[i].w;
            switch (todo[i].kind) {
            case RQ_CREATE:
                x_create(w);
                mutex_lock(&lock);
                created_count++;
                cond_broadcast(&req_done);
                mutex_unlock(&lock);
                break;
            case RQ_DESTROY:
                if (w->xw) {
                    X.DestroyWindow(dpy, w->xw);
                    w->xw = 0;
                }
                break;
            case RQ_TITLE: if (w->xw) x_title(w); break;
            case RQ_SIZE:
                if (w->xw && !w->fullscreen) {
                    size_hints(w);
                    X.ResizeWindow(dpy, w->xw, (unsigned)ceil(w->w * w->scale), (unsigned)ceil(w->h * w->scale));
                }
                break;
            case RQ_FLAGS:
                if (w->xw) {
                    size_hints(w);
                    if (!w->fs_allowed && w->fullscreen) x_fullscreen(w, false);
                }
                break;
            case RQ_FULLSCREEN: if (w->xw) x_fullscreen(w, todo[i].arg != 0); break;
            case RQ_CURSOR: if (w->xw) x_cursor(w, todo[i].arg); break;
            case RQ_ICON:
                if (w->xw) x_icon(w, todo[i].img);
                gfx_image_unref(todo[i].img);
                break;
            case RQ_PAINT: x_paint(w); break;
            }
        }
        free(todo);
        X.Flush(dpy);
        while (X.Pending(dpy)) {
            XEvent e;
            X.NextEvent(dpy, &e);
            x_event(&e);
        }
        struct pollfd fds[2] = {{xfd, POLLIN, 0}, {wake_pipe[0], POLLIN, 0}};
        if (poll(fds, 2, 100) > 0 && (fds[1].revents & POLLIN)) {
            char buf[64];
            while (read(wake_pipe[0], buf, sizeof buf) > 0) {
            }
        }
    }
    return NULL;
}

static bool plat_open(GfxWindow *w, char *err) {
    if (!started) {
        if (!load_x11(err)) return false;
        dpy = X.OpenDisplay(NULL);
        if (!dpy) {
            snprintf(err, GFX_ERR, "can't open a window: there's no screen to show it on (DISPLAY isn't set). Set JLB_HEADLESS=1 to draw without one");
            return false;
        }
        X.SetErrorHandler(x_error);
        screen = X.DefaultScreen(dpy);
        wm_delete = X.InternAtom(dpy, "WM_DELETE_WINDOW", False);
        net_wm_state = X.InternAtom(dpy, "_NET_WM_STATE", False);
        net_fullscreen = X.InternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        net_wm_name = X.InternAtom(dpy, "_NET_WM_NAME", False);
        utf8_string = X.InternAtom(dpy, "UTF8_STRING", False);
        net_wm_icon = X.InternAtom(dpy, "_NET_WM_ICON", False);
        if (pipe(wake_pipe) != 0) {
            snprintf(err, GFX_ERR, "couldn't start the window thread");
            return false;
        }
        fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
        cond_init(&req_done);
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, gui_main, NULL) != 0) {
            snprintf(err, GFX_ERR, "couldn't start the window thread");
            return false;
        }
        started = true;
    }
    mutex_lock(&lock);
    int want = created_count + 1;
    mutex_unlock(&lock);
    request(RQ_CREATE, w, 0, NULL);
    mutex_lock(&lock);
    while (created_count < want) pthread_cond_wait(&req_done, &lock);
    mutex_unlock(&lock);
    if (!w->gui_ok) {
        snprintf(err, GFX_ERR, "%s", w->gui_err);
        return false;
    }
    return true;
}

static void plat_present(GfxWindow *w) { request(RQ_PAINT, w, 0, NULL); }
static void plat_close(GfxWindow *w) { request(RQ_DESTROY, w, 0, NULL); }
static void plat_title(GfxWindow *w) { request(RQ_TITLE, w, 0, NULL); }
static void plat_size(GfxWindow *w) { request(RQ_SIZE, w, 0, NULL); }
static void plat_flags(GfxWindow *w) { request(RQ_FLAGS, w, 0, NULL); }
static void plat_fullscreen(GfxWindow *w, bool on) { request(RQ_FULLSCREEN, w, on, NULL); }
static void plat_cursor(GfxWindow *w) { request(RQ_CURSOR, w, w->cursor, NULL); }
static void plat_icon(GfxWindow *w, const GfxImage *img) {
    GfxImage *c = gfx_image_copy(img);
    if (c) request(RQ_ICON, w, 0, c);
}

// ======================================================================== none
#else

static bool plat_open(GfxWindow *w, char *err) {
    (void)w;
    snprintf(err, GFX_ERR, "windows aren't supported on this system yet. Set JLB_HEADLESS=1 to draw without one");
    return false;
}
static void plat_present(GfxWindow *w) { (void)w; }
static void plat_close(GfxWindow *w) { (void)w; }
static void plat_title(GfxWindow *w) { (void)w; }
static void plat_size(GfxWindow *w) { (void)w; }
static void plat_flags(GfxWindow *w) { (void)w; }
static void plat_fullscreen(GfxWindow *w, bool on) { (void)w, (void)on; }
static void plat_cursor(GfxWindow *w) { (void)w; }
static void plat_icon(GfxWindow *w, const GfxImage *img) { (void)w, (void)img; }

#endif

// ======================================================================== common

static int frames_left = -1; // headless: SHOW() calls before windows close

GfxWindow *gfx_window_open(int w, int h, double scale, const char *title, char *err) {
    ensure_lock();
    if (w < 1 || h < 1 || w > 16384 || h > 16384) {
        snprintf(err, GFX_ERR, "a window's size must be from 1 to 16384 pixels each way, not %d x %d", w, h);
        return NULL;
    }
    if (!(scale > 0) || scale > 64) {
        snprintf(err, GFX_ERR, "a window's scale must be more than 0 (and at most 64)");
        return NULL;
    }
    GfxWindow *win = calloc(1, sizeof *win);
    if (!win) exit(70);
    win->w = w, win->h = h, win->scale = scale;
    win->frame = gfx_image_new(w, h, 0xFF000000u);
    if (!win->frame) {
        free(win);
        snprintf(err, GFX_ERR, "a %d x %d window is too big", w, h);
        return NULL;
    }
    win->cam.zoom = 1;
    snprintf(win->title, sizeof win->title, "%s", title && *title ? title : "jlb");
    win->resizable = true;
    win->fs_allowed = true;
    win->open = true;
    win->fw = w, win->fh = h;
    win->front = calloc((size_t)w * h, 4);
    if (!win->front) exit(70);
    mutex_lock(&lock);
    if (nwins == wincap) {
        wincap = wincap ? wincap * 2 : 8;
        wins = realloc(wins, (size_t)wincap * sizeof *wins);
        if (!wins) exit(70);
    }
    wins[nwins++] = win;
    mutex_unlock(&lock);
    if (gfx_headless()) {
        if (frames_left < 0) {
            const char *f = getenv("JLB_FRAMES");
            frames_left = f && atoi(f) > 0 ? atoi(f) : 1;
        }
        return win;
    }
    if (!plat_open(win, err)) {
        win->open = false;
        win->closed = true;
        return NULL; // stays in the list (the GUI thread may still look at it)
    }
    return win;
}

GfxImage *gfx_window_frame(GfxWindow *win) { return win->frame; }
GfxCamera *gfx_window_camera(GfxWindow *win) { return &win->cam; }
bool gfx_window_is_open(const GfxWindow *win) { return win->open; }
const char *gfx_window_title(const GfxWindow *win) { return win->title; }

void gfx_window_close(GfxWindow *win) {
    if (!win->open) return;
    win->open = false;
    mutex_lock(&lock);
    win->closed = true;
    mutex_unlock(&lock);
    if (!gfx_headless()) plat_close(win);
}

void gfx_window_set_title(GfxWindow *win, const char *title) {
    mutex_lock(&lock);
    snprintf(win->title, sizeof win->title, "%s", title);
    mutex_unlock(&lock);
    if (!gfx_headless() && win->open) plat_title(win);
}

bool gfx_window_resize(GfxWindow *win, int w, int h, char *err) {
    if (w < 1 || h < 1 || w > 16384 || h > 16384) {
        snprintf(err, GFX_ERR, "a window's size must be from 1 to 16384 pixels each way, not %d x %d", w, h);
        return false;
    }
    GfxImage *f = gfx_image_new(w, h, 0xFF000000u);
    if (!f) {
        snprintf(err, GFX_ERR, "a %d x %d window is too big", w, h);
        return false;
    }
    gfx_image_unref(win->frame);
    win->frame = f;
    mutex_lock(&lock);
    win->w = w, win->h = h;
    mutex_unlock(&lock);
    if (!gfx_headless() && win->open) plat_size(win);
    return true;
}

void gfx_window_set_resizable(GfxWindow *win, bool on) {
    win->resizable = on;
    if (!gfx_headless() && win->open) plat_flags(win);
}

void gfx_window_set_fullscreen_allowed(GfxWindow *win, bool on) {
    win->fs_allowed = on;
    if (!gfx_headless() && win->open) plat_flags(win);
}

void gfx_window_set_fullscreen(GfxWindow *win, bool on) {
    if (on && !win->fs_allowed) return;
    if (!gfx_headless() && win->open) plat_fullscreen(win, on);
    else win->fullscreen = on;
}

void gfx_window_set_cursor(GfxWindow *win, int cursor) {
    win->cursor = cursor;
    if (!gfx_headless() && win->open) plat_cursor(win);
}

void gfx_window_set_icon(GfxWindow *win, const GfxImage *icon) {
    if (!gfx_headless() && win->open) plat_icon(win, icon);
}

void gfx_window_ask_before_close(GfxWindow *win, bool on) {
    mutex_lock(&lock);
    win->ask_close = on;
    mutex_unlock(&lock);
}

static void save_screenshot(GfxWindow *win) {
    const char *path = getenv("JLB_SCREENSHOT");
    if (!path || !*path || win != wins[0]) return;
    char err[GFX_ERR];
    if (!gfx_image_save(win->frame, path, err)) fprintf(stderr, "%s\n", err);
}

void gfx_window_show(GfxWindow *win) {
    if (!win->open) return;
    if (gfx_headless()) {
        save_screenshot(win);
        return;
    }
    mutex_lock(&lock);
    if (win->fw != win->frame->w || win->fh != win->frame->h) {
        free(win->front);
        win->fw = win->frame->w, win->fh = win->frame->h;
        win->front = malloc((size_t)win->fw * win->fh * 4);
        if (!win->front) exit(70);
    }
    memcpy(win->front, win->frame->px, (size_t)win->fw * win->fh * 4);
    mutex_unlock(&lock);
    plat_present(win);
}

void gfx_headless_frame_done(void) {
    if (!gfx_headless() || frames_left < 0) return;
    if (--frames_left <= 0) {
        for (int i = 0; i < nwins; i++) {
            if (wins[i]->open) {
                save_screenshot(wins[i]);
                wins[i]->open = false;
                wins[i]->closed = true;
            }
        }
    }
}

int gfx_window_poll(GfxWindow *win, const GfxEvent **events) {
    GfxInput *in = &win->input;
    memset(in->pressed, 0, sizeof in->pressed);
    memset(in->released, 0, sizeof in->released);
    memset(in->mpressed, 0, sizeof in->mpressed);
    memset(in->mreleased, 0, sizeof in->mreleased);
    in->wheel = 0;
    in->ntyped = 0;
    in->typed[0] = 0;
    mutex_lock(&lock);
    // Swap the queue out, so the GUI thread can keep adding while this frame reads.
    GfxEvent *q = win->queue;
    int n = win->nq, cap = win->qcap;
    win->queue = win->evs;
    win->qcap = win->evcap;
    win->nq = 0;
    win->evs = q;
    win->evcap = cap;
    win->nevs = n;
    in->mx = win->mx, in->my = win->my;
    in->inside = win->inside;
    bool closed = win->closed;
    mutex_unlock(&lock);
    if (closed && win->open) win->open = false;
    for (int i = 0; i < n; i++) {
        GfxEvent *e = &win->evs[i];
        switch (e->kind) {
        case GE_KEY_DOWN:
            if (!in->down[e->key]) in->pressed[e->key] = 1;
            in->down[e->key] = 1;
            break;
        case GE_KEY_UP:
            if (in->down[e->key]) in->released[e->key] = 1;
            in->down[e->key] = 0;
            break;
        case GE_TYPE: {
            size_t l = strlen(e->text);
            if (in->ntyped + (int)l < (int)sizeof in->typed - 1) {
                memcpy(in->typed + in->ntyped, e->text, l);
                in->ntyped += (int)l;
                in->typed[in->ntyped] = 0;
            }
            break;
        }
        case GE_MOUSE_DOWN:
            if (e->key >= 0 && e->key < 3) {
                if (!in->mdown[e->key]) in->mpressed[e->key] = 1;
                in->mdown[e->key] = 1;
            }
            break;
        case GE_MOUSE_UP:
            if (e->key >= 0 && e->key < 3) {
                if (in->mdown[e->key]) in->mreleased[e->key] = 1;
                in->mdown[e->key] = 0;
            }
            break;
        case GE_WHEEL:
            in->wheel += e->wheel;
            break;
        default:
            break;
        }
    }
    *events = win->evs;
    return n;
}

const GfxInput *gfx_window_input(const GfxWindow *win) { return &win->input; }

double gfx_frame_delay(double fps) {
    static double next;
    if (fps <= 0 || gfx_headless()) {
        next = 0;
        return 0;
    }
    double t = now_secs(), step = 1 / fps;
    if (next == 0 || t > next + step * 4) next = t; // first frame, or far behind: start again from now
    next += step;
    double wait = next - t;
    return wait > 0 ? wait : 0;
}

void gfx_wait_events(double secs) {
    if (gfx_headless()) return;
    ensure_lock();
    mutex_lock(&lock);
    int seen = news_count;
#ifdef _WIN32
    DWORD ms = (DWORD)(secs * 1000);
    while (news_count == seen) {
        if (!SleepConditionVariableCS(&news, &lock, ms)) break;
    }
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct timespec until;
    double end = tv.tv_sec + tv.tv_usec / 1e6 + secs;
    until.tv_sec = (time_t)end;
    until.tv_nsec = (long)((end - (double)until.tv_sec) * 1e9);
    while (news_count == seen) {
        if (pthread_cond_timedwait(&news, &lock, &until) != 0) break;
    }
#endif
    mutex_unlock(&lock);
}

// ---------- for other languages ----------

int gfx_input_key(const GfxWindow *win, int which, int key) {
    if (key < 0 || key >= GFX_KEYS) return 0;
    const GfxInput *in = &win->input;
    return (which == 0 ? in->down : which == 1 ? in->pressed : in->released)[key];
}

int gfx_input_mouse(const GfxWindow *win, int which, int button) {
    if (button < 0 || button > 2) return 0;
    const GfxInput *in = &win->input;
    return (which == 0 ? in->mdown : which == 1 ? in->mpressed : in->mreleased)[button];
}

void gfx_input_pointer(const GfxWindow *win, double *x, double *y, double *wheel, int *inside) {
    *x = win->input.mx;
    *y = win->input.my;
    *wheel = win->input.wheel;
    *inside = win->input.inside;
}

const char *gfx_input_typed(const GfxWindow *win) { return win->input.typed; }
int gfx_keys_count(void) { return GFX_KEYS; }

void gfx_event_get(const GfxEvent *evs, int i, int *kind, int *key, int *repeat, double *x, double *y, double *wheel,
                   const char **text, int *w, int *h) {
    const GfxEvent *e = &evs[i];
    *kind = (int)e->kind, *key = e->key, *repeat = e->repeat, *x = e->x, *y = e->y, *wheel = e->wheel;
    *text = e->text, *w = e->w, *h = e->h;
}

size_t gfx_style_size(void) { return sizeof(GfxStyle); }

void gfx_camera_set(GfxWindow *win, int on, double x, double y, double zoom, double angle) {
    win->cam.on = on != 0, win->cam.x = x, win->cam.y = y, win->cam.zoom = zoom, win->cam.angle = angle;
}

int gfx_image_width(const GfxImage *img) { return img->w; }
int gfx_image_height(const GfxImage *img) { return img->h; }
int gfx_image_refs(const GfxImage *img) { return img->refs; }
