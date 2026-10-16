// Graphics commands for jlb-c: windows, drawing, text, images and input. The drawing
// itself is the shared core in gfx/ (the same code jlb-rust uses), so this file only
// turns .jlb values into its inputs.

#include "jlb.h"
#include "gfx/gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

// The commands, in the order they're listed in WORDS (lex.c).
enum {
    B_WINDOW, B_DRAW_TO, B_SHOW, B_FPS, B_WINDOW_OPEN, B_WINDOW_CLOSE, B_WINDOW_TITLE, B_WINDOW_RESIZE, B_WINDOW_SET,
    B_STYLE, B_RESET_STYLE, B_RGB, B_CLEAR, B_PIXEL, B_GET_PIXEL, B_LINE, B_LINES, B_ARROW, B_CURVE, B_RECT, B_CIRCLE,
    B_ELLIPSE, B_TRIANGLE, B_POLYGON, B_NGON, B_STAR, B_ARC, B_PIE, B_FLOOD, B_TEXT, B_TEXT_SIZE, B_FONT, B_IMAGE,
    B_IMAGE_NEW, B_PART, B_DRAW, B_SAVE_IMAGE, B_SCREENSHOT, B_CAMERA, B_KEY_DOWN, B_KEY_PRESSED, B_KEY_RELEASED,
    B_KEYS_DOWN, B_TYPED, B_MOUSE, B_MOUSE_PRESSED, B_MOUSE_RELEASED, B_ON_KEY, B_ON_KEY_UP, B_ON_CLICK,
    B_ON_MOUSE_MOVE, B_ON_TYPE, B_ON_RESIZE, B_ON_CLOSE,
};

enum { H_KEY, H_KEY_UP, H_CLICK, H_MOVE, H_TYPE, H_RESIZE, H_CLOSE };
#define ANY_KEY (-2)

typedef struct {
    int kind, key;
    Value fn;
} Handler;

typedef struct {
    GfxWindow *w;
    Handler *h;
    int nh, cap;
    bool closed_by_program;
} Win;

static Win *wins;
static int nwins;
static int sel_win = -1;   // DRAW_TO(window): where drawing and input go
static Slot *draw_img;     // DRAW_TO(image name): drawing goes into this variable's image
static double fps = 60;
static bool asked_open;    // the program checks WINDOW_OPEN(), so it handles closing itself

// ---------- inputs ----------

static bool argc(const char *name, int n, int lo, int hi) {
    if (n >= lo && n <= hi) return true;
    char need[32];
    if (lo == hi) snprintf(need, sizeof need, "%d", lo);
    else snprintf(need, sizeof need, "%d to %d", lo, hi);
    return fail("%s takes %s input%s, but got %d", name, need, lo == 1 && hi == 1 ? "" : "s", n);
}

static bool num(const char *name, Value v, double *out) {
    switch (v.t) {
    case V_NUM: *out = num_to_f64(v.num); break;
    case V_NULL: *out = 0; break;
    case V_TEXT: return fail("%s needs a number, not the text \"%s\" (use NUM() to convert)", name, v.s->s);
    default: return fail("%s needs a number, not a %s", name, v_type(v));
    }
    if (!isfinite(*out)) return fail("%s needs an ordinary number", name);
    return true;
}

static bool whole(const char *name, const char *what, Value v, int lo, int hi, int *out) {
    double d;
    if (!num(name, v, &d)) return false;
    if (d != floor(d) || d < lo || d > hi) return fail("%s: %s must be a whole number from %d to %d", name, what, lo, hi);
    *out = (int)d;
    return true;
}

static bool flag(const char *name, Value v, bool *out) {
    if (v.t != V_BOOL) return fail("%s needs TRUE or FALSE, not a %s", name, v_type(v));
    *out = v.b;
    return true;
}

static Value item(const Table *t, size_t i) {
    return i < t->n ? t->items[i]->v : NULL_VALUE;
}

static Slot *named(const Table *t, const char *k) {
    return om_get(&t->named, k, strlen(k));
}

static bool color(const char *name, Value v, uint32_t *out) {
    if (v.t == V_NUM) {
        if (gfx_color_from_number(num_to_f64(v.num), out)) return true;
        return fail("%s: that number isn't a color (colors go from 0x000000 to 0xFFFFFF, or use RGB())", name);
    }
    if (v.t == V_TEXT) {
        if (gfx_color_from_name(v.s->s, v.s->len, out)) return true;
        return fail("%s: there's no color called \"%s\" (try \"red\", \"skyblue\", \"#ff8800\", or a number like 0xFF8800)", name, v.s->s);
    }
    if (v.t == V_TABLE) {
        // [r, g, b], [r, g, b, alpha] or [r:, g:, b:, alpha:]
        const Table *t = v.tb.tab;
        Value parts[4];
        const char *keys[4] = {"r", "g", "b", "alpha"};
        for (int i = 0; i < 4; i++) {
            Slot *s = named(t, keys[i]);
            parts[i] = s ? s->v : item(t, (size_t)i);
        }
        int c[4] = {0, 0, 0, 255};
        for (int i = 0; i < 4; i++) {
            if (i == 3 && parts[3].t == V_NULL) break;
            if (!whole(name, "each part of a color", parts[i], 0, 255, &c[i])) return false;
        }
        *out = (uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
        return true;
    }
    return fail("%s needs a color (like 0xFF8800, RGB(255, 136, 0) or \"orange\"), not a %s", name, v_type(v));
}

static bool point(const char *name, Value v, GfxPt *out) {
    if (v.t != V_TABLE) return fail("%s needs points like [x, y], not a %s", name, v_type(v));
    const Table *t = v.tb.tab;
    Slot *sx = named(t, "x"), *sy = named(t, "y");
    if ((!sx && t->n < 1) || (!sy && t->n < 2)) return fail("%s needs points like [x, y] or [x: 1, y: 2]", name);
    Value x = sx ? sx->v : item(t, 0), y = sy ? sy->v : item(t, 1);
    return num(name, x, &out->x) && num(name, y, &out->y);
}

static bool points(const char *name, Value v, int min, GfxPt **out, int *n) {
    if (v.t != V_TABLE) return fail("%s needs a table of points like [[1, 1], [10, 5]], not a %s", name, v_type(v));
    const Table *t = v.tb.tab;
    if ((int)t->n < min) return fail("%s needs at least %d point%s", name, min, min == 1 ? "" : "s");
    GfxPt *p = xmalloc(t->n * sizeof(GfxPt) + sizeof(GfxPt));
    for (size_t i = 0; i < t->n; i++) {
        if (!point(name, t->items[i]->v, &p[i])) {
            free(p);
            return false;
        }
    }
    *out = p;
    *n = (int)t->n;
    return true;
}

static const char OPTIONS[] = "anchor, angle, pivot, outline, alpha, scale, scale_x, scale_y, flip_x, flip_y, tint, smooth, "
                              "radius, font, size, align and wrap";

/// An options table, like [angle: 45, anchor: 7].
static bool options(const char *name, Value v, GfxStyle *st) {
    memset(st, 0, sizeof *st);
    if (v.t == V_NULL) return true;
    if (v.t != V_TABLE) return fail("%s's options go in a table, like [angle: 45], not a %s", name, v_type(v));
    const Table *t = v.tb.tab;
    if (t->n) return fail("%s's options need names, like [angle: 45]", name);
    for (size_t i = 0; i < t->named.n; i++) {
        const char *k = t->named.e[i].key->s;
        Value x = t->named.e[i].slot->v;
        bool ok = true;
        if (!strcmp(k, "anchor")) ok = whole(name, "anchor", x, 1, 9, &st->anchor), st->set |= GS_ANCHOR;
        else if (!strcmp(k, "angle")) ok = num(name, x, &st->angle), st->set |= GS_ANGLE;
        else if (!strcmp(k, "pivot")) {
            st->set |= GS_PIVOT;
            if (x.t == V_NUM) {
                ok = whole(name, "pivot (an anchor number)", x, 1, 9, &st->pivot_anchor);
            } else {
                GfxPt p;
                ok = point(name, x, &p);
                st->pivot_anchor = 0, st->pivot_x = p.x, st->pivot_y = p.y;
            }
        } else if (!strcmp(k, "outline")) ok = num(name, x, &st->outline), st->set |= GS_OUTLINE;
        else if (!strcmp(k, "alpha")) ok = whole(name, "alpha", x, 0, 255, &st->alpha), st->set |= GS_ALPHA;
        else if (!strcmp(k, "scale")) {
            ok = num(name, x, &st->scale_x);
            st->scale_y = st->scale_x;
            st->set |= GS_SCALE_X | GS_SCALE_Y;
        } else if (!strcmp(k, "scale_x")) ok = num(name, x, &st->scale_x), st->set |= GS_SCALE_X;
        else if (!strcmp(k, "scale_y")) ok = num(name, x, &st->scale_y), st->set |= GS_SCALE_Y;
        else if (!strcmp(k, "flip_x")) ok = flag(name, x, &st->flip_x), st->set |= GS_FLIP_X;
        else if (!strcmp(k, "flip_y")) ok = flag(name, x, &st->flip_y), st->set |= GS_FLIP_Y;
        else if (!strcmp(k, "tint")) ok = color(name, x, &st->tint), st->set |= GS_TINT;
        else if (!strcmp(k, "smooth")) ok = flag(name, x, &st->smooth), st->set |= GS_SMOOTH;
        else if (!strcmp(k, "radius")) ok = num(name, x, &st->radius), st->set |= GS_RADIUS;
        else if (!strcmp(k, "size")) ok = num(name, x, &st->size), st->set |= GS_SIZE;
        else if (!strcmp(k, "wrap")) ok = num(name, x, &st->wrap), st->set |= GS_WRAP;
        else if (!strcmp(k, "font")) {
            if (x.t != V_FONT) return fail("%s: font needs a FONT (from FONT()), not a %s", name, v_type(x));
            st->font = x.gfx.p;
            st->set |= GS_FONT;
        } else if (!strcmp(k, "align")) {
            if (x.t != V_TEXT || (strcmp(x.s->s, "left") && strcmp(x.s->s, "center") && strcmp(x.s->s, "right")))
                return fail("%s: align is \"left\", \"center\" or \"right\"", name);
            st->align = !strcmp(x.s->s, "left") ? 0 : !strcmp(x.s->s, "center") ? 1 : 2;
            st->set |= GS_ALIGN;
        } else {
            return fail("%s doesn't have an option called %s (its options are %s)", name, k, OPTIONS);
        }
        if (!ok) return false;
    }
    return true;
}

/// Takes an options table off the end of the inputs, when there is one.
static bool take_options(const char *name, Value *a, int *n, int required, GfxStyle *st) {
    memset(st, 0, sizeof *st);
    if (*n > required && a[*n - 1].t == V_TABLE) {
        (*n)--;
        return options(name, a[*n], st);
    }
    return true;
}

static void tset(Table *t, const char *k, Value v) {
    Str *key = str_c(k);
    om_insert(&t->named, key, slot_new(v));
    str_unref(key);
}

static Value v_double(double d) {
    Number n;
    if (d == floor(d) && fabs(d) < 9e15) return v_int((int64_t)d);
    if (!num_from_f64(d, &n)) return v_int(0);
    return v_num(n);
}

static Value v_image(GfxImage *img) {
    Value v = NULL_VALUE;
    v.t = V_IMAGE;
    v.gfx.p = img;
    return v;
}

static bool image_arg(const char *name, Value v, GfxImage **out) {
    if (v.t != V_IMAGE) return fail("%s needs an IMAGE (from IMAGE(\"file.png\") or IMAGE_NEW()), not a %s", name, v_type(v));
    *out = v.gfx.p;
    return true;
}

// ---------- windows ----------

static int pick_win(void) {
    if (sel_win >= 0 && sel_win < nwins && gfx_window_is_open(wins[sel_win].w)) return sel_win;
    for (int i = 0; i < nwins; i++) {
        if (gfx_window_is_open(wins[i].w)) return i;
    }
    if (sel_win >= 0 && sel_win < nwins) return sel_win;
    return nwins ? 0 : -1;
}

static bool window_arg(const char *name, Value *a, int n, int *out) {
    if (n && a[0].t != V_NULL) {
        if (a[0].t != V_WINDOW) return fail("%s needs a WINDOW (from WINDOW()), not a %s", name, v_type(a[0]));
        *out = a[0].gfx.id;
        return true;
    }
    *out = pick_win();
    if (*out < 0) return fail("there's no window yet: open one with WINDOW(width, height)");
    return true;
}

/// Where drawing goes now.
static bool target(GfxTarget *t) {
    if (draw_img && draw_img->v.t == V_IMAGE) {
        GfxImage *img = draw_img->v.gfx.p;
        // Images copy like tables: drawing into one that's shared gives this name its own.
        if (img->refs > 1) {
            GfxImage *c = gfx_image_copy(img);
            if (!c) return fail("out of memory for the image");
            gfx_image_unref(img);
            draw_img->v.gfx.p = c;
            img = c;
        }
        t->img = img;
        t->cam = NULL;
        return true;
    }
    int i = pick_win();
    if (i < 0) return fail("there's no window to draw in yet: open one with WINDOW(width, height)");
    t->img = gfx_window_frame(wins[i].w);
    t->cam = gfx_window_camera(wins[i].w);
    return true;
}

bool gfx_draw_to_slot(Interp *it, Slot *s) {
    (void)it;
    if (s->constant) return fail("DRAW_TO can't draw into a constant (CONST) image");
    if (draw_img) slot_unref(draw_img);
    draw_img = slot_ref(s);
    return true;
}

static bool window_set(const char *name, Win *w, Value v) {
    if (v.t != V_TABLE || v.tb.tab->n) return fail("%s's settings go in a table, like [resizable: FALSE]", name);
    const Table *t = v.tb.tab;
    for (size_t i = 0; i < t->named.n; i++) {
        const char *k = t->named.e[i].key->s;
        Value x = t->named.e[i].slot->v;
        bool b = false;
        if (!strcmp(k, "resizable")) {
            if (!flag(name, x, &b)) return false;
            gfx_window_set_resizable(w->w, b);
        } else if (!strcmp(k, "fullscreen_allowed")) {
            if (!flag(name, x, &b)) return false;
            gfx_window_set_fullscreen_allowed(w->w, b);
        } else if (!strcmp(k, "fullscreen")) {
            if (!flag(name, x, &b)) return false;
            gfx_window_set_fullscreen(w->w, b);
        } else if (!strcmp(k, "title")) {
            char *s = display(x);
            gfx_window_set_title(w->w, s);
            free(s);
        } else if (!strcmp(k, "cursor")) {
            static const char *names[] = {"arrow", "hand", "text", "crosshair", "hidden"};
            int c = -1;
            for (int j = 0; j < 5; j++) {
                if (x.t == V_TEXT && !strcmp(x.s->s, names[j])) c = j;
            }
            if (c < 0) return fail("%s: cursor is \"arrow\", \"hand\", \"text\", \"crosshair\" or \"hidden\"", name);
            gfx_window_set_cursor(w->w, c);
        } else if (!strcmp(k, "icon")) {
            GfxImage *img = NULL;
            char err[GFX_ERR];
            if (x.t == V_TEXT) {
                img = gfx_image_load(x.s->s, err);
                if (!img) return fail("%s", err);
                gfx_window_set_icon(w->w, img);
                gfx_image_unref(img);
            } else {
                if (!image_arg(name, x, &img)) return false;
                gfx_window_set_icon(w->w, img);
            }
        } else {
            return fail("%s doesn't have a setting called %s (its settings are resizable, fullscreen_allowed, fullscreen, "
                        "title, cursor and icon)", name, k);
        }
    }
    return true;
}

static void sleep_for(double s) {
    if (s <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)(s * 1000 + 0.5));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

// ---------- events ----------

static Handler *find_handler(Win *w, int kind, int key) {
    for (int i = 0; i < w->nh; i++) {
        if (w->h[i].kind == kind && w->h[i].key == key) return &w->h[i];
    }
    return NULL;
}

static bool set_handler(const char *name, int kind, int key, Value fn) {
    if (fn.t != V_FUNC && fn.t != V_NULL) return fail("%s needs a function (or NULL to stop), not a %s", name, v_type(fn));
    int i = pick_win();
    if (i < 0) return fail("there's no window yet: open one with WINDOW(width, height) before %s", name);
    Win *w = &wins[i];
    Handler *h = find_handler(w, kind, key);
    if (h) {
        v_release(h->fn);
        if (fn.t == V_NULL) {
            *h = w->h[--w->nh];
        } else {
            h->fn = v_retain(fn);
        }
    } else if (fn.t == V_FUNC) {
        if (w->nh == w->cap) {
            w->cap = w->cap ? w->cap * 2 : 8;
            w->h = xrealloc(w->h, (size_t)w->cap * sizeof(Handler));
        }
        w->h[w->nh++] = (Handler){kind, key, v_retain(fn)};
    }
    if (kind == H_CLOSE) gfx_window_ask_before_close(w->w, find_handler(w, H_CLOSE, 0) != NULL);
    return true;
}

/// Calls a handler with up to `n` inputs (fewer if the function takes fewer).
static bool call_handler(Interp *it, Value fn, Value *args, int n, Value *result) {
    Function *f = fn.fn;
    FuncDecl *d = f->decl;
    bool rest = d->nparams && d->params[d->nparams - 1].rest;
    int take = rest || n < d->nparams ? n : d->nparams;
    Slot *vals[4];
    for (int i = 0; i < take; i++) vals[i] = slot_new(v_retain(args[i]));
    int line = it->nframes ? it->frames[it->nframes - 1].line : 0;
    v_retain(fn);
    frame_push(it, f, f->env, line, f->src);
    Value out = NULL_VALUE;
    bool ok = call_prepared(it, f, vals, take, &out);
    frame_pop(it);
    v_release(fn);
    for (int i = 0; i < take; i++) slot_unref(vals[i]);
    if (result) *result = out;
    else v_release(out);
    return ok;
}

static const char *BUTTONS[3] = {"left", "right", "middle"};

static bool dispatch(Interp *it, int wi, const GfxEvent *e) {
    Win *w = &wins[wi];
    Value args[3] = {NULL_VALUE, NULL_VALUE, NULL_VALUE};
    int kind, n = 0;
    switch (e->kind) {
    case GE_KEY_DOWN:
    case GE_KEY_UP: {
        if (e->repeat) return true;
        const char *k = gfx_key_name(e->key);
        if (!k) return true;
        kind = e->kind == GE_KEY_DOWN ? H_KEY : H_KEY_UP;
        args[0] = v_textc(k);
        n = 1;
        bool ok = true;
        Handler *h = find_handler(w, kind, e->key);
        if (h) ok = call_handler(it, h->fn, args, n, NULL);
        if (ok && (h = find_handler(&wins[wi], kind, ANY_KEY))) ok = call_handler(it, h->fn, args, n, NULL);
        v_release(args[0]);
        return ok;
    }
    case GE_MOUSE_DOWN:
        kind = H_CLICK;
        args[0] = v_double(e->x), args[1] = v_double(e->y), args[2] = v_textc(BUTTONS[e->key >= 0 && e->key < 3 ? e->key : 0]);
        n = 3;
        break;
    case GE_MOUSE_MOVE:
        kind = H_MOVE;
        args[0] = v_double(e->x), args[1] = v_double(e->y);
        n = 2;
        break;
    case GE_TYPE:
        kind = H_TYPE;
        args[0] = v_textc(e->text);
        n = 1;
        break;
    case GE_RESIZE:
        kind = H_RESIZE;
        args[0] = v_int(e->w), args[1] = v_int(e->h);
        n = 2;
        break;
    case GE_CLOSE: {
        Handler *h = find_handler(w, H_CLOSE, 0);
        bool close = true;
        if (h) {
            Value r;
            if (!call_handler(it, h->fn, args, 0, &r)) return false;
            close = !(r.t == V_BOOL && !r.b);
            v_release(r);
        }
        if (close) {
            wins[wi].closed_by_program = true;
            gfx_window_close(wins[wi].w);
        }
        return true;
    }
    default:
        return true;
    }
    Handler *h = find_handler(w, kind, 0);
    bool ok = h ? call_handler(it, h->fn, args, n, NULL) : true;
    for (int i = 0; i < n; i++) v_release(args[i]);
    return ok;
}

/// A new input frame for every window, and their events to the handlers.
static bool pump(Interp *it) {
    for (int i = 0; i < nwins; i++) {
        const GfxEvent *evs;
        int n = gfx_window_poll(wins[i].w, &evs);
        if (!n) continue;
        // Handlers can open windows or SHOW again, so work from a copy.
        GfxEvent *copy = xmalloc((size_t)n * sizeof(GfxEvent));
        memcpy(copy, evs, (size_t)n * sizeof(GfxEvent));
        bool ok = true;
        for (int k = 0; k < n && ok; k++) ok = dispatch(it, i, &copy[k]);
        free(copy);
        if (!ok) return false;
    }
    return true;
}

static void show_all(void) {
    for (int i = 0; i < nwins; i++) {
        if (gfx_window_is_open(wins[i].w)) gfx_window_show(wins[i].w);
    }
}

static bool any_open(void) {
    for (int i = 0; i < nwins; i++) {
        if (gfx_window_is_open(wins[i].w)) return true;
    }
    return false;
}

bool gfx_finish(Interp *it) {
    if (!nwins) return true;
    if (gfx_headless()) {
        show_all();
        return true;
    }
    // The program's code has ended: its windows stay up, and their events still work.
    show_all();
    while (any_open()) {
        gfx_wait_events(0.25);
        if (!pump(it)) return false;
        show_all();
    }
    return true;
}

// ---------- values ----------

void gfx_value_retain(Value v) {
    if (v.t == V_IMAGE) gfx_image_ref(v.gfx.p);
    else if (v.t == V_FONT) gfx_font_ref(v.gfx.p);
}

void gfx_value_release(Value v) {
    if (v.t == V_IMAGE) gfx_image_unref(v.gfx.p);
    else if (v.t == V_FONT) gfx_font_unref(v.gfx.p);
}

void gfx_value_write(Buf *b, Value v) {
    if (v.t == V_IMAGE) {
        GfxImage *img = v.gfx.p;
        buf_printf(b, "IMAGE %dx%d", img->w, img->h);
    } else if (v.t == V_WINDOW) {
        buf_printf(b, "WINDOW %s", v.gfx.id < nwins ? gfx_window_title(wins[v.gfx.id].w) : "");
    } else {
        buf_puts(b, gfx_font_is_pixel(v.gfx.p) ? "FONT pixel" : "FONT");
    }
}

bool gfx_field(Value base, const Str *name, Value *out) {
    int w = 0, h = 0;
    if (base.t == V_IMAGE) {
        GfxImage *img = base.gfx.p;
        w = img->w, h = img->h;
    } else if (base.t == V_WINDOW && base.gfx.id < nwins) {
        GfxImage *f = gfx_window_frame(wins[base.gfx.id].w);
        w = f->w, h = f->h;
        if (str_is(name, "title")) {
            *out = v_textc(gfx_window_title(wins[base.gfx.id].w));
            return true;
        }
    } else {
        return fail("a FONT has no .%s", name->s);
    }
    if (str_is(name, "width")) *out = v_int(w);
    else if (str_is(name, "height")) *out = v_int(h);
    else return fail("a%s %s has .width and .height%s, not .%s", base.t == V_IMAGE ? "n" : "", v_type(base),
                     base.t == V_WINDOW ? " and .title" : "", name->s);
    return true;
}

static int key_arg(const char *name, Value v, bool *ok) {
    *ok = false;
    if (v.t != V_TEXT) {
        fail("%s needs a key's name, like \"a\", \"space\" or \"left\", not a %s", name, v_type(v));
        return -1;
    }
    if (!strcmp(v.s->s, "any")) {
        *ok = true;
        return ANY_KEY;
    }
    int k = gfx_key_code(v.s->s, v.s->len);
    if (k < 0) {
        fail("%s: there's no key called \"%s\" (keys are letters and numbers like \"a\" and \"1\", \"space\", \"enter\", "
             "\"escape\", \"tab\", \"backspace\", \"delete\", \"left\", \"right\", \"up\", \"down\", \"shift\", \"ctrl\", "
             "\"alt\", \"f1\" to \"f12\", \"home\", \"end\", \"pageup\", \"pagedown\", punctuation like \"-\", or \"any\")",
             name, v.s->s);
        return -1;
    }
    *ok = true;
    return k;
}

static int button_arg(const char *name, Value *a, int n, bool *ok) {
    *ok = true;
    if (!n || a[0].t == V_NULL) return GFX_MOUSE_LEFT;
    for (int i = 0; i < 3; i++) {
        if (a[0].t == V_TEXT && !strcmp(a[0].s->s, BUTTONS[i])) return i;
    }
    *ok = fail("%s: the mouse buttons are \"left\", \"right\" and \"middle\"", name);
    return 0;
}

static const GfxInput *input_now(void) {
    int i = pick_win();
    return i < 0 ? NULL : gfx_window_input(wins[i].w);
}

static Value style_table(void) {
    const GfxStyle *s = gfx_style_current();
    Table *t = table_new();
    if (s->set & GS_ANCHOR) tset(t, "anchor", v_int(s->anchor));
    if (s->set & GS_ANGLE) tset(t, "angle", v_double(s->angle));
    if (s->set & GS_PIVOT) {
        if (s->pivot_anchor) {
            tset(t, "pivot", v_int(s->pivot_anchor));
        } else {
            Table *p = table_new();
            table_push(p, v_double(s->pivot_x));
            table_push(p, v_double(s->pivot_y));
            tset(t, "pivot", v_table(p));
        }
    }
    if (s->set & GS_OUTLINE) tset(t, "outline", v_double(s->outline));
    if (s->set & GS_ALPHA) tset(t, "alpha", v_int(s->alpha));
    if (s->set & GS_SCALE_X) tset(t, "scale_x", v_double(s->scale_x));
    if (s->set & GS_SCALE_Y) tset(t, "scale_y", v_double(s->scale_y));
    if (s->set & GS_FLIP_X) tset(t, "flip_x", v_bool(s->flip_x));
    if (s->set & GS_FLIP_Y) tset(t, "flip_y", v_bool(s->flip_y));
    if (s->set & GS_TINT) tset(t, "tint", v_double(gfx_color_to_number(s->tint)));
    if (s->set & GS_SMOOTH) tset(t, "smooth", v_bool(s->smooth));
    if (s->set & GS_RADIUS) tset(t, "radius", v_double(s->radius));
    if ((s->set & GS_FONT) && s->font) {
        Value f = NULL_VALUE;
        f.t = V_FONT;
        f.gfx.p = s->font;
        tset(t, "font", v_retain(f));
    }
    if (s->set & GS_SIZE) tset(t, "size", v_double(s->size));
    if (s->set & GS_ALIGN) tset(t, "align", v_textc(s->align == 0 ? "left" : s->align == 1 ? "center" : "right"));
    if (s->set & GS_WRAP) tset(t, "wrap", v_double(s->wrap));
    return v_table(t);
}

// ---------- the commands ----------

/// Shapes: the numbers they take, then color, then an optional outline/width, then options.
static bool shape(const char *name, GfxKind kind, int nnums, bool has_extra, Value *a, int n) {
    GfxStyle st;
    if (!take_options(name, a, &n, nnums + 1, &st)) return false;
    if (!argc(name, n, nnums + 1, nnums + 1 + (has_extra ? 1 : 0))) return false;
    double v[8] = {0};
    for (int i = 0; i < nnums; i++) {
        if (!num(name, a[i], &v[i])) return false;
    }
    uint32_t c;
    if (!color(name, a[nnums], &c)) return false;
    if (n > nnums + 1 && a[nnums + 1].t != V_NULL) {
        double extra;
        if (!num(name, a[nnums + 1], &extra)) return false;
        bool is_width = kind == GK_LINE || kind == GK_ARROW || kind == GK_ARC;
        if (is_width) {
            v[nnums] = extra;
        } else {
            st.outline = extra;
            st.set |= GS_OUTLINE;
        }
    }
    GfxTarget t;
    if (!target(&t)) return false;
    char err[GFX_ERR] = "";
    if (!gfx_shape(&t, kind, v, nnums + 1, NULL, 0, c, &st, err)) return fail("%s: %s", name, err);
    return true;
}

/// Shapes made from a table of points.
static bool point_shape(const char *name, GfxKind kind, int min, Value *a, int n) {
    GfxStyle st;
    if (!take_options(name, a, &n, 2, &st)) return false;
    if (!argc(name, n, 2, 3)) return false;
    GfxPt *p = NULL;
    int np = 0;
    if (!points(name, a[0], min, &p, &np)) return false;
    uint32_t c;
    double extra = 0;
    bool ok = color(name, a[1], &c);
    if (ok && n > 2 && a[2].t != V_NULL) ok = num(name, a[2], &extra);
    if (ok && kind == GK_POLYGON) {
        st.outline = extra;
        if (n > 2) st.set |= GS_OUTLINE;
    }
    GfxTarget t;
    if (ok) ok = target(&t);
    char err[GFX_ERR] = "";
    if (ok && !gfx_shape(&t, kind, &extra, 1, p, np, c, &st, err)) ok = fail("%s: %s", name, err);
    free(p);
    return ok;
}

bool gfx_builtin(Interp *it, int bi, Value *a, int n, Value *out) {
    const char *name = WORDS[bi];
    int k = bi - GFX_FIRST;
    *out = NULL_VALUE;
    char err[GFX_ERR] = "";
    GfxStyle st;
    switch (k) {
    case B_WINDOW: {
        Value *set = NULL;
        if (n > 2 && a[n - 1].t == V_TABLE) set = &a[--n];
        if (!argc(name, n, 2, 4)) return false;
        int w, h;
        double scale = 1;
        if (!whole(name, "the width", a[0], 1, 16384, &w) || !whole(name, "the height", a[1], 1, 16384, &h)) return false;
        char *title = NULL;
        if (n > 2 && a[2].t != V_NULL) title = display(a[2]);
        else if (it->nframes && it->frames[0].src && it->frames[0].src->name) title = xstrdup(it->frames[0].src->name);
        if (n > 3 && a[3].t != V_NULL && !num(name, a[3], &scale)) {
            free(title);
            return false;
        }
        GfxWindow *gw = gfx_window_open(w, h, scale, title, err);
        free(title);
        if (!gw) return fail("%s", err);
        wins = xrealloc(wins, (size_t)(nwins + 1) * sizeof(Win));
        memset(&wins[nwins], 0, sizeof(Win));
        wins[nwins].w = gw;
        out->t = V_WINDOW;
        out->gfx.id = nwins;
        out->gfx.p = NULL;
        nwins++;
        if (set) return window_set(name, &wins[nwins - 1], *set);
        return true;
    }
    case B_DRAW_TO:
        if (!argc(name, n, 0, 1)) return false;
        if (draw_img) {
            slot_unref(draw_img);
            draw_img = NULL;
        }
        if (!n || a[0].t == V_NULL) {
            sel_win = -1;
            return true;
        }
        if (a[0].t == V_IMAGE) return fail("DRAW_TO needs the name of an image's variable, like DRAW_TO(background), so the drawing has somewhere to stay");
        if (a[0].t != V_WINDOW) return fail("DRAW_TO needs a WINDOW or the name of an IMAGE's variable, not a %s", v_type(a[0]));
        sel_win = a[0].gfx.id;
        return true;
    case B_SHOW: {
        if (!argc(name, n, 0, 0)) return false;
        show_all();
        gfx_headless_frame_done();
        out_flush();
        double d = gfx_frame_delay(fps);
        if (d > 0) {
            if (it->ntasks) {
                if (!task_sleep(it, d)) return false;
            } else {
                sleep_for(d);
            }
        }
        if (!pump(it)) return false;
        // Closing the last window ends a program that never asks WINDOW_OPEN() (one that
        // does handles closing itself).
        if (!asked_open && nwins && !any_open()) {
            bool by_user = false;
            for (int i = 0; i < nwins; i++) by_user |= !wins[i].closed_by_program;
            if (by_user) {
                g_fault = FAULT_END;
                return false;
            }
        }
        return true;
    }
    case B_FPS:
        if (!argc(name, n, 0, 1)) return false;
        if (n && a[0].t != V_NULL) {
            double f;
            if (!num(name, a[0], &f)) return false;
            if (f < 0 || f > 1000) return fail("FPS goes from 0 (as fast as possible) to 1000");
            fps = f;
        } else {
            *out = v_double(fps);
        }
        return true;
    case B_WINDOW_OPEN: {
        if (!argc(name, n, 0, 1)) return false;
        asked_open = true;
        if (!n && !nwins) {
            *out = v_bool(false);
            return true;
        }
        int i;
        if (!window_arg(name, a, n, &i)) return false;
        *out = v_bool(gfx_window_is_open(wins[i].w));
        return true;
    }
    case B_WINDOW_CLOSE: {
        if (!argc(name, n, 0, 1)) return false;
        int i;
        if (!window_arg(name, a, n, &i)) return false;
        wins[i].closed_by_program = true;
        gfx_window_close(wins[i].w);
        return true;
    }
    case B_WINDOW_TITLE: {
        if (!argc(name, n, 0, 1)) return false;
        int i = pick_win();
        if (i < 0) return fail("there's no window yet: open one with WINDOW(width, height)");
        if (!n) {
            *out = v_textc(gfx_window_title(wins[i].w));
            return true;
        }
        char *s = display(a[0]);
        gfx_window_set_title(wins[i].w, s);
        free(s);
        return true;
    }
    case B_WINDOW_RESIZE: {
        if (!argc(name, n, 2, 2)) return false;
        int i = pick_win(), w, h;
        if (i < 0) return fail("there's no window yet: open one with WINDOW(width, height)");
        if (!whole(name, "the width", a[0], 1, 16384, &w) || !whole(name, "the height", a[1], 1, 16384, &h)) return false;
        if (!gfx_window_resize(wins[i].w, w, h, err)) return fail("%s", err);
        return true;
    }
    case B_WINDOW_SET: {
        if (!argc(name, n, 1, 1)) return false;
        int i = pick_win();
        if (i < 0) return fail("there's no window yet: open one with WINDOW(width, height)");
        return window_set(name, &wins[i], a[0]);
    }
    case B_STYLE:
        if (!argc(name, n, 0, 1)) return false;
        if (!n) {
            *out = style_table();
            return true;
        }
        if (!options(name, a[0], &st)) return false;
        gfx_style_set(&st);
        return true;
    case B_RESET_STYLE:
        if (!argc(name, n, 0, 0)) return false;
        memset(&st, 0, sizeof st);
        gfx_style_set(&st);
        return true;
    case B_RGB: {
        if (!argc(name, n, 3, 4)) return false;
        int c[4] = {0, 0, 0, 255};
        static const char *what[4] = {"red", "green", "blue", "alpha"};
        for (int i = 0; i < n; i++) {
            if (!whole(name, what[i], a[i], 0, 255, &c[i])) return false;
        }
        *out = v_double(gfx_color_to_number((uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2]));
        return true;
    }
    case B_CLEAR: {
        if (!argc(name, n, 0, 1)) return false;
        uint32_t c = 0xFF000000u;
        if (n && !color(name, a[0], &c)) return false;
        GfxTarget t;
        if (!target(&t)) return false;
        gfx_clear(t.img, c, draw_img == NULL);
        return true;
    }
    case B_PIXEL: return shape(name, GK_PIXEL, 2, false, a, n);
    case B_GET_PIXEL: {
        if (!argc(name, n, 2, 2)) return false;
        double x, y;
        if (!num(name, a[0], &x) || !num(name, a[1], &y)) return false;
        GfxTarget t;
        if (!target(&t)) return false;
        uint32_t c;
        if (gfx_get_pixel(&t, x, y, &c)) *out = v_double(gfx_color_to_number(c));
        return true;
    }
    case B_LINE: return shape(name, GK_LINE, 4, true, a, n);
    case B_ARROW: return shape(name, GK_ARROW, 4, true, a, n);
    case B_LINES: return point_shape(name, GK_LINES, 1, a, n);
    case B_CURVE: return point_shape(name, GK_CURVE, 2, a, n);
    case B_POLYGON: return point_shape(name, GK_POLYGON, 3, a, n);
    case B_RECT: return shape(name, GK_RECT, 4, true, a, n);
    case B_CIRCLE: return shape(name, GK_CIRCLE, 3, true, a, n);
    case B_ELLIPSE: return shape(name, GK_ELLIPSE, 4, true, a, n);
    case B_NGON: return shape(name, GK_NGON, 4, true, a, n);
    case B_STAR: return shape(name, GK_STAR, 5, true, a, n);
    case B_ARC: return shape(name, GK_ARC, 5, true, a, n);
    case B_PIE: return shape(name, GK_PIE, 5, true, a, n);
    case B_TRIANGLE: {
        if (!take_options(name, a, &n, 7, &st)) return false;
        if (!argc(name, n, 7, 8)) return false;
        GfxPt p[3];
        for (int i = 0; i < 3; i++) {
            if (!num(name, a[i * 2], &p[i].x) || !num(name, a[i * 2 + 1], &p[i].y)) return false;
        }
        uint32_t c;
        if (!color(name, a[6], &c)) return false;
        if (n > 7 && a[7].t != V_NULL) {
            if (!num(name, a[7], &st.outline)) return false;
            st.set |= GS_OUTLINE;
        }
        GfxTarget t;
        if (!target(&t)) return false;
        double none = 0;
        if (!gfx_shape(&t, GK_TRIANGLE, &none, 1, p, 3, c, &st, err)) return fail("%s: %s", name, err);
        return true;
    }
    case B_FLOOD: {
        if (!argc(name, n, 3, 3)) return false;
        double x, y;
        uint32_t c;
        if (!num(name, a[0], &x) || !num(name, a[1], &y) || !color(name, a[2], &c)) return false;
        GfxTarget t;
        if (!target(&t)) return false;
        gfx_flood(&t, x, y, c);
        return true;
    }
    case B_TEXT: {
        if (!take_options(name, a, &n, 4, &st)) return false;
        if (!argc(name, n, 4, 5)) return false;
        double x, y;
        uint32_t c;
        if (!num(name, a[0], &x) || !num(name, a[1], &y) || !color(name, a[3], &c)) return false;
        if (n > 4 && a[4].t != V_NULL) {
            if (!num(name, a[4], &st.size)) return false;
            st.set |= GS_SIZE;
        }
        GfxTarget t;
        if (!target(&t)) return false;
        char *s = display(a[2]);
        gfx_text(&t, x, y, s, strlen(s), c, &st, err);
        free(s);
        return true;
    }
    case B_TEXT_SIZE: {
        if (!take_options(name, a, &n, 1, &st)) return false;
        if (!argc(name, n, 1, 2)) return false;
        if (n > 1 && a[1].t != V_NULL) {
            if (!num(name, a[1], &st.size)) return false;
            st.set |= GS_SIZE;
        }
        char *s = display(a[0]);
        double w, h;
        gfx_text_size(s, strlen(s), &st, &w, &h);
        free(s);
        Table *t = table_new();
        tset(t, "width", v_double(w));
        tset(t, "height", v_double(h));
        *out = v_table(t);
        return true;
    }
    case B_FONT: {
        if (!argc(name, n, 0, 1)) return false;
        if (n && a[0].t != V_TEXT && a[0].t != V_NULL) return fail("FONT needs a font file's name (like \"Roboto.ttf\"), or \"pixel\"");
        GfxFont *f = gfx_font_load(n && a[0].t == V_TEXT ? a[0].s->s : NULL, err);
        if (!f) return fail("%s", err);
        out->t = V_FONT;
        out->gfx.p = f;
        return true;
    }
    case B_IMAGE: {
        if (!argc(name, n, 1, 1)) return false;
        if (a[0].t != V_TEXT) return fail("IMAGE needs an image file's name, like \"hero.png\"");
        GfxImage *img = gfx_image_load(a[0].s->s, err);
        if (!img) return fail("%s", err);
        *out = v_image(img);
        return true;
    }
    case B_IMAGE_NEW: {
        if (!argc(name, n, 2, 3)) return false;
        int w, h;
        uint32_t c = 0;
        if (!whole(name, "the width", a[0], 1, 16384, &w) || !whole(name, "the height", a[1], 1, 16384, &h)) return false;
        if (n > 2 && !color(name, a[2], &c)) return false;
        GfxImage *img = gfx_image_new(w, h, c);
        if (!img) return fail("a %d x %d image is too big", w, h);
        *out = v_image(img);
        return true;
    }
    case B_PART: {
        if (!argc(name, n, 5, 5)) return false;
        GfxImage *src = NULL;
        int v[4];
        if (!image_arg(name, a[0], &src)) return false;
        for (int i = 0; i < 4; i++) {
            if (!whole(name, i < 2 ? "the position" : "the size", a[i + 1], i < 2 ? -1000000 : 1, 1000000, &v[i])) return false;
        }
        GfxImage *img = gfx_image_part(src, v[0], v[1], v[2], v[3], err);
        if (!img) return fail("PART: %s", err);
        *out = v_image(img);
        return true;
    }
    case B_DRAW: {
        if (!take_options(name, a, &n, 3, &st)) return false;
        if (!argc(name, n, 3, 3)) return false;
        GfxImage *img = NULL;
        double x, y;
        if (!image_arg(name, a[0], &img) || !num(name, a[1], &x) || !num(name, a[2], &y)) return false;
        GfxTarget t;
        if (!target(&t)) return false;
        gfx_draw_image(&t, img, x, y, &st, err);
        return true;
    }
    case B_SAVE_IMAGE: {
        if (!argc(name, n, 2, 2)) return false;
        GfxImage *img = NULL;
        if (!image_arg(name, a[0], &img)) return false;
        if (a[1].t != V_TEXT) return fail("SAVE_IMAGE needs a file name, like \"picture.png\"");
        if (!gfx_image_save(img, a[1].s->s, err)) return fail("%s", err);
        return true;
    }
    case B_SCREENSHOT: {
        if (!argc(name, n, 0, 0)) return false;
        GfxTarget t;
        if (!target(&t)) return false;
        GfxImage *img = gfx_image_copy(t.img);
        if (!img) return fail("out of memory for the screenshot");
        *out = v_image(img);
        return true;
    }
    case B_CAMERA: {
        if (!argc(name, n, 0, 4)) return false;
        int i = pick_win();
        if (i < 0) return fail("there's no window yet: open one with WINDOW(width, height)");
        GfxCamera *cam = gfx_window_camera(wins[i].w);
        if (!n) {
            cam->on = false;
            return true;
        }
        if (n == 1) return fail("CAMERA needs both x and y (or nothing, to go back to the window's own positions)");
        double x, y, zoom = 1, angle = 0;
        if (!num(name, a[0], &x) || !num(name, a[1], &y)) return false;
        if (n > 2 && a[2].t != V_NULL && !num(name, a[2], &zoom)) return false;
        if (n > 3 && a[3].t != V_NULL && !num(name, a[3], &angle)) return false;
        if (zoom <= 0) return fail("CAMERA's zoom must be more than 0");
        cam->on = true, cam->x = x, cam->y = y, cam->zoom = zoom, cam->angle = angle;
        return true;
    }
    case B_KEY_DOWN:
    case B_KEY_PRESSED:
    case B_KEY_RELEASED: {
        if (!argc(name, n, 1, 1)) return false;
        bool ok;
        int key = key_arg(name, a[0], &ok);
        if (!ok) return false;
        const GfxInput *in = input_now();
        bool r = false;
        if (in) {
            const uint8_t *arr = k == B_KEY_DOWN ? in->down : k == B_KEY_PRESSED ? in->pressed : in->released;
            if (key == ANY_KEY) {
                for (int j = 0; j < GFX_KEYS && !r; j++) r = arr[j];
            } else {
                r = arr[key];
            }
        }
        *out = v_bool(r);
        return true;
    }
    case B_KEYS_DOWN: {
        if (!argc(name, n, 0, 0)) return false;
        Table *t = table_new();
        const GfxInput *in = input_now();
        for (int j = 0; in && j < GFX_KEYS; j++) {
            const char *kn = in->down[j] ? gfx_key_name(j) : NULL;
            if (kn) table_push(t, v_textc(kn));
        }
        *out = v_table(t);
        return true;
    }
    case B_TYPED: {
        if (!argc(name, n, 0, 0)) return false;
        const GfxInput *in = input_now();
        *out = v_textc(in ? in->typed : "");
        return true;
    }
    case B_MOUSE: {
        if (!argc(name, n, 0, 0)) return false;
        int i = pick_win();
        Table *t = table_new();
        const GfxInput *in = i < 0 ? NULL : gfx_window_input(wins[i].w);
        double x = in ? in->mx : 0, y = in ? in->my : 0, wx = x, wy = y;
        if (in) {
            GfxImage *f = gfx_window_frame(wins[i].w);
            gfx_screen_to_world(gfx_window_camera(wins[i].w), f->w, f->h, x, y, &wx, &wy);
        }
        tset(t, "x", v_double(x));
        tset(t, "y", v_double(y));
        tset(t, "world_x", v_double(wx));
        tset(t, "world_y", v_double(wy));
        tset(t, "left", v_bool(in && in->mdown[0]));
        tset(t, "right", v_bool(in && in->mdown[1]));
        tset(t, "middle", v_bool(in && in->mdown[2]));
        tset(t, "wheel", v_double(in ? in->wheel : 0));
        tset(t, "inside", v_bool(in && in->inside));
        *out = v_table(t);
        return true;
    }
    case B_MOUSE_PRESSED:
    case B_MOUSE_RELEASED: {
        if (!argc(name, n, 0, 1)) return false;
        bool ok;
        int b = button_arg(name, a, n, &ok);
        if (!ok) return false;
        const GfxInput *in = input_now();
        *out = v_bool(in && (k == B_MOUSE_PRESSED ? in->mpressed[b] : in->mreleased[b]));
        return true;
    }
    case B_ON_KEY:
    case B_ON_KEY_UP: {
        if (!argc(name, n, 2, 2)) return false;
        bool ok;
        int key = key_arg(name, a[0], &ok);
        if (!ok) return false;
        return set_handler(name, k == B_ON_KEY ? H_KEY : H_KEY_UP, key, a[1]);
    }
    case B_ON_CLICK:
    case B_ON_MOUSE_MOVE:
    case B_ON_TYPE:
    case B_ON_RESIZE:
    case B_ON_CLOSE: {
        if (!argc(name, n, 1, 1)) return false;
        int kind = k == B_ON_CLICK ? H_CLICK : k == B_ON_MOUSE_MOVE ? H_MOVE : k == B_ON_TYPE ? H_TYPE : k == B_ON_RESIZE ? H_RESIZE : H_CLOSE;
        return set_handler(name, kind, 0, a[0]);
    }
    }
    return fail("%s isn't available", name);
}
