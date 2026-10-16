// The .jlb graphics core: windows, drawing, text, images and input. Both interpreters
// use this same code (jlb-rust builds it with the cc crate), so they draw the same pixels.
//
// Positions are .jlb's: (1, 1) is the bottom-left pixel's center, x goes right, y goes
// up, angles are degrees counterclockwise. Pixels are stored as 0xAARRGGBB with row 0
// at the top (straight alpha: color and see-through amount kept apart).

#ifndef JLB_GFX_H
#define JLB_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX_ERR 200 // size of error message buffers

// ---------- images ----------

typedef struct GfxImage {
    int refs;
    int w, h;
    uint32_t *px;
} GfxImage;

GfxImage *gfx_image_new(int w, int h, uint32_t fill);
GfxImage *gfx_image_load(const char *path, char *err);
GfxImage *gfx_image_copy(const GfxImage *src);
/// Part of an image; x, y is the bottom-left pixel of the part, in the image's own positions.
GfxImage *gfx_image_part(const GfxImage *src, int x, int y, int w, int h, char *err);
bool gfx_image_save(const GfxImage *img, const char *path, char *err);
void gfx_image_ref(GfxImage *img);
void gfx_image_unref(GfxImage *img);

// ---------- colors ----------

/// A .jlb color number (0xRRGGBB; RGB() with see-through keeps 255 - alpha in the top byte).
bool gfx_color_from_number(double v, uint32_t *out);
double gfx_color_to_number(uint32_t argb);
/// "red", "transparent", "#ff8800", ...
bool gfx_color_from_name(const char *s, size_t n, uint32_t *out);

// ---------- fonts ----------

typedef struct GfxFont GfxFont;
/// NULL path: the built-in smooth font. "pixel": the built-in pixel font.
GfxFont *gfx_font_load(const char *path, char *err);
void gfx_font_ref(GfxFont *f);
void gfx_font_unref(GfxFont *f);
bool gfx_font_is_pixel(const GfxFont *f);

// ---------- style ----------

enum {
    GS_ANCHOR = 1 << 0,
    GS_ANGLE = 1 << 1,
    GS_PIVOT = 1 << 2,
    GS_OUTLINE = 1 << 3,
    GS_ALPHA = 1 << 4,
    GS_SCALE_X = 1 << 5,
    GS_SCALE_Y = 1 << 6,
    GS_TINT = 1 << 7,
    GS_SMOOTH = 1 << 8,
    GS_RADIUS = 1 << 9,
    GS_FONT = 1 << 10,
    GS_SIZE = 1 << 11,
    GS_ALIGN = 1 << 12,
    GS_WRAP = 1 << 13,
    GS_FLIP_X = 1 << 14,
    GS_FLIP_Y = 1 << 15,
};

/// Drawing options; `set` says which ones were given.
typedef struct {
    unsigned set;
    int anchor;          // 1 top-left ... 5 center ... 9 bottom-right
    double angle;        // degrees, counterclockwise
    int pivot_anchor;    // 1..9, or 0 when pivot_x/pivot_y are a point
    double pivot_x, pivot_y; // in the shape's own pixels, (1, 1) its bottom-left
    double outline;      // 0 = filled
    int alpha;           // 0..255
    double scale_x, scale_y;
    uint32_t tint;
    bool smooth;
    double radius;       // RECT's rounded corners
    GfxFont *font;       // not owned (the style keeps its own reference in gfx_style_set)
    double size;         // text size in pixels
    int align;           // 0 left, 1 center, 2 right
    double wrap;         // text width to wrap at, 0 = none
    bool flip_x, flip_y;
} GfxStyle;

/// The current style (STYLE); every drawing command starts from it.
const GfxStyle *gfx_style_current(void);
/// Replaces the current style (keeps a reference to its font).
void gfx_style_set(const GfxStyle *s);

// ---------- drawing ----------

typedef struct {
    bool on;
    double x, y, zoom, angle;
} GfxCamera;

/// Where drawing goes: an image (a window's frame, or any image), and its camera.
typedef struct {
    GfxImage *img;
    const GfxCamera *cam;
} GfxTarget;

typedef struct {
    double x, y;
} GfxPt;

typedef enum {
    GK_PIXEL,    // x y
    GK_LINE,     // x1 y1 x2 y2 width
    GK_ARROW,    // x1 y1 x2 y2 width
    GK_LINES,    // points, width
    GK_CURVE,    // points, width
    GK_RECT,     // x y w h
    GK_CIRCLE,   // x y r
    GK_ELLIPSE,  // x y w h
    GK_TRIANGLE, // points (3)
    GK_POLYGON,  // points
    GK_NGON,     // x y r sides
    GK_STAR,     // x y r_outer r_inner points
    GK_ARC,      // x y r start end width
    GK_PIE,      // x y r start end
} GfxKind;

/// Draws a shape: `n` holds the numbers listed by GfxKind, `pts` the points.
bool gfx_shape(GfxTarget *t, GfxKind k, const double *n, int nn, const GfxPt *pts, int npts, uint32_t color,
               const GfxStyle *call, char *err);
/// Fills the whole image (no camera, no blending).
void gfx_clear(GfxImage *img, uint32_t color, bool opaque);
bool gfx_get_pixel(GfxTarget *t, double x, double y, uint32_t *out);
void gfx_flood(GfxTarget *t, double x, double y, uint32_t color);
bool gfx_draw_image(GfxTarget *t, const GfxImage *img, double x, double y, const GfxStyle *call, char *err);
bool gfx_text(GfxTarget *t, double x, double y, const char *s, size_t len, uint32_t color, const GfxStyle *call, char *err);
void gfx_text_size(const char *s, size_t len, const GfxStyle *call, double *w, double *h);
/// A point on the screen (window positions) in the world the camera looks at.
void gfx_screen_to_world(const GfxCamera *cam, int w, int h, double sx, double sy, double *wx, double *wy);

// ---------- windows and input ----------

enum {
    GFX_KEY_SPACE = ' ',
    GFX_KEY_ENTER = 128, GFX_KEY_ESCAPE, GFX_KEY_TAB, GFX_KEY_BACKSPACE, GFX_KEY_DELETE, GFX_KEY_INSERT,
    GFX_KEY_LEFT, GFX_KEY_RIGHT, GFX_KEY_UP, GFX_KEY_DOWN, GFX_KEY_HOME, GFX_KEY_END,
    GFX_KEY_PAGEUP, GFX_KEY_PAGEDOWN, GFX_KEY_SHIFT, GFX_KEY_CTRL, GFX_KEY_ALT,
    GFX_KEY_F1, // ... F12 follow
    GFX_KEYS = GFX_KEY_F1 + 12,
};
/// A key's code from its .jlb name ("a", "space", "f1", ...), or -1.
int gfx_key_code(const char *name, size_t n);
const char *gfx_key_name(int code);

enum { GFX_MOUSE_LEFT, GFX_MOUSE_RIGHT, GFX_MOUSE_MIDDLE };

typedef enum { GE_KEY_DOWN, GE_KEY_UP, GE_TYPE, GE_MOUSE_DOWN, GE_MOUSE_UP, GE_MOUSE_MOVE, GE_WHEEL, GE_RESIZE, GE_CLOSE } GfxEventKind;

typedef struct {
    GfxEventKind kind;
    int key;        // GE_KEY_*: the key; GE_MOUSE_DOWN/UP: the button
    bool repeat;    // GE_KEY_DOWN from holding the key
    double x, y;    // mouse position (window positions)
    double wheel;   // GE_WHEEL: steps, up is positive
    char text[8];   // GE_TYPE: one character, UTF-8
    int w, h;       // GE_RESIZE: the window's new size on screen
} GfxEvent;

/// The keyboard and mouse as of the last SHOW().
typedef struct {
    uint8_t down[GFX_KEYS], pressed[GFX_KEYS], released[GFX_KEYS];
    uint8_t mdown[3], mpressed[3], mreleased[3];
    double mx, my, wheel;
    bool inside;
    char typed[512];
    int ntyped;
} GfxInput;

typedef struct GfxWindow GfxWindow;

/// Headless (JLB_HEADLESS set): windows draw into memory only.
bool gfx_headless(void);
GfxWindow *gfx_window_open(int w, int h, double scale, const char *title, char *err);
GfxImage *gfx_window_frame(GfxWindow *win); // the picture being drawn
GfxCamera *gfx_window_camera(GfxWindow *win);
bool gfx_window_is_open(const GfxWindow *win);
const char *gfx_window_title(const GfxWindow *win);
void gfx_window_close(GfxWindow *win);
void gfx_window_set_title(GfxWindow *win, const char *title);
bool gfx_window_resize(GfxWindow *win, int w, int h, char *err);
void gfx_window_set_resizable(GfxWindow *win, bool on);
void gfx_window_set_fullscreen_allowed(GfxWindow *win, bool on);
void gfx_window_set_fullscreen(GfxWindow *win, bool on);
/// 0 arrow, 1 hand, 2 text, 3 crosshair, 4 hidden
void gfx_window_set_cursor(GfxWindow *win, int cursor);
void gfx_window_set_icon(GfxWindow *win, const GfxImage *icon);
/// With an ON_CLOSE handler, closing the window only asks (a GE_CLOSE event).
void gfx_window_ask_before_close(GfxWindow *win, bool on);
/// Puts the frame on screen.
void gfx_window_show(GfxWindow *win);
/// Starts a new input frame: updates the GfxInput and gives back the events since the
/// last one (valid until the next call). Returns how many.
int gfx_window_poll(GfxWindow *win, const GfxEvent **events);
const GfxInput *gfx_window_input(const GfxWindow *win);
/// How long to wait (seconds) so frames come `fps` times a second (0 = no waiting).
double gfx_frame_delay(double fps);
/// Waits until a window has news (an event) or `secs` pass.
void gfx_wait_events(double secs);
/// Headless: SHOW() calls left before windows close (JLB_FRAMES, default 1).
void gfx_headless_frame_done(void);

// ---------- for other languages (jlb-rust), without copying the structs above ----------

/// which: 0 down, 1 pressed, 2 released. Keys: GFX_KEYS of them; mouse buttons: 3.
int gfx_input_key(const GfxWindow *win, int which, int key);
int gfx_input_mouse(const GfxWindow *win, int which, int button);
void gfx_input_pointer(const GfxWindow *win, double *x, double *y, double *wheel, int *inside);
const char *gfx_input_typed(const GfxWindow *win);
int gfx_keys_count(void);
/// Event i of a gfx_window_poll() list.
void gfx_event_get(const GfxEvent *evs, int i, int *kind, int *key, int *repeat, double *x, double *y, double *wheel,
                   const char **text, int *w, int *h);
size_t gfx_style_size(void);
void gfx_camera_set(GfxWindow *win, int on, double x, double y, double zoom, double angle);
int gfx_image_width(const GfxImage *img);
int gfx_image_height(const GfxImage *img);
int gfx_image_refs(const GfxImage *img);

#endif
