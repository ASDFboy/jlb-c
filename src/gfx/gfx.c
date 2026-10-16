// The .jlb graphics core: colors, styles, shapes, images and text (see gfx.h).
//
// Every shape becomes polygons in .jlb positions, then one transform (rotation, camera
// and the flip to rows-from-the-top) maps them onto the picture, where a scanline
// filler colors the pixels. Crisp drawing samples each pixel's center; smooth drawing
// samples 16 points per pixel. Images and text are drawn by mapping each pixel back to
// the picture being drawn.

#include "gfx.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_NO_STDIO_HDR
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "gfx_font8x8.h"
#include "gfx_font_default.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void seterr(char *err, const char *fmt, const char *a) {
    if (err) snprintf(err, GFX_ERR, fmt, a ? a : "");
}

static void *xalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(70);
    }
    return p;
}

// ---------- images ----------

GfxImage *gfx_image_new(int w, int h, uint32_t fill) {
    if (w < 1 || h < 1 || (int64_t)w * h > 100000000) return NULL;
    GfxImage *img = xalloc(sizeof *img);
    img->refs = 1;
    img->w = w;
    img->h = h;
    img->px = malloc((size_t)w * h * 4);
    if (!img->px) {
        free(img);
        return NULL;
    }
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) img->px[i] = fill;
    return img;
}

GfxImage *gfx_image_copy(const GfxImage *src) {
    GfxImage *img = gfx_image_new(src->w, src->h, 0);
    if (img) memcpy(img->px, src->px, (size_t)src->w * src->h * 4);
    return img;
}

GfxImage *gfx_image_load(const char *path, char *err) {
    int w, h, n;
    unsigned char *d = stbi_load(path, &w, &h, &n, 4);
    if (!d) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            seterr(err, "can't open the image %s (no such file)", path);
        } else {
            fclose(f);
            if (err) snprintf(err, GFX_ERR, "can't read the image %s (%s; PNG, BMP, JPG, GIF and TGA work)", path, stbi_failure_reason());
        }
        return NULL;
    }
    GfxImage *img = gfx_image_new(w, h, 0);
    if (!img) {
        stbi_image_free(d);
        seterr(err, "the image %s is too big", path);
        return NULL;
    }
    for (size_t i = 0, k = (size_t)w * h; i < k; i++) {
        const unsigned char *p = d + i * 4;
        img->px[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    stbi_image_free(d);
    return img;
}

GfxImage *gfx_image_part(const GfxImage *src, int x, int y, int w, int h, char *err) {
    if (w < 1 || h < 1 || x < 1 || y < 1 || x + w - 1 > src->w || y + h - 1 > src->h) {
        if (err) snprintf(err, GFX_ERR, "that part isn't inside the %dx%d image (positions go from 1, 1 at the bottom-left)", src->w, src->h);
        return NULL;
    }
    GfxImage *img = gfx_image_new(w, h, 0);
    // y counts up from the bottom; rows are stored from the top.
    int top = src->h - (y + h - 1);
    for (int r = 0; r < h; r++) memcpy(img->px + (size_t)r * w, src->px + (size_t)(top + r) * src->w + (x - 1), (size_t)w * 4);
    return img;
}

bool gfx_image_save(const GfxImage *img, const char *path, char *err) {
    size_t n = (size_t)img->w * img->h;
    unsigned char *d = malloc(n * 4);
    if (!d) return false;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = img->px[i];
        d[i * 4] = (unsigned char)(c >> 16);
        d[i * 4 + 1] = (unsigned char)(c >> 8);
        d[i * 4 + 2] = (unsigned char)c;
        d[i * 4 + 3] = (unsigned char)(c >> 24);
    }
    int ok = stbi_write_png(path, img->w, img->h, 4, d, img->w * 4);
    free(d);
    if (!ok) seterr(err, "couldn't save the image as %s", path);
    return ok != 0;
}

void gfx_image_ref(GfxImage *img) {
    if (img) img->refs++;
}

void gfx_image_unref(GfxImage *img) {
    if (img && --img->refs == 0) {
        free(img->px);
        free(img);
    }
}

// ---------- colors ----------

bool gfx_color_from_number(double v, uint32_t *out) {
    if (!(v >= 0 && v <= 4294967295.0) || v != floor(v)) return false;
    uint32_t n = (uint32_t)v;
    *out = (uint32_t)(255 - (n >> 24)) << 24 | (n & 0xFFFFFF);
    return true;
}

double gfx_color_to_number(uint32_t argb) {
    return (double)((uint32_t)(255 - (argb >> 24)) << 24 | (argb & 0xFFFFFF));
}

static const struct {
    const char *name;
    uint32_t rgb;
} COLORS[] = {
    {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x00C000}, {"blue", 0x0000FF},
    {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"orange", 0xFFA500}, {"purple", 0x800080},
    {"pink", 0xFFC0CB}, {"brown", 0x8B4513}, {"gray", 0x808080}, {"grey", 0x808080}, {"lightgray", 0xD3D3D3},
    {"lightgrey", 0xD3D3D3}, {"darkgray", 0x404040}, {"darkgrey", 0x404040}, {"silver", 0xC0C0C0},
    {"gold", 0xFFD700}, {"navy", 0x000080}, {"teal", 0x008080}, {"lime", 0x00FF00}, {"maroon", 0x800000},
    {"olive", 0x808000}, {"darkgreen", 0x006400}, {"skyblue", 0x87CEEB}, {"lightblue", 0xADD8E6},
    {"darkblue", 0x00008B}, {"darkred", 0x8B0000}, {"violet", 0xEE82EE}, {"indigo", 0x4B0082},
    {"beige", 0xF5F5DC}, {"tan", 0xD2B48C}, {"coral", 0xFF7F50}, {"salmon", 0xFA8072}, {"crimson", 0xDC143C},
    {"turquoise", 0x40E0D0}, {"mint", 0x98FF98}, {"lavender", 0xE6E6FA},
};

bool gfx_color_from_name(const char *s, size_t n, uint32_t *out) {
    char low[24];
    if (n == 0 || n >= sizeof low) return false;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)s[i]);
    low[n] = 0;
    if (strcmp(low, "transparent") == 0) {
        *out = 0;
        return true;
    }
    if (low[0] == '#' && (n == 7 || n == 9)) {
        uint32_t v = 0;
        for (size_t i = 1; i < n; i++) {
            int c = low[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = v << 4 | (uint32_t)d;
        }
        *out = n == 7 ? 0xFF000000u | v : (v << 24) | (v >> 8); // #rrggbbaa
        return true;
    }
    for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++) {
        if (strcmp(COLORS[i].name, low) == 0) {
            *out = 0xFF000000u | COLORS[i].rgb;
            return true;
        }
    }
    return false;
}

// ---------- blending ----------

/// `src`'s color over `dst`, `a` (0..255) solid.
static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a) {
    if (a == 0) return dst;
    unsigned da = dst >> 24;
    if (a >= 255 || da == 0) return (uint32_t)(a > 255 ? 255 : a) << 24 | (src & 0xFFFFFF);
    unsigned sr = src >> 16 & 255, sg = src >> 8 & 255, sb = src & 255;
    unsigned dr = dst >> 16 & 255, dg = dst >> 8 & 255, db = dst & 255;
    if (da == 255) {
        unsigned ia = 255 - a;
        return 0xFF000000u | ((sr * a + dr * ia + 127) / 255) << 16 | ((sg * a + dg * ia + 127) / 255) << 8 | ((sb * a + db * ia + 127) / 255);
    }
    // Both see-through: the general "over".
    unsigned keep = da * (255 - a);               // 0..65025
    unsigned oa = a * 255 + keep;                  // 255 * result alpha
    unsigned r = (sr * a * 255 + dr * keep + oa / 2) / oa;
    unsigned g = (sg * a * 255 + dg * keep + oa / 2) / oa;
    unsigned b = (sb * a * 255 + db * keep + oa / 2) / oa;
    return (uint32_t)((oa + 127) / 255) << 24 | r << 16 | g << 8 | b;
}

static inline unsigned mul255(unsigned a, unsigned b) {
    return (a * b + 127) / 255;
}

// ---------- style ----------

static const GfxStyle DEFAULTS = {
    .set = 0, .anchor = 5, .angle = 0, .pivot_anchor = 0, .outline = 0, .alpha = 255, .scale_x = 1, .scale_y = 1,
    .tint = 0xFFFFFFFF, .smooth = false, .radius = 0, .font = NULL, .size = 0, .align = 0, .wrap = 0,
};
static GfxStyle current;
static bool current_ready;

const GfxStyle *gfx_style_current(void) {
    if (!current_ready) {
        current = DEFAULTS;
        current_ready = true;
    }
    return &current;
}

void gfx_style_set(const GfxStyle *s) {
    gfx_style_current();
    GfxFont *old = current.font;
    current = *s;
    if (!(current.set & GS_FONT)) current.font = NULL;
    gfx_font_ref(current.font);
    gfx_font_unref(old);
}

/// Defaults, then the current style, then the call's own options.
static GfxStyle resolve(const GfxStyle *call) {
    const GfxStyle *layers[2] = {gfx_style_current(), call};
    GfxStyle r = DEFAULTS;
    for (int i = 0; i < 2; i++) {
        const GfxStyle *s = layers[i];
        if (!s) continue;
        unsigned m = s->set;
        if (m & GS_ANCHOR) r.anchor = s->anchor;
        if (m & GS_ANGLE) r.angle = s->angle;
        if (m & GS_PIVOT) r.pivot_anchor = s->pivot_anchor, r.pivot_x = s->pivot_x, r.pivot_y = s->pivot_y;
        if (m & GS_OUTLINE) r.outline = s->outline;
        if (m & GS_ALPHA) r.alpha = s->alpha;
        if (m & GS_SCALE_X) r.scale_x = s->scale_x;
        if (m & GS_SCALE_Y) r.scale_y = s->scale_y;
        if (m & GS_TINT) r.tint = s->tint;
        if (m & GS_SMOOTH) r.smooth = s->smooth;
        if (m & GS_RADIUS) r.radius = s->radius;
        if (m & GS_FONT) r.font = s->font;
        if (m & GS_SIZE) r.size = s->size;
        if (m & GS_ALIGN) r.align = s->align;
        if (m & GS_WRAP) r.wrap = s->wrap;
        if (m & GS_FLIP_X) r.flip_x = s->flip_x;
        if (m & GS_FLIP_Y) r.flip_y = s->flip_y;
        r.set |= m;
    }
    return r;
}

// ---------- placing things: anchors, pivots, the transform ----------

/// Where an anchor (1..9) sits on a box: fractions of its width and height from the bottom-left.
static void anchor_frac(int a, double *fx, double *fy) {
    if (a < 1 || a > 9) a = 5;
    int col = (a - 1) % 3, row = (a - 1) / 3; // row 0 is the top
    *fx = col * 0.5;
    *fy = 1 - row * 0.5;
}

/// A 2D affine map: X = a*x + b*y + c, Y = d*x + e*y + f.
typedef struct {
    double a, b, c, d, e, f;
} Affine;

static Affine aff_mul(Affine m, Affine n) { // m after n
    Affine r = {m.a * n.a + m.b * n.d, m.a * n.b + m.b * n.e, m.a * n.c + m.b * n.f + m.c,
                m.d * n.a + m.e * n.d, m.d * n.b + m.e * n.e, m.d * n.c + m.e * n.f + m.f};
    return r;
}

static Affine aff_rotate_about(double deg, double px, double py) {
    double t = deg * M_PI / 180, c = cos(t), s = sin(t);
    // Snap the common angles, so a quarter turn is exact.
    if (fmod(deg, 90) == 0) {
        int q = ((int)fmod(deg / 90, 4) + 4) % 4;
        c = q == 0 ? 1 : q == 2 ? -1 : 0;
        s = q == 1 ? 1 : q == 3 ? -1 : 0;
    }
    Affine r = {c, -s, px - c * px + s * py, s, c, py - s * px - c * py};
    return r;
}

/// .jlb positions on screen (after the camera) to the picture's rows-from-the-top.
static Affine aff_to_buffer(int h) {
    Affine r = {1, 0, -0.5, 0, -1, h + 0.5};
    return r;
}

static Affine aff_camera(const GfxCamera *cam, int w, int h) {
    Affine id = {1, 0, 0, 0, 1, 0};
    if (!cam || !cam->on) return id;
    double z = cam->zoom > 0 ? cam->zoom : 1;
    double cx = (w + 1) / 2.0, cy = (h + 1) / 2.0;
    // Move the camera's point to the origin, zoom, turn the other way, then to the screen's center.
    Affine m = {z, 0, -z * cam->x, 0, z, -z * cam->y};
    m = aff_mul(aff_rotate_about(-cam->angle, 0, 0), m);
    Affine to = {1, 0, cx, 0, 1, cy};
    return aff_mul(to, m);
}

/// The whole map from .jlb positions to the picture, with a rotation about `pivot`.
static Affine full_map(const GfxTarget *t, double angle, double px, double py) {
    Affine m = aff_to_buffer(t->img->h);
    m = aff_mul(m, aff_camera(t->cam, t->img->w, t->img->h));
    if (angle != 0) m = aff_mul(m, aff_rotate_about(angle, px, py));
    return m;
}

void gfx_screen_to_world(const GfxCamera *cam, int w, int h, double sx, double sy, double *wx, double *wy) {
    Affine m = aff_camera(cam, w, h);
    double det = m.a * m.e - m.b * m.d;
    if (det == 0) {
        *wx = sx, *wy = sy;
        return;
    }
    double x = sx - m.c, y = sy - m.f;
    *wx = (m.e * x - m.b * y) / det;
    *wy = (-m.d * x + m.a * y) / det;
}

/// A box placed by its anchor: its bottom-left corner, and the pivot to turn around.
typedef struct {
    double ox, oy, w, h, px, py;
} Placed;

static Placed place_box(double x, double y, double w, double h, const GfxStyle *st) {
    double fx, fy;
    anchor_frac(st->anchor, &fx, &fy);
    Placed p;
    p.w = w;
    p.h = h;
    // The anchor point of the box sits on the same point of the pixel at (x, y).
    double ax = x - 0.5 + fx, ay = y - 0.5 + fy;
    p.ox = ax - fx * w;
    p.oy = ay - fy * h;
    p.px = ax;
    p.py = ay;
    if (st->set & GS_PIVOT) {
        if (st->pivot_anchor) {
            double gx, gy;
            anchor_frac(st->pivot_anchor, &gx, &gy);
            p.px = p.ox + gx * w;
            p.py = p.oy + gy * h;
        } else {
            p.px = p.ox + st->pivot_x - 0.5;
            p.py = p.oy + st->pivot_y - 0.5;
        }
    }
    return p;
}

/// The pivot for shapes made of points (lines, polygons): their bounding box.
static void points_pivot(const GfxPt *p, int n, const GfxStyle *st, double *px, double *py) {
    double x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].x < x0) x0 = p[i].x;
        if (p[i].x > x1) x1 = p[i].x;
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    // In pixel terms the box covers the pixels at its corners.
    x0 -= 0.5, y0 -= 0.5, x1 += 0.5, y1 += 0.5;
    if ((st->set & GS_PIVOT) && !st->pivot_anchor) {
        *px = x0 + st->pivot_x - 0.5;
        *py = y0 + st->pivot_y - 0.5;
        return;
    }
    double fx = 0.5, fy = 0.5;
    if (st->set & GS_PIVOT) anchor_frac(st->pivot_anchor, &fx, &fy);
    *px = x0 + fx * (x1 - x0);
    *py = y0 + fy * (y1 - y0);
}

// ---------- paths ----------

typedef struct {
    GfxPt *p;
    int n, cap;
} Path;

/// Polygons to fill: contours stored one after another.
typedef struct {
    Path pts;
    int *starts; // where each contour starts in pts
    int nc, ccap;
} Shape;

static void path_add(Path *p, double x, double y) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->p = realloc(p->p, (size_t)p->cap * sizeof(GfxPt));
        if (!p->p) exit(70);
    }
    p->p[p->n].x = x;
    p->p[p->n].y = y;
    p->n++;
}

static void shape_begin(Shape *s) {
    if (s->nc == s->ccap) {
        s->ccap = s->ccap ? s->ccap * 2 : 16;
        s->starts = realloc(s->starts, (size_t)s->ccap * sizeof(int));
        if (!s->starts) exit(70);
    }
    s->starts[s->nc++] = s->pts.n;
}

static void shape_free(Shape *s) {
    free(s->pts.p);
    free(s->starts);
}

static double signed_area(const GfxPt *p, int n) {
    double a = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) a += (p[j].x * p[i].y - p[i].x * p[j].y);
    return a / 2;
}

/// Adds a contour, turned counterclockwise (when `ccw`) or clockwise.
static void shape_contour(Shape *s, const GfxPt *p, int n, bool ccw) {
    if (n < 3) return;
    bool is_ccw = signed_area(p, n) >= 0;
    shape_begin(s);
    if (is_ccw == ccw) {
        for (int i = 0; i < n; i++) path_add(&s->pts, p[i].x, p[i].y);
    } else {
        for (int i = n - 1; i >= 0; i--) path_add(&s->pts, p[i].x, p[i].y);
    }
}

/// How many straight pieces a curve of this radius needs to look round.
static int segments(double r, double sweep_deg) {
    double full = ceil(2 * M_PI * fabs(r) / 2.0);
    if (full < 24) full = 24;
    if (full > 720) full = 720;
    int n = (int)ceil(full * fabs(sweep_deg) / 360);
    return n < 2 ? 2 : n;
}

static void ellipse_points(Path *p, double cx, double cy, double rx, double ry, double start, double sweep, bool closed_full) {
    int n = segments(rx > ry ? rx : ry, sweep);
    if (closed_full) n = (n + 3) / 4 * 4; // a multiple of 4, so the half-step offset clears all four sides
    int last = closed_full ? n - 1 : n;
    // A full ellipse starts half a step round, so no corner sits exactly on a pixel's
    // center at its left, right, top or bottom (that would poke out one pixel on one side).
    double off = closed_full ? sweep / n / 2 : 0;
    for (int i = 0; i <= last; i++) {
        double t = (start + off + sweep * i / n) * M_PI / 180;
        path_add(p, cx + rx * cos(t), cy + ry * sin(t));
    }
}

static void rounded_rect(Path *p, double x0, double y0, double w, double h, double r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        path_add(p, x0, y0);
        path_add(p, x0 + w, y0);
        path_add(p, x0 + w, y0 + h);
        path_add(p, x0, y0 + h);
        return;
    }
    ellipse_points(p, x0 + w - r, y0 + r, r, r, 270, 90, false);
    ellipse_points(p, x0 + w - r, y0 + h - r, r, r, 0, 90, false);
    ellipse_points(p, x0 + r, y0 + h - r, r, r, 90, 90, false);
    ellipse_points(p, x0 + r, y0 + r, r, r, 180, 90, false);
}

/// The polygon moved inward by `t` (it must go counterclockwise), using miter corners.
static void inset(const GfxPt *p, int n, double t, Path *out) {
    for (int i = 0; i < n; i++) {
        GfxPt a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
        double d1x = b.x - a.x, d1y = b.y - a.y, d2x = c.x - b.x, d2y = c.y - b.y;
        double l1 = hypot(d1x, d1y), l2 = hypot(d2x, d2y);
        if (l1 == 0 || l2 == 0) {
            path_add(out, b.x, b.y);
            continue;
        }
        // Inward normals of a counterclockwise polygon point left of each edge.
        double n1x = -d1y / l1, n1y = d1x / l1, n2x = -d2y / l2, n2y = d2x / l2;
        double k = 1 + n1x * n2x + n1y * n2y;
        double mx = n1x, my = n1y;
        if (k > 1e-6) { // the miter corner (a hairpin turn just keeps the first edge's normal)
            mx = (n1x + n2x) / k;
            my = (n1y + n2y) / k;
        }
        double len = hypot(mx, my);
        if (len > 4) mx *= 4 / len, my *= 4 / len; // very sharp corners are cut short
        path_add(out, b.x + mx * t, b.y + my * t);
    }
}

/// A filled shape, or with `outline` > 0 just a band that wide inside its edge.
static void shape_closed(Shape *s, const GfxPt *p, int n, double outline) {
    if (n < 3) return;
    if (outline <= 0) {
        shape_contour(s, p, n, true);
        return;
    }
    // Work on a counterclockwise copy, so "inward" is always to the left.
    GfxPt *q = malloc((size_t)n * sizeof(GfxPt));
    if (!q) exit(70);
    bool ccw = signed_area(p, n) >= 0;
    for (int i = 0; i < n; i++) q[i] = ccw ? p[i] : p[n - 1 - i];
    Path in = {0};
    inset(q, n, outline, &in);
    shape_contour(s, q, n, true);
    // When the band would fill the whole shape, the inner edge turns inside out: skip it.
    if (signed_area(in.p, in.n) > 0) shape_contour(s, in.p, in.n, false);
    free(in.p);
    free(q);
}

/// A thick line from a to b; `ext` makes it longer at both ends (square ends).
static void quad(Shape *s, GfxPt a, GfxPt b, double width, double ext) {
    double dx = b.x - a.x, dy = b.y - a.y, l = hypot(dx, dy);
    if (l == 0) {
        dx = 1, dy = 0, l = 1;
    }
    dx /= l, dy /= l;
    double hw = width / 2, nx = -dy * hw, ny = dx * hw;
    GfxPt q[4] = {{a.x - dx * ext + nx, a.y - dy * ext + ny}, {a.x - dx * ext - nx, a.y - dy * ext - ny},
                  {b.x + dx * ext - nx, b.y + dy * ext - ny}, {b.x + dx * ext + nx, b.y + dy * ext + ny}};
    shape_contour(s, q, 4, true);
}

static void dot(Shape *s, GfxPt c, double r) {
    Path p = {0};
    ellipse_points(&p, c.x, c.y, r, r, 0, 360, true);
    shape_contour(s, p.p, p.n, true);
    free(p.p);
}

/// A line through points: thick pieces with round joins (and square ends unless `butt`).
static void stroke(Shape *s, const GfxPt *p, int n, double width, bool butt) {
    if (n == 1) {
        quad(s, p[0], p[0], width, width / 2);
        return;
    }
    for (int i = 0; i + 1 < n; i++) {
        GfxPt a = p[i], b = p[i + 1];
        double dx = b.x - a.x, dy = b.y - a.y, l = hypot(dx, dy);
        if (l == 0) continue;
        dx /= l, dy /= l;
        // Only the two outer ends get square ends.
        double e0 = !butt && i == 0 ? width / 2 : 0, e1 = !butt && i + 2 == n ? width / 2 : 0;
        GfxPt a2 = {a.x - dx * e0, a.y - dy * e0}, b2 = {b.x + dx * e1, b.y + dy * e1};
        quad(s, a2, b2, width, 0);
    }
    if (width > 1.5) {
        for (int i = 1; i + 1 < n; i++) dot(s, p[i], width / 2);
    } else {
        // Thin lines: a small square at each bend keeps corners filled in.
        for (int i = 1; i + 1 < n; i++) quad(s, p[i], p[i], width, width / 2);
    }
}

/// Catmull-Rom curve through the points.
static void curve_points(const GfxPt *p, int n, Path *out) {
    if (n < 3) {
        for (int i = 0; i < n; i++) path_add(out, p[i].x, p[i].y);
        return;
    }
    for (int i = 0; i + 1 < n; i++) {
        GfxPt p0 = p[i > 0 ? i - 1 : 0], p1 = p[i], p2 = p[i + 1], p3 = p[i + 2 < n ? i + 2 : n - 1];
        double len = hypot(p2.x - p1.x, p2.y - p1.y);
        int steps = (int)ceil(len / 3);
        if (steps < 4) steps = 4;
        if (steps > 200) steps = 200;
        for (int k = 0; k < steps; k++) {
            double t = (double)k / steps, t2 = t * t, t3 = t2 * t;
            double x = 0.5 * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
            double y = 0.5 * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
            path_add(out, x, y);
        }
    }
    path_add(out, p[n - 1].x, p[n - 1].y);
}

// ---------- filling polygons ----------

typedef struct {
    double x0, y0, x1, y1, dxdy;
    int dir;
} Edge;

typedef struct {
    double x;
    int dir;
} Cross;

static int edge_cmp(const void *a, const void *b) {
    double d = ((const Edge *)a)->y0 - ((const Edge *)b)->y0;
    return d < 0 ? -1 : d > 0;
}

static int cross_cmp(const void *a, const void *b) {
    double d = ((const Cross *)a)->x - ((const Cross *)b)->x;
    return d < 0 ? -1 : d > 0;
}

static inline bool inside_rule(int w, bool positive) {
    return positive ? w > 0 : w != 0;
}

/// Colors every pixel the shape covers (after mapping it with `m`).
static void fill_shape(GfxImage *img, const Shape *s, Affine m, uint32_t color, unsigned alpha, bool smooth, bool positive) {
    if (alpha == 0 || s->nc == 0) return;
    int W = img->w, H = img->h;
    // Counterclockwise shapes (in .jlb's y-up positions) count +1 on their inside. The map
    // to rows-from-the-top mirrors them, which decides the sign of an edge going down.
    int flip = (m.a * m.e - m.b * m.d) < 0 ? 1 : -1;
    int ne = 0;
    Edge *E = malloc((size_t)s->pts.n * sizeof(Edge) + sizeof(Edge));
    if (!E) exit(70);
    double ymin = 1e300, ymax = -1e300;
    for (int c = 0; c < s->nc; c++) {
        int a = s->starts[c], b = c + 1 < s->nc ? s->starts[c + 1] : s->pts.n;
        for (int i = a; i < b; i++) {
            GfxPt p = s->pts.p[i], q = s->pts.p[i + 1 < b ? i + 1 : a];
            double x0 = m.a * p.x + m.b * p.y + m.c, y0 = m.d * p.x + m.e * p.y + m.f;
            double x1 = m.a * q.x + m.b * q.y + m.c, y1 = m.d * q.x + m.e * q.y + m.f;
            if (y0 == y1 || !isfinite(x0 + y0 + x1 + y1)) continue;
            Edge e;
            e.dir = flip;
            if (y0 > y1) {
                double t = x0;
                x0 = x1, x1 = t, t = y0, y0 = y1, y1 = t;
                e.dir = -flip;
            }
            e.x0 = x0, e.y0 = y0, e.x1 = x1, e.y1 = y1, e.dxdy = (x1 - x0) / (y1 - y0);
            E[ne++] = e;
            if (y0 < ymin) ymin = y0;
            if (y1 > ymax) ymax = y1;
        }
    }
    if (ne == 0) {
        free(E);
        return;
    }
    qsort(E, (size_t)ne, sizeof(Edge), edge_cmp);
    int sub = smooth ? 4 : 1;
    int r0 = (int)floor(ymin), r1 = (int)ceil(ymax);
    if (r0 < 0) r0 = 0;
    if (r1 > H) r1 = H;
    int *active = malloc((size_t)ne * sizeof(int));
    Cross *cx = malloc((size_t)ne * sizeof(Cross));
    uint16_t *cov = smooth ? calloc((size_t)W, sizeof(uint16_t)) : NULL;
    if (!active || !cx || (smooth && !cov)) exit(70);
    int na = 0, next = 0;
    for (int r = r0; r < r1; r++) {
        int cmin = W, cmax = -1;
        for (int k = 0; k < sub; k++) {
            double y = r + (k + 0.5) / sub;
            // Edges that start by this row join; ones that ended leave.
            while (next < ne && E[next].y0 <= y) active[na++] = next++;
            int nc = 0;
            for (int i = 0; i < na;) {
                Edge *e = &E[active[i]];
                if (e->y1 <= y) {
                    active[i] = active[--na];
                    continue;
                }
                if (e->y0 <= y) {
                    cx[nc].x = e->x0 + (y - e->y0) * e->dxdy;
                    cx[nc].dir = e->dir;
                    nc++;
                }
                i++;
            }
            if (nc < 2) continue;
            qsort(cx, (size_t)nc, sizeof(Cross), cross_cmp);
            int w = 0;
            for (int i = 0; i + 1 < nc; i++) {
                w += cx[i].dir;
                if (!inside_rule(w, positive)) continue;
                double xa = cx[i].x, xb = cx[i + 1].x;
                if (xb <= xa) continue;
                if (!smooth) {
                    // Pixels whose centers are inside [xa, xb).
                    double fa = ceil(xa - 0.5), fb = ceil(xb - 0.5);
                    int ca = fa < 0 ? 0 : fa > W ? W : (int)fa;
                    int cb = fb < 0 ? 0 : fb > W ? W : (int)fb;
                    uint32_t *row = img->px + (size_t)r * W;
                    for (int c = ca; c < cb; c++) row[c] = blend(row[c], color, alpha);
                } else {
                    // 4 sample points across each pixel.
                    double sa_ = ceil(xa * 4 - 0.5), sb_ = ceil(xb * 4 - 0.5);
                    long long sa = sa_ < 0 ? 0 : sa_ > W * 4.0 ? W * 4LL : (long long)sa_;
                    long long sb = sb_ < 0 ? 0 : sb_ > W * 4.0 ? W * 4LL : (long long)sb_;
                    for (long long q = sa; q < sb;) {
                        int c = (int)(q / 4);
                        long long end = (long long)(c + 1) * 4;
                        if (end > sb) end = sb;
                        cov[c] += (uint16_t)(end - q);
                        if (c < cmin) cmin = c;
                        if (c > cmax) cmax = c;
                        q = end;
                    }
                }
            }
        }
        if (smooth && cmax >= cmin) {
            uint32_t *row = img->px + (size_t)r * W;
            for (int c = cmin; c <= cmax; c++) {
                if (cov[c]) {
                    unsigned a = (alpha * (cov[c] > 16 ? 16 : cov[c]) + 8) / 16;
                    row[c] = blend(row[c], color, a);
                    cov[c] = 0;
                }
            }
        }
    }
    free(cov);
    free(cx);
    free(active);
    free(E);
}

// ---------- shapes ----------

bool gfx_shape(GfxTarget *t, GfxKind k, const double *n, int nn, const GfxPt *pts, int npts, uint32_t color,
               const GfxStyle *call, char *err) {
    (void)nn;
    GfxStyle st = resolve(call);
    unsigned alpha = mul255(color >> 24, (unsigned)(st.alpha < 0 ? 0 : st.alpha > 255 ? 255 : st.alpha));
    if (alpha == 0) return true;
    double ol = st.outline > 0 ? st.outline : 0;
    Shape s = {0};
    Path p = {0};
    double px = 0, py = 0;
    bool positive = false;
    switch (k) {
    case GK_PIXEL: {
        if ((!t->cam || !t->cam->on)) {
            // The common case, straight to the pixel.
            double c = floor(n[0] + 0.5) - 1, r = t->img->h - floor(n[1] + 0.5);
            if (c >= 0 && c < t->img->w && r >= 0 && r < t->img->h) {
                uint32_t *q = t->img->px + (size_t)r * t->img->w + (size_t)c;
                *q = blend(*q, color, alpha);
            }
            return true;
        }
        double x = floor(n[0] + 0.5), y = floor(n[1] + 0.5);
        GfxPt q[4] = {{x - 0.5, y - 0.5}, {x + 0.5, y - 0.5}, {x + 0.5, y + 0.5}, {x - 0.5, y + 0.5}};
        shape_contour(&s, q, 4, true);
        fill_shape(t->img, &s, full_map(t, 0, 0, 0), color, alpha, false, false);
        shape_free(&s);
        return true;
    }
    case GK_LINE:
    case GK_ARROW: {
        GfxPt a = {n[0], n[1]}, b = {n[2], n[3]};
        double w = n[4] > 0 ? n[4] : 1;
        GfxPt two[2] = {a, b};
        points_pivot(two, 2, &st, &px, &py);
        if (k == GK_LINE) {
            quad(&s, a, b, w, w / 2);
        } else {
            double dx = b.x - a.x, dy = b.y - a.y, l = hypot(dx, dy);
            if (l == 0) l = 1, dx = 1, dy = 0;
            dx /= l, dy /= l;
            double hl = w * 4 > 6 ? w * 4 : 6, hw = w * 3 > 5 ? w * 3 : 5;
            if (hl > l) hl = l;
            GfxPt base = {b.x - dx * hl, b.y - dy * hl};
            quad(&s, a, base, w, 0);
            GfxPt head[3] = {{b.x + dx * w / 2, b.y + dy * w / 2}, {base.x - dy * hw / 2, base.y + dx * hw / 2}, {base.x + dy * hw / 2, base.y - dx * hw / 2}};
            shape_contour(&s, head, 3, true);
        }
        break;
    }
    case GK_LINES:
    case GK_CURVE: {
        if (npts < 1) return true;
        points_pivot(pts, npts, &st, &px, &py);
        double w = n[0] > 0 ? n[0] : 1;
        if (k == GK_CURVE) {
            curve_points(pts, npts, &p);
            stroke(&s, p.p, p.n, w, false);
        } else {
            stroke(&s, pts, npts, w, false);
        }
        break;
    }
    case GK_RECT: {
        Placed b = place_box(n[0], n[1], n[2], n[3], &st);
        px = b.px, py = b.py;
        double r = st.radius > 0 ? st.radius : 0;
        rounded_rect(&p, b.ox, b.oy, b.w, b.h, r);
        if (ol > 0 && ol * 2 < (b.w < b.h ? b.w : b.h)) {
            shape_contour(&s, p.p, p.n, true);
            Path in = {0};
            rounded_rect(&in, b.ox + ol, b.oy + ol, b.w - 2 * ol, b.h - 2 * ol, r > ol ? r - ol : 0);
            shape_contour(&s, in.p, in.n, false);
            free(in.p);
            positive = true;
        } else {
            shape_contour(&s, p.p, p.n, true);
        }
        break;
    }
    case GK_CIRCLE:
    case GK_ELLIPSE: {
        double w = k == GK_CIRCLE ? n[2] * 2 : n[2], h = k == GK_CIRCLE ? n[2] * 2 : n[3];
        Placed b = place_box(n[0], n[1], w, h, &st);
        px = b.px, py = b.py;
        double cx = b.ox + w / 2, cy = b.oy + h / 2;
        ellipse_points(&p, cx, cy, w / 2, h / 2, 0, 360, true);
        if (ol > 0 && ol * 2 < (w < h ? w : h)) {
            shape_contour(&s, p.p, p.n, true);
            Path in = {0};
            ellipse_points(&in, cx, cy, w / 2 - ol, h / 2 - ol, 0, 360, true);
            shape_contour(&s, in.p, in.n, false);
            free(in.p);
            positive = true;
        } else {
            shape_contour(&s, p.p, p.n, true);
        }
        break;
    }
    case GK_TRIANGLE:
    case GK_POLYGON: {
        if (npts < 3) {
            if (err) snprintf(err, GFX_ERR, "a polygon needs at least 3 points, not %d", npts);
            return false;
        }
        points_pivot(pts, npts, &st, &px, &py);
        shape_closed(&s, pts, npts, ol);
        positive = ol > 0;
        break;
    }
    case GK_NGON:
    case GK_STAR: {
        int count = (int)(k == GK_STAR ? n[4] : n[3]);
        if (count < (k == GK_NGON ? 3 : 2) || count > 1000) {
            if (err) snprintf(err, GFX_ERR, k == GK_NGON ? "NGON needs 3 or more sides" : "STAR needs 2 or more points");
            return false;
        }
        double ro = n[2], ri = k == GK_STAR ? n[3] : n[2];
        Placed b = place_box(n[0], n[1], ro * 2, ro * 2, &st);
        px = b.px, py = b.py;
        double cx = b.ox + ro, cy = b.oy + ro;
        int corners = k == GK_STAR ? count * 2 : count;
        for (int i = 0; i < corners; i++) {
            double a = (90 + 360.0 * i / corners) * M_PI / 180, r = (k == GK_STAR && i % 2) ? ri : ro;
            path_add(&p, cx + r * cos(a), cy + r * sin(a));
        }
        shape_closed(&s, p.p, p.n, ol);
        positive = ol > 0;
        break;
    }
    case GK_ARC:
    case GK_PIE: {
        double r = n[2], a0 = n[3], sweep = n[4] - n[3];
        Placed b = place_box(n[0], n[1], r * 2, r * 2, &st);
        px = b.px, py = b.py;
        double cx = b.ox + r, cy = b.oy + r;
        if (sweep > 360) sweep = 360;
        if (sweep < -360) sweep = -360;
        if (k == GK_PIE) path_add(&p, cx, cy);
        ellipse_points(&p, cx, cy, r, r, a0, sweep, false);
        if (k == GK_ARC) {
            double w = n[5] > 0 ? n[5] : 1;
            // An arc's ends are cut straight, along the circle.
            for (int i = 0; i + 1 < p.n; i++) quad(&s, p.p[i], p.p[i + 1], w, 0);
            for (int i = 1; i + 1 < p.n; i++) quad(&s, p.p[i], p.p[i], w, w / 2);
        } else {
            shape_closed(&s, p.p, p.n, ol);
            positive = ol > 0;
        }
        break;
    }
    }
    fill_shape(t->img, &s, full_map(t, st.angle, px, py), color, alpha, st.smooth, positive);
    shape_free(&s);
    free(p.p);
    return true;
}

void gfx_clear(GfxImage *img, uint32_t color, bool opaque) {
    if (opaque) color = blend(0xFF000000u, color, color >> 24);
    for (size_t i = 0, n = (size_t)img->w * img->h; i < n; i++) img->px[i] = color;
}

/// The picture's pixel at a .jlb position (after the camera), or false off the picture.
static bool to_pixel(GfxTarget *t, double x, double y, int *c, int *r) {
    Affine m = full_map(t, 0, 0, 0);
    double bx = m.a * x + m.b * y + m.c, by = m.d * x + m.e * y + m.f;
    *c = (int)floor(bx);
    *r = (int)floor(by);
    return *c >= 0 && *c < t->img->w && *r >= 0 && *r < t->img->h;
}

bool gfx_get_pixel(GfxTarget *t, double x, double y, uint32_t *out) {
    int c, r;
    if (!to_pixel(t, x, y, &c, &r)) return false;
    *out = t->img->px[(size_t)r * t->img->w + (size_t)c];
    return true;
}

void gfx_flood(GfxTarget *t, double x, double y, uint32_t color) {
    int c, r;
    if (!to_pixel(t, x, y, &c, &r)) return;
    GfxImage *img = t->img;
    int W = img->w, H = img->h;
    uint32_t from = img->px[(size_t)r * W + (size_t)c];
    uint32_t to = blend(from, color, color >> 24);
    if (from == to) return;
    // Scanline flood fill with an explicit stack of seeds.
    size_t cap = 1024, n = 0;
    int *stack = malloc(cap * 2 * sizeof(int));
    if (!stack) exit(70);
    stack[n * 2] = c, stack[n * 2 + 1] = r, n++;
    while (n) {
        n--;
        int sx = stack[n * 2], sy = stack[n * 2 + 1];
        uint32_t *row = img->px + (size_t)sy * W;
        if (row[sx] != from) continue;
        int a = sx, b = sx;
        while (a > 0 && row[a - 1] == from) a--;
        while (b + 1 < W && row[b + 1] == from) b++;
        for (int i = a; i <= b; i++) row[i] = to;
        for (int dy = -1; dy <= 1; dy += 2) {
            int ny = sy + dy;
            if (ny < 0 || ny >= H) continue;
            uint32_t *nr = img->px + (size_t)ny * W;
            for (int i = a; i <= b; i++) {
                if (nr[i] == from && (i == a || nr[i - 1] != from)) {
                    if (n == cap) {
                        cap *= 2;
                        stack = realloc(stack, cap * 2 * sizeof(int));
                        if (!stack) exit(70);
                    }
                    stack[n * 2] = i, stack[n * 2 + 1] = ny, n++;
                }
            }
        }
    }
    free(stack);
}

// ---------- drawing pictures (images and text) ----------

/// Something to draw pixel by pixel: an image, or a coverage mask in one color.
typedef struct {
    const uint32_t *argb;
    const uint8_t *mask;
    int w, h;
    uint32_t color;
} Source;

static inline uint32_t texel(const Source *s, int u, int v) {
    if (s->argb) return s->argb[(size_t)v * s->w + (size_t)u];
    return (uint32_t)s->mask[(size_t)v * s->w + (size_t)u] << 24 | (s->color & 0xFFFFFF);
}

static uint32_t sample_smooth(const Source *s, double u, double v) {
    u -= 0.5, v -= 0.5;
    int u0 = (int)floor(u), v0 = (int)floor(v);
    double fu = u - u0, fv = v - v0;
    double acc[4] = {0, 0, 0, 0}, wsum = 0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int uu = u0 + i, vv = v0 + j;
            if (uu < 0) uu = 0;
            if (vv < 0) vv = 0;
            if (uu >= s->w) uu = s->w - 1;
            if (vv >= s->h) vv = s->h - 1;
            double w = (i ? fu : 1 - fu) * (j ? fv : 1 - fv);
            uint32_t c = texel(s, uu, vv);
            double a = (c >> 24) * w;
            // Weight colors by how solid they are, so see-through edges don't go dark.
            acc[0] += a;
            acc[1] += (c >> 16 & 255) * a;
            acc[2] += (c >> 8 & 255) * a;
            acc[3] += (c & 255) * a;
            wsum += w;
        }
    }
    if (acc[0] <= 0) return 0;
    unsigned a = (unsigned)(acc[0] / wsum + 0.5);
    unsigned r = (unsigned)(acc[1] / acc[0] + 0.5), g = (unsigned)(acc[2] / acc[0] + 0.5), b = (unsigned)(acc[3] / acc[0] + 0.5);
    return (uint32_t)a << 24 | r << 16 | g << 8 | b;
}

/// Draws `src` into the box (bottom-left ox, oy; size bw x bh), flipped as asked, with
/// map `m` from .jlb positions to the picture.
static void draw_source(GfxImage *img, const Source *src, Affine m, double ox, double oy, double bw, double bh, bool fx, bool fy,
                        uint32_t tint, unsigned alpha, bool smooth) {
    if (alpha == 0 || bw == 0 || bh == 0) return;
    // (s, t) in [0, 1) across the box -> picture: P = A (s, t) + B.
    double A00 = m.a * bw, A01 = m.b * bh, A10 = m.d * bw, A11 = m.e * bh;
    double B0 = m.a * ox + m.b * oy + m.c, B1 = m.d * ox + m.e * oy + m.f;
    double det = A00 * A11 - A01 * A10;
    if (det == 0) return;
    double xs[4] = {B0, B0 + A00, B0 + A01, B0 + A00 + A01}, ys[4] = {B1, B1 + A10, B1 + A11, B1 + A10 + A11};
    double x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < x0) x0 = xs[i];
        if (xs[i] > x1) x1 = xs[i];
        if (ys[i] < y0) y0 = ys[i];
        if (ys[i] > y1) y1 = ys[i];
    }
    int c0 = (int)floor(x0), c1 = (int)ceil(x1), r0 = (int)floor(y0), r1 = (int)ceil(y1);
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > img->w) c1 = img->w;
    if (r1 > img->h) r1 = img->h;
    double I00 = A11 / det, I01 = -A01 / det, I10 = -A10 / det, I11 = A00 / det;
    unsigned tr = tint >> 16 & 255, tg = tint >> 8 & 255, tb = tint & 255, ta = tint >> 24;
    bool plain_tint = (tint & 0xFFFFFF) == 0xFFFFFF;
    for (int r = r0; r < r1; r++) {
        uint32_t *row = img->px + (size_t)r * img->w;
        double py = r + 0.5 - B1;
        for (int c = c0; c < c1; c++) {
            double pxx = c + 0.5 - B0;
            double s = I00 * pxx + I01 * py, t = I10 * pxx + I11 * py;
            if (s < 0 || s >= 1 || t < 0 || t >= 1) continue;
            double u = (fx ? 1 - s : s) * src->w, v = (fy ? t : 1 - t) * src->h;
            uint32_t px;
            if (smooth) {
                px = sample_smooth(src, u, v);
            } else {
                int ui = (int)floor(u + 1e-9), vi = (int)floor(v + 1e-9);
                if (ui >= src->w) ui = src->w - 1;
                if (vi >= src->h) vi = src->h - 1;
                if (ui < 0) ui = 0;
                if (vi < 0) vi = 0;
                px = texel(src, ui, vi);
            }
            unsigned a = mul255(mul255(px >> 24, ta), alpha);
            if (!a) continue;
            if (!plain_tint) {
                px = mul255(px >> 16 & 255, tr) << 16 | mul255(px >> 8 & 255, tg) << 8 | mul255(px & 255, tb);
            }
            row[c] = blend(row[c], px, a);
        }
    }
}

/// True when the map is a plain shift (no turning or resizing), so pictures stay pixel-sharp.
static bool is_plain(Affine m, double sx, double sy) {
    return m.b == 0 && m.d == 0 && fabs(m.a) == 1 && fabs(m.e) == 1 && fabs(sx) == 1 && fabs(sy) == 1;
}

bool gfx_draw_image(GfxTarget *t, const GfxImage *img, double x, double y, const GfxStyle *call, char *err) {
    (void)err;
    GfxStyle st = resolve(call);
    double sx = st.scale_x, sy = st.scale_y;
    bool fx = (sx < 0) != st.flip_x, fy = (sy < 0) != st.flip_y;
    double bw = img->w * fabs(sx), bh = img->h * fabs(sy);
    Placed b = place_box(x, y, bw, bh, &st);
    Affine m = full_map(t, st.angle, b.px, b.py);
    Source src = {img->px, NULL, img->w, img->h, 0};
    bool smooth = st.smooth && !is_plain(m, sx, sy);
    draw_source(t->img, &src, m, b.ox, b.oy, bw, bh, fx, fy, st.tint, (unsigned)(st.alpha < 0 ? 0 : st.alpha > 255 ? 255 : st.alpha), smooth);
    return true;
}

// ---------- fonts and text ----------

#define GLYPH_CACHE 512

typedef struct {
    int cp, size64, sub; // codepoint, size in 1/64 px, horizontal offset in quarter pixels
    int w, h, x0, y0;
    uint8_t *bits;
} Glyph;

struct GfxFont {
    int refs;
    bool pixel;
    unsigned char *data; // owned (NULL for the built-in one)
    stbtt_fontinfo info;
    Glyph cache[GLYPH_CACHE];
};

static GfxFont *builtin_smooth, *builtin_pixel;

GfxFont *gfx_font_load(const char *path, char *err) {
    if (!path || strcmp(path, "pixel") == 0) {
        GfxFont **slot = path ? &builtin_pixel : &builtin_smooth;
        if (!*slot) {
            GfxFont *f = xalloc(sizeof *f);
            f->refs = 1; // kept for the whole run
            f->pixel = path != NULL;
            if (!f->pixel) stbtt_InitFont(&f->info, FONT_DEFAULT, stbtt_GetFontOffsetForIndex(FONT_DEFAULT, 0));
            *slot = f;
        }
        gfx_font_ref(*slot);
        return *slot;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        seterr(err, "can't open the font %s (no such file)", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > 64L * 1024 * 1024) {
        fclose(fp);
        seterr(err, "%s isn't a font this can read", path);
        return NULL;
    }
    unsigned char *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(d);
        seterr(err, "couldn't read the font %s", path);
        return NULL;
    }
    fclose(fp);
    GfxFont *f = xalloc(sizeof *f);
    f->refs = 1;
    f->data = d;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, d, off)) {
        free(d);
        free(f);
        seterr(err, "%s isn't a font this can read (TTF and OTF fonts work)", path);
        return NULL;
    }
    return f;
}

void gfx_font_ref(GfxFont *f) {
    if (f) f->refs++;
}

void gfx_font_unref(GfxFont *f) {
    if (f && --f->refs == 0) {
        for (int i = 0; i < GLYPH_CACHE; i++) free(f->cache[i].bits);
        free(f->data);
        free(f);
    }
}

bool gfx_font_is_pixel(const GfxFont *f) {
    return f && f->pixel;
}

static int utf8_next(const char *s, size_t n, size_t *i) {
    unsigned char c = (unsigned char)s[*i];
    int cp, extra;
    if (c < 0x80) cp = c, extra = 0;
    else if ((c & 0xE0) == 0xC0) cp = c & 0x1F, extra = 1;
    else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, extra = 2;
    else if ((c & 0xF8) == 0xF0) cp = c & 0x07, extra = 3;
    else {
        (*i)++;
        return 0xFFFD;
    }
    (*i)++;
    for (int k = 0; k < extra; k++) {
        if (*i >= n || ((unsigned char)s[*i] & 0xC0) != 0x80) return 0xFFFD;
        cp = cp << 6 | ((unsigned char)s[*i] & 0x3F);
        (*i)++;
    }
    return cp;
}

/// Text measured and laid out: lines, each with its width and where it starts in the text.
typedef struct {
    GfxFont *font;
    double size, scale; // pixel size; stb's scale for smooth fonts, whole pixel steps for the pixel font
    double line_h, ascent, height, width;
    int nlines;
    struct {
        size_t a, b;
        double w;
    } *lines;
} Layout;

static double advance(const Layout *L, int cp, int next) {
    if (L->font->pixel) return 8 * L->scale;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&L->font->info, cp, &adv, &lsb);
    double a = adv * L->scale;
    if (next) a += stbtt_GetCodepointKernAdvance(&L->font->info, cp, next) * L->scale;
    return a;
}

static double measure(const Layout *L, const char *s, size_t a, size_t b) {
    double w = 0;
    size_t i = a;
    while (i < b) {
        int cp = utf8_next(s, b, &i);
        size_t j = i;
        int next = j < b ? utf8_next(s, b, &j) : 0;
        w += advance(L, cp, next);
    }
    return w;
}

static void add_line(Layout *L, const char *s, size_t a, size_t b) {
    L->lines = realloc(L->lines, (size_t)(L->nlines + 1) * sizeof *L->lines);
    if (!L->lines) exit(70);
    L->lines[L->nlines].a = a;
    L->lines[L->nlines].b = b;
    L->lines[L->nlines].w = measure(L, s, a, b);
    L->nlines++;
}

static void layout(Layout *L, const char *s, size_t n, const GfxStyle *st) {
    memset(L, 0, sizeof *L);
    L->font = st->font ? st->font : NULL;
    if (!L->font) {
        static GfxFont *def;
        if (!def) def = gfx_font_load(NULL, NULL);
        L->font = def;
    }
    if (L->font->pixel) {
        double k = st->size > 0 ? floor(st->size / 8 + 0.5) : 1;
        if (k < 1) k = 1;
        L->size = 8 * k;
        L->scale = k;
        L->ascent = 8 * k;
        L->line_h = 10 * k;
    } else {
        L->size = st->size > 0 ? st->size : 16;
        L->scale = stbtt_ScaleForPixelHeight(&L->font->info, (float)L->size);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&L->font->info, &asc, &desc, &gap);
        L->ascent = asc * L->scale;
        L->line_h = (asc - desc + gap) * L->scale;
    }
    // Split at new lines, then wrap long lines at spaces.
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n') continue;
        size_t a = start, b = i;
        if (b > a && s[b - 1] == '\r') b--;
        if (st->wrap > 0) {
            while (a < b && measure(L, s, a, b) > st->wrap) {
                size_t cut = 0;
                for (size_t k = a; k < b; k++) {
                    if (s[k] == ' ' && measure(L, s, a, k) <= st->wrap) cut = k;
                }
                if (!cut) { // one long word: break it where it no longer fits
                    size_t k = a;
                    while (k < b) {
                        size_t j = k;
                        utf8_next(s, b, &j);
                        if (k > a && measure(L, s, a, j) > st->wrap) break;
                        k = j;
                    }
                    cut = k;
                    add_line(L, s, a, cut);
                    a = cut;
                    continue;
                }
                add_line(L, s, a, cut);
                a = cut + 1;
            }
        }
        add_line(L, s, a, b);
        start = i + 1;
    }
    for (int i = 0; i < L->nlines; i++) {
        if (L->lines[i].w > L->width) L->width = L->lines[i].w;
    }
    double glyph_h = L->font->pixel ? L->size : L->line_h;
    L->height = L->nlines ? glyph_h + (L->nlines - 1) * L->line_h : 0;
}

void gfx_text_size(const char *s, size_t len, const GfxStyle *call, double *w, double *h) {
    GfxStyle st = resolve(call);
    Layout L;
    layout(&L, s, len, &st);
    double sx = fabs(st.scale_x), sy = fabs(st.scale_y);
    *w = ceil(L.width) * sx;
    *h = ceil(L.height) * sy;
    free(L.lines);
}

static const Glyph *glyph(GfxFont *f, int cp, double scale, double size, int sub) {
    int size64 = (int)(size * 64 + 0.5);
    unsigned hsh = ((unsigned)cp * 2654435761u ^ (unsigned)size64 * 40503u ^ (unsigned)sub * 97u) % GLYPH_CACHE;
    Glyph *g = &f->cache[hsh];
    if (g->bits && g->cp == cp && g->size64 == size64 && g->sub == sub) return g;
    free(g->bits);
    memset(g, 0, sizeof *g);
    int w, h, x0, y0;
    unsigned char *b = stbtt_GetCodepointBitmapSubpixel(&f->info, (float)scale, (float)scale, sub / 4.0f, 0, cp, &w, &h, &x0, &y0);
    g->cp = cp, g->size64 = size64, g->sub = sub, g->w = w, g->h = h, g->x0 = x0, g->y0 = y0;
    g->bits = malloc(w * h > 0 ? (size_t)w * h : 1);
    if (!g->bits) exit(70);
    if (b && w > 0 && h > 0) memcpy(g->bits, b, (size_t)w * h);
    stbtt_FreeBitmap(b, NULL);
    return g;
}

/// Renders the laid-out text as a coverage mask (rows from the top).
static uint8_t *render_mask(const Layout *L, const char *s, int align, int *mw, int *mh) {
    int W = (int)ceil(L->width), H = (int)ceil(L->height);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    uint8_t *m = calloc((size_t)W * H, 1);
    if (!m) exit(70);
    for (int li = 0; li < L->nlines; li++) {
        double x = align == 1 ? (L->width - L->lines[li].w) / 2 : align == 2 ? L->width - L->lines[li].w : 0;
        double base = li * L->line_h + L->ascent;
        size_t i = L->lines[li].a, b = L->lines[li].b;
        while (i < b) {
            int cp = utf8_next(s, b, &i);
            size_t j = i;
            int next = j < b ? utf8_next(s, b, &j) : 0;
            if (L->font->pixel) {
                int k = (int)L->scale, gx = (int)floor(x + 0.5), gy = (int)floor(base - 8 * k + 0.5);
                int idx = cp < 128 ? cp : (cp >= 0xA0 && cp <= 0xFF) ? cp - 0xA0 + 128 : '?';
                for (int ry = 0; ry < 8; ry++) {
                    unsigned bits = FONT8X8[idx][ry];
                    for (int rx = 0; rx < 8; rx++) {
                        if (!(bits >> rx & 1)) continue;
                        for (int yy = 0; yy < k; yy++) {
                            for (int xx = 0; xx < k; xx++) {
                                int px = gx + rx * k + xx, py = gy + ry * k + yy;
                                if (px >= 0 && px < W && py >= 0 && py < H) m[(size_t)py * W + px] = 255;
                            }
                        }
                    }
                }
            } else {
                double fx = floor(x);
                int sub = (int)floor((x - fx) * 4);
                const Glyph *g = glyph(L->font, cp, L->scale, L->size, sub);
                int gx = (int)fx + g->x0, gy = (int)floor(base + 0.5) + g->y0;
                for (int ry = 0; ry < g->h; ry++) {
                    int py = gy + ry;
                    if (py < 0 || py >= H) continue;
                    for (int rx = 0; rx < g->w; rx++) {
                        int px = gx + rx;
                        if (px < 0 || px >= W) continue;
                        unsigned v = g->bits[(size_t)ry * g->w + rx] + m[(size_t)py * W + px];
                        m[(size_t)py * W + px] = (uint8_t)(v > 255 ? 255 : v);
                    }
                }
            }
            x += advance(L, cp, next);
        }
    }
    *mw = W;
    *mh = H;
    return m;
}

bool gfx_text(GfxTarget *t, double x, double y, const char *s, size_t len, uint32_t color, const GfxStyle *call, char *err) {
    (void)err;
    GfxStyle st = resolve(call);
    Layout L;
    layout(&L, s, len, &st);
    int mw, mh;
    uint8_t *mask = render_mask(&L, s, st.align, &mw, &mh);
    double sx = st.scale_x, sy = st.scale_y;
    bool fx = (sx < 0) != st.flip_x, fy = (sy < 0) != st.flip_y;
    double bw = mw * fabs(sx), bh = mh * fabs(sy);
    Placed b = place_box(x, y, bw, bh, &st);
    Affine m = full_map(t, st.angle, b.px, b.py);
    Source src = {NULL, mask, mw, mh, color};
    bool smooth = !L.font->pixel && !is_plain(m, sx, sy);
    unsigned alpha = mul255(color >> 24, (unsigned)(st.alpha < 0 ? 0 : st.alpha > 255 ? 255 : st.alpha));
    draw_source(t->img, &src, m, b.ox, b.oy, bw, bh, fx, fy, st.tint, alpha, smooth);
    free(mask);
    free(L.lines);
    return true;
}

// ---------- keys ----------

static const struct {
    const char *name;
    int code;
} KEYNAMES[] = {
    {"space", GFX_KEY_SPACE}, {"enter", GFX_KEY_ENTER}, {"escape", GFX_KEY_ESCAPE}, {"tab", GFX_KEY_TAB},
    {"backspace", GFX_KEY_BACKSPACE}, {"delete", GFX_KEY_DELETE}, {"insert", GFX_KEY_INSERT}, {"left", GFX_KEY_LEFT},
    {"right", GFX_KEY_RIGHT}, {"up", GFX_KEY_UP}, {"down", GFX_KEY_DOWN}, {"home", GFX_KEY_HOME}, {"end", GFX_KEY_END},
    {"pageup", GFX_KEY_PAGEUP}, {"pagedown", GFX_KEY_PAGEDOWN}, {"shift", GFX_KEY_SHIFT}, {"ctrl", GFX_KEY_CTRL},
    {"alt", GFX_KEY_ALT},
};

int gfx_key_code(const char *name, size_t n) {
    char low[16];
    if (n == 0 || n >= sizeof low) return -1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[n] = 0;
    if (n == 1 && (unsigned char)low[0] > ' ' && (unsigned char)low[0] < 127) return low[0];
    for (size_t i = 0; i < sizeof KEYNAMES / sizeof KEYNAMES[0]; i++) {
        if (strcmp(KEYNAMES[i].name, low) == 0) return KEYNAMES[i].code;
    }
    if (low[0] == 'f' && n <= 3) {
        int k = atoi(low + 1);
        if (k >= 1 && k <= 12 && (n == 2 || low[1] != '0')) return GFX_KEY_F1 + k - 1;
    }
    return -1;
}

const char *gfx_key_name(int code) {
    static char one[2];
    static char fk[4];
    if (code > ' ' && code < 127) {
        one[0] = (char)code;
        one[1] = 0;
        return one;
    }
    for (size_t i = 0; i < sizeof KEYNAMES / sizeof KEYNAMES[0]; i++) {
        if (KEYNAMES[i].code == code) return KEYNAMES[i].name;
    }
    if (code >= GFX_KEY_F1 && code < GFX_KEY_F1 + 12) {
        snprintf(fk, sizeof fk, "f%d", code - GFX_KEY_F1 + 1);
        return fk;
    }
    return NULL;
}
