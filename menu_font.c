/*
 * menu_font.c - TTF text rendering for the picoarch menu.
 * Ported from FrogUI's font.c. Loads GamePocket (FrogUI's default) from the
 * FrogUI font dirs and renders uppercase RGB565 glyphs, matching FrogUI's look.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "menu_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

static stbtt_fontinfo font_info;
static unsigned char *font_buffer = NULL;
static size_t font_buffer_size = 0;
static float font_scale = 0.0f;
static stbtt_fontinfo fallback_info;
static unsigned char *fallback_buffer = NULL;
static size_t fallback_buffer_size = 0;
static float fallback_scale = 0.0f;
static int fallback_loaded = 0;
static stbtt_fontinfo latin_info;
static unsigned char *latin_buffer = NULL;
static size_t latin_buffer_size = 0;
static float latin_scale = 0.0f;
static int latin_loaded = 0;
static int active_font_id = 0;
static int unicode_upper(int cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0xE0 && cp <= 0xF6) return cp - 0x20;
    if (cp >= 0xF8 && cp <= 0xFE) return cp - 0x20;
    switch (cp) { case 0x0105:return 0x0104; case 0x0107:return 0x0106; case 0x0119:return 0x0118; case 0x0142:return 0x0141; case 0x0144:return 0x0143; case 0x015B:return 0x015A; case 0x017A:return 0x0179; case 0x017C:return 0x017B; default:return cp; }
}
static int   font_loaded = 0;
static float font_px = 20.0f;

static int load_font_into(const char *fname, stbtt_fontinfo *info,
                          unsigned char **buffer_out, size_t *buffer_size_out,
                          float *scale_out) {
    char paths[4][256];
    snprintf(paths[0], sizeof(paths[0]), "/mnt/sdcard/cubegm/fonts/%s", fname);
    snprintf(paths[1], sizeof(paths[1]), "/mnt/sdcard/frogui/fonts/%s", fname);
    snprintf(paths[2], sizeof(paths[2]), "fonts/%s", fname);
    snprintf(paths[3], sizeof(paths[3]), "%s", fname);

    FILE *fp = NULL;
    for (int i = 0; i < 4; i++) { fp = fopen(paths[i], "rb"); if (fp) break; }
    if (!fp) return 0;

    struct stat st;
    if (fstat(fileno(fp), &st) || st.st_size <= 0) { fclose(fp); return 0; }
    size_t sz = (size_t)st.st_size;
    unsigned char *buf = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fileno(fp), 0);
    fclose(fp);
    if (buf == MAP_FAILED) return 0;

    if (!stbtt_InitFont(info, buf, stbtt_GetFontOffsetForIndex(buf, 0))) {
        munmap(buf, sz);
        return 0;
    }
    if (*buffer_out) munmap(*buffer_out, *buffer_size_out);
    *buffer_out = buf;
    *buffer_size_out = sz;
    *scale_out = stbtt_ScaleForPixelHeight(info, font_px);
    return 1;
}

static int load_font_file(const char *fname) {
    if (!load_font_into(fname, &font_info, &font_buffer, &font_buffer_size, &font_scale)) return 0;
    font_loaded = 1;
    return 1;
}

static int load_latin_fallback(void) {
    if (latin_loaded) return 1;
    if (!load_font_into("TreeFrogLatin.ttf", &latin_info, &latin_buffer, &latin_buffer_size, &latin_scale)) return 0;
    latin_loaded = 1;
    return 1;
}

static int load_unicode_fallback(void) {
    if (fallback_loaded) return 1;
    if (!load_font_into("TreeFrogUnicode.ttf", &fallback_info, &fallback_buffer, &fallback_buffer_size, &fallback_scale)) return 0;
    fallback_loaded = 1;
    return 1;
}

int menu_font_init(float pixel_height) {
    if (pixel_height > 0) font_px = pixel_height;
    if (font_loaded) return 1;
    /* FrogUI's default font first, then monogram as fallback. */
    load_font_file("GamePocket-Regular-ZeroKern.ttf");
    if (!font_loaded) load_font_file("monogram.ttf");
    /* Unicode fallback is 11 MB. font_for_cp() loads it only when needed. */
    return font_loaded;
}

int menu_font_ready(void) { return font_loaded; }
int menu_font_height(void) { return (int)font_px; }

/* Persistent glyph cache. draw_char used to run stbtt_GetGlyphBitmapBox +
 * stbtt_MakeGlyphBitmap (both allocate internally: vertex arrays, active-edge
 * lists) on EVERY glyph EVERY frame. On the memory-pressured device (SF3500 fw
 * 1.1) an internal alloc intermittently fails mid-parse → the glyph rasterizes
 * with a corrupted bbox/bitmap and blits vertically displaced (the "T of EXIT
 * shifted half a row up" bug), self-healing on the next redraw when the alloc
 * succeeds. Fix: rasterize each glyph ONCE into this cache and blit from it —
 * no per-frame stbtt, nothing left to corrupt. Rebuilt only when the pixel size
 * changes. */
#define GC_FIRST 32
#define GC_LAST  126
#define GC_N     (GC_LAST - GC_FIRST + 1)
#define GC_MAX   24                 /* max glyph dim cached (menu px ~14 → ~10) */
typedef struct {
    unsigned char ready;            /* 0=empty 1=rasterized 2=blank(space) */
    short w, h, x0, y0;
    unsigned char bmp[GC_MAX * GC_MAX];
} gcache_t;
static gcache_t gcache[GC_N];
static int      gcache_baseline = 0;

static void gcache_reset(void) {
    for (int i = 0; i < GC_N; i++) gcache[i].ready = 0;
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
    gcache_baseline = (int)(ascent * font_scale);
}

/* Rasterize glyph for uppercased char c into the cache (once). NULL if oversized
 * (caller skips). ready==2 means valid-but-no-ink (space). */
static gcache_t *gcache_get(char c) {
    if (c < GC_FIRST || c > GC_LAST) return NULL;
    gcache_t *g = &gcache[(int)c - GC_FIRST];
    if (g->ready) return g;
    int gi = stbtt_FindGlyphIndex(&font_info, c);
    if (gi == 0) { g->w = 0; g->ready = 2; return g; }
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&font_info, gi, font_scale, font_scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) { g->w = 0; g->ready = 2; return g; }
    if (w > GC_MAX || h > GC_MAX) return NULL;
    stbtt_MakeGlyphBitmap(&font_info, g->bmp, w, h, w, font_scale, font_scale, gi);
    g->w = w; g->h = h; g->x0 = x0; g->y0 = y0; g->ready = 1;
    return g;
}

static int utf8_next(const char **text) {
    const unsigned char *p = (const unsigned char *)*text;
    int cp;
    if (!*p) return 0;
    if (p[0] < 0x80) cp = p[0], *text += 1;
    else if ((p[0] & 0xe0) == 0xc0 && p[1]) cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f), *text += 2;
    else if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f), *text += 3;
    else if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) cp = ((p[0] & 7) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f), *text += 4;
    else cp = '?', *text += 1;
    return cp;
}

static const stbtt_fontinfo *font_for_cp(int cp, float **scale_out) {
    if (active_font_id == 1 && fallback_loaded) { *scale_out = &fallback_scale; return &fallback_info; }
    if (active_font_id == 2 && latin_loaded) { *scale_out = &latin_scale; return &latin_info; }
    if (stbtt_FindGlyphIndex(&font_info, cp)) { *scale_out = &font_scale; return &font_info; }
    if (load_unicode_fallback() && stbtt_FindGlyphIndex(&fallback_info, cp)) { *scale_out = &fallback_scale; return &fallback_info; }
    if (load_latin_fallback() && stbtt_FindGlyphIndex(&latin_info, cp)) { *scale_out = &latin_scale; return &latin_info; }
    *scale_out = &font_scale;
    return &font_info;
}

static int choose_text_font(const char *text) {
    const char *p = text; int missing = 0;
    while (*p) { if (!stbtt_FindGlyphIndex(&font_info, unicode_upper(utf8_next(&p)))) missing = 1; }
    if (!missing) return 0;
    if (load_unicode_fallback()) {
        p = text; int all = 1; while (*p) if (!stbtt_FindGlyphIndex(&fallback_info, unicode_upper(utf8_next(&p)))) { all = 0; break; }
        if (all) return 1;
    }
    if (load_latin_fallback()) {
        p = text; int all = 1; while (*p) if (!stbtt_FindGlyphIndex(&latin_info, unicode_upper(utf8_next(&p)))) { all = 0; break; }
        if (all) return 2;
    }
    return 0;
}

static int draw_uncached_codepoint(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                                   int cp, uint16_t color, const stbtt_fontinfo *info, float scale) {
    int gi = stbtt_FindGlyphIndex(info, cp), x0, y0, x1, y1;
    if (!gi) return 0;
    stbtt_GetGlyphBitmapBox(info, gi, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 64 || h > 64) return 0;
    unsigned char bmp[64 * 64];
    memset(bmp, 0, sizeof(bmp));
    stbtt_MakeGlyphBitmap(info, bmp, w, h, w, scale, scale, gi);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
    int baseline = (int)(ascent * scale);
    for (int row = 0; row < h; row++) for (int col = 0; col < w; col++) {
        unsigned char a = bmp[row * w + col];
        int px = x + x0 + col, py = y + baseline + y0 + row;
        if (!a || px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;
        if (a >= 255) fb[py * fb_w + px] = color;
        else {
            uint16_t bg = fb[py * fb_w + px]; int ia = 255 - a;
            int fr=(color>>11)&31, fg=(color>>5)&63, fbl=color&31;
            int br=(bg>>11)&31, bgc=(bg>>5)&63, bb=bg&31;
            fb[py*fb_w+px]=(uint16_t)(((fr*a+br*ia)/255<<11)|((fg*a+bgc*ia)/255<<5)|((fbl*a+bb*ia)/255));
        }
    }
    return (int)(stbtt_GetGlyphHMetrics(info, gi, &x0, &y0), x0 * scale);
}

void menu_font_set_px(float pixel_height) {
    if (pixel_height <= 0 || !font_loaded) return;
    if (pixel_height == font_px && gcache_baseline) return;  /* unchanged */
    font_px = pixel_height;
    font_scale = stbtt_ScaleForPixelHeight(&font_info, font_px);
    if (fallback_loaded) fallback_scale = stbtt_ScaleForPixelHeight(&fallback_info, font_px);
    if (latin_loaded) latin_scale = stbtt_ScaleForPixelHeight(&latin_info, font_px);
    gcache_reset();
    /* Pre-warm at this calm moment (once per size change) so no glyph is
     * first-rasterized during a memory-pressured frame. */
    for (char c = GC_FIRST; c <= GC_LAST; c++) gcache_get(c);
}

static void draw_char(uint16_t *fb, int fb_w, int fb_h, int x, int y, char c, uint16_t color) {
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if (!gcache_baseline) gcache_reset();
    gcache_t *g = gcache_get(c);
    if (!g || g->ready == 2 || g->w == 0) return;   /* oversized / space */

    for (int row = 0; row < g->h; row++) {
        for (int col = 0; col < g->w; col++) {
            unsigned char a = g->bmp[row * g->w + col];
            if (!a) continue;
            int px = x + g->x0 + col;
            int py = y + gcache_baseline + g->y0 + row;
            if (px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;
            uint16_t *dst = &fb[py * fb_w + px];
            if (a >= 255) {
                *dst = color;
            } else {
                uint16_t bg = *dst;
                int fr = (color >> 11) & 0x1F, fg = (color >> 5) & 0x3F, fbl = color & 0x1F;
                int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
                int ia = 255 - a;
                int rr = (fr * a + br * ia) / 255;
                int rg = (fg * a + bgc * ia) / 255;
                int rb = (fbl * a + bb * ia) / 255;
                *dst = (uint16_t)((rr << 11) | (rg << 5) | rb);
            }
        }
    }
}

void menu_font_cap_metrics(int *baseline_out, int *cap_height_out) {
    int baseline = 0, cap = 0;
    if (font_loaded) {
        int ascent, descent, line_gap;
        stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
        baseline = (int)(ascent * font_scale);
        int gi = stbtt_FindGlyphIndex(&font_info, 'H');
        int x0, y0, x1, y1;
        if (gi && stbtt_GetGlyphBox(&font_info, gi, &x0, &y0, &x1, &y1))
            cap = (int)((y1 - y0) * font_scale);
        else
            cap = baseline;
    }
    if (baseline_out)   *baseline_out = baseline;
    if (cap_height_out) *cap_height_out = cap;
}

void menu_font_draw_text(uint16_t *fb, int fb_w, int fb_h,
                         int x, int y, const char *text, uint16_t color) {
    if (!font_loaded || !fb || !text) return;
    int prev = 0;
    active_font_id = choose_text_font(text);
    while (*text) {
        int cp = utf8_next(&text);
        cp = unicode_upper(cp);
        float *scale_ptr;
        const stbtt_fontinfo *info = font_for_cp(cp, &scale_ptr);
        int gi = stbtt_FindGlyphIndex(info, cp);
        if (gi != 0) {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
            if (info == &font_info && prev) x += (int)(stbtt_GetGlyphKernAdvance(info, prev, gi) * *scale_ptr);
            if (info == &font_info && cp >= GC_FIRST && cp <= GC_LAST) draw_char(fb, fb_w, fb_h, x, y, (char)cp, color);
            else draw_uncached_codepoint(fb, fb_w, fb_h, x, y, cp, color, info, *scale_ptr);
            x += (int)(adv * *scale_ptr);
            prev = info == &font_info ? gi : 0;
        } else {
            x += MENU_FONT_CHAR_SPACING;
            prev = 0;
        }
    }
    active_font_id = 0;
}

int menu_font_measure(const char *text) {
    if (!font_loaded || !text) return 0;
    int width = 0, prev = 0;
    active_font_id = choose_text_font(text);
    while (*text) {
        int cp = utf8_next(&text);
        cp = unicode_upper(cp);
        float *scale_ptr;
        const stbtt_fontinfo *info = font_for_cp(cp, &scale_ptr);
        int gi = stbtt_FindGlyphIndex(info, cp);
        if (gi != 0) {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
            if (info == &font_info && prev) width += (int)(stbtt_GetGlyphKernAdvance(info, prev, gi) * *scale_ptr);
            width += (int)(adv * *scale_ptr);
            prev = info == &font_info ? gi : 0;
        } else {
            width += MENU_FONT_CHAR_SPACING;
            prev = 0;
        }
    }
    active_font_id = 0;
    return width;
}
