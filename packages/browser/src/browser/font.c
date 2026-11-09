/* TrueType text: the DejaVu fonts that come with the package (/disk/share/browser/fonts), rasterized with
 * stb_truetype into anti-aliased glyphs that are cached per size.  Sans, serif and monospace families with real
 * bold and italic faces, and the fonts' whole Unicode range (Latin, Greek, Cyrillic, Arabic, Hebrew, symbols, ...).
 * Characters the chosen face lacks come from DejaVu Sans; if the font files are missing, the browser falls back
 * to its built-in bitmap font. */
#include "web.h"

/* ---- the bits of a maths library stb_truetype needs (this file is built with SSE) ---- */

static double m_floor(double x)
{
    if (x != x || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    long long i = (long long)x;
    double d = (double)i;
    return d > x ? d - 1 : d;
}
static double m_ceil(double x) { double f = m_floor(x); return f < x ? f + 1 : f; }
static double m_fabs(double x) { return x < 0 ? -x : x; }
static double m_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double m_fmod(double a, double b)
{
    if (b == 0) return 0;
    double q = a / b;
    q = q < 0 ? m_ceil(q) : m_floor(q);
    return a - q * b;
}
static double m_cos(double x)
{
    x = m_fmod(x, 6.283185307179586);
    if (x > 3.141592653589793) x -= 6.283185307179586;
    if (x < -3.141592653589793) x += 6.283185307179586;
    double t = 1, s = 1;
    for (int i = 1; i < 12; i++) { t *= -x * x / ((2 * i - 1) * (2 * i)); s += t; }
    return s;
}
static double m_acos(double x)
{
    /* Newton's method on cos(y) = x, from a linear first guess */
    if (x >= 1) return 0;
    if (x <= -1) return 3.141592653589793;
    double y = 1.5707963267948966 - x;
    for (int i = 0; i < 30; i++) {
        double c = m_cos(y), sn = m_cos(y - 1.5707963267948966);
        if (m_fabs(sn) < 1e-12) break;
        y += (c - x) / sn;
    }
    return y;
}
static double m_pow(double a, double b)
{
    if (b == m_floor(b) && m_fabs(b) < 64) {
        double r = 1;
        long long e = (long long)m_fabs(b);
        while (e--) r *= a;
        return b < 0 ? 1 / r : r;
    }
    if (a <= 0) return 0;
    /* a^b = e^(b ln a), both by series */
    double y = (a - 1) / (a + 1), y2 = y * y, ln = 0, t = y;
    for (int i = 1; i < 80; i += 2) { ln += t / i; t *= y2; }
    double x = 2 * ln * b, term = 1, sum = 1;
    for (int i = 1; i < 40; i++) { term *= x / i; sum += term; }
    return sum;
}

#define STBTT_ifloor(x) ((int)m_floor(x))
#define STBTT_iceil(x)  ((int)m_ceil(x))
#define STBTT_sqrt(x)   m_sqrt(x)
#define STBTT_pow(x, y) m_pow(x, y)
#define STBTT_fmod(x, y) m_fmod(x, y)
#define STBTT_cos(x)    m_cos(x)
#define STBTT_acos(x)   m_acos(x)
#define STBTT_fabs(x)   m_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy    memcpy
#define STBTT_memset    memset
#define STBTT_STATIC
#pragma clang diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define FONT_DIR "/disk/share/browser/fonts/"

enum { F_SANS, F_SANS_B, F_SANS_I, F_SANS_BI, F_SERIF, F_SERIF_B, F_MONO, F_MONO_B, NF };
static const char *const FILES[NF] = {
    "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSans-BoldOblique.ttf",
    "DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf"};

#define ADV_CACHE 0x3000                     /* advances cached for the first 12288 code points */

static struct face {
    int ok;
    u8 *data;
    stbtt_fontinfo info;
    float em1;                               /* scale for a 1 px em */
    u16 *adv;                                /* font units + 1; 0 = not looked up yet */
} F[NF];

static int state;                            /* 0: not tried, 1: ready, -1: no fonts */

static u8 *slurp(const char *path, int *len)
{
    struct stat st;
    if (stat(path, &st) < 0 || st.size <= 0) return NULL;
    u8 *buf = malloc(st.size);
    int fd = open(path, O_RDONLY), n = 0, r;
    if (fd < 0 || !buf) { free(buf); if (fd >= 0) close(fd); return NULL; }
    while (n < (int)st.size && (r = read(fd, buf + n, st.size - n)) > 0) n += r;
    close(fd);
    if (n != (int)st.size) { free(buf); return NULL; }
    *len = n;
    return buf;
}

int font_ready(void)
{
    if (state) return state > 0;
    state = -1;
    for (int i = 0; i < NF; i++) {
        char path[96];
        snprintf(path, sizeof path, "%s%s", FONT_DIR, FILES[i]);
        int len;
        u8 *d = slurp(path, &len);
        if (!d) continue;
        if (!stbtt_InitFont(&F[i].info, d, stbtt_GetFontOffsetForIndex(d, 0))) { free(d); continue; }
        F[i].data = d;
        F[i].em1 = stbtt_ScaleForMappingEmToPixels(&F[i].info, 1.0f);
        F[i].adv = calloc(ADV_CACHE, sizeof(u16));
        F[i].ok = F[i].adv != NULL;
    }
    if (F[F_SANS].ok) state = 1;
    return state > 0;
}

static int face_for(int flags)
{
    int want;
    if (flags & CF_MONO) want = flags & CF_BOLD ? F_MONO_B : F_MONO;
    else if (flags & CF_SERIF) want = flags & CF_BOLD ? F_SERIF_B : F_SERIF;
    else if ((flags & CF_BOLD) && (flags & CF_ITALIC)) want = F_SANS_BI;
    else if (flags & CF_BOLD) want = F_SANS_B;
    else if (flags & CF_ITALIC) want = F_SANS_I;
    else want = F_SANS;
    if (F[want].ok) return want;
    if ((flags & CF_BOLD) && F[F_SANS_B].ok) return F_SANS_B;
    return F_SANS;
}

static int invisible(int cp)
{
    return cp == 0xAD || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF || cp == '\n' || cp == '\r';
}

/* The face that has this character (falling back to Sans), and its glyph index. */
static int face_of_char(int cp, int flags, int *glyph)
{
    int f = face_for(flags);
    int g = stbtt_FindGlyphIndex(&F[f].info, cp);
    if (!g && f != F_SANS) {
        int g2 = stbtt_FindGlyphIndex(&F[F_SANS].info, cp);
        if (g2) { f = F_SANS; g = g2; }
    }
    if (!g) g = stbtt_FindGlyphIndex(&F[f].info, 0xFFFD);       /* the replacement character */
    *glyph = g;
    return f;
}

int font_adv16(int cp, int px, int flags)
{
    if (invisible(cp)) return 0;
    if (cp == '\t') cp = ' ';
    int f = face_for(flags), units;
    if (cp < ADV_CACHE && F[f].adv[cp]) {
        units = F[f].adv[cp] - 1;
    } else {
        int g, adv, lsb;
        int ff = face_of_char(cp, flags, &g);
        stbtt_GetGlyphHMetrics(&F[ff].info, g, &adv, &lsb);
        units = (int)(adv * F[ff].em1 / F[f].em1);               /* in the main face's units */
        if (cp < ADV_CACHE && units < 65534) F[f].adv[cp] = units + 1;
    }
    return (int)(units * F[f].em1 * px * 16 + 0.5f);
}

/* ---- glyph bitmaps, cached ---- */

struct gl {
    u32 key;
    u8 face;
    u8 *a;
    short w, h, xoff, yoff;
};

#define NG 4096
static struct gl cache[NG];
static int ncached;

static void flush(void)
{
    for (int i = 0; i < NG; i++) {
        free(cache[i].a);
        cache[i].a = NULL;
        cache[i].key = 0;
    }
    ncached = 0;
}

static struct gl *glyph(int cp, int px, int flags)
{
    int f = face_for(flags);
    u32 key = ((u32)cp << 11) ^ ((u32)px << 3) ^ (u32)f ^ 0x80000000u;
    u32 h = (key * 2654435761u) >> 20;
    for (int probe = 0; probe < NG; probe++, h = (h + 1) % NG) {
        struct gl *e = &cache[h];
        if (e->key == key && e->face == f) return e;
        if (e->key) continue;
        if (ncached > NG * 3 / 4) { flush(); return glyph(cp, px, flags); }
        int g;
        int ff = face_of_char(cp, flags, &g);
        float scale = F[ff].em1 * px;
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&F[ff].info, g, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, ht = y1 - y0;
        e->key = key;
        e->face = f;
        e->w = w > 0 ? w : 0;
        e->h = ht > 0 ? ht : 0;
        e->xoff = x0;
        e->yoff = y0;
        e->a = NULL;
        if (e->w && e->h && e->w < 512 && e->h < 512) {
            e->a = malloc(e->w * e->h);
            if (e->a) stbtt_MakeGlyphBitmap(&F[ff].info, e->a, e->w, e->h, e->w, scale, scale, g);
        }
        ncached++;
        return e;
    }
    return NULL;
}

int utf8_next(const char **ps, const char *e);

/* (x, y) is the top of the text box; the baseline sits where layout expects it, at 4/5 of the size. */
void font_draw(int x, int y, const char *s, int n, int px, u32 color, int flags)
{
    const char *e = s + n;
    int x16 = 0, base = y + px * 4 / 5;
    while (s < e) {
        int cp = utf8_next(&s, e);
        if (invisible(cp)) continue;
        if (cp != ' ' && cp != '\t' && cp != 0xA0) {
            struct gl *g = glyph(cp, px, flags);
            if (g && g->a)
                gfx_blit_a8(g->a, x + (x16 + 8) / 16 + g->xoff, base + g->yoff, g->w, g->h, color);
        }
        x16 += font_adv16(cp, px, flags);
    }
}
