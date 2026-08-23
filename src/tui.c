#include "tui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
static bool g_raw = false;

/* ---------- output accumulation ---------- */

static void out_reserve(Term *t, size_t extra) {
    if (t->out_len + extra <= t->out_cap) return;
    size_t cap = t->out_cap ? t->out_cap : 8192;
    while (cap < t->out_len + extra) cap *= 2;
    t->out = realloc(t->out, cap);
    t->out_cap = cap;
}

static void out_raw(Term *t, const char *s, size_t n) {
    out_reserve(t, n);
    memcpy(t->out + t->out_len, s, n);
    t->out_len += n;
}

static void out_s(Term *t, const char *s) { out_raw(t, s, strlen(s)); }

static void out_f(Term *t, const char *fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) out_raw(t, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

/* ---------- UTF-8 ---------- */

static void out_utf8(Term *t, uint32_t cp) {
    char b[4];
    int n;
    if      (cp < 0x80)    { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800)   { b[0] = (char)(0xC0 | (cp >> 6));
                             b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12));
                             b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                             b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { b[0] = (char)(0xF0 | (cp >> 18));
                             b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                             b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                             b[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    out_raw(t, b, (size_t)n);
}

/* Decode one UTF-8 sequence; advances *s. Invalid bytes yield U+FFFD. */
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int n;
    if      (p[0] < 0x80)  { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
    else { (*s)++; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return cp;
}

/* ---------- lifecycle ---------- */

static bool detect_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
        return true;
    }
    return false;
}

static void alloc_buffers(Term *t) {
    size_t n = (size_t)t->w * (size_t)t->h;
    t->cells = realloc(t->cells, n * sizeof(Cell));
    t->prev  = realloc(t->prev,  n * sizeof(Cell));
    for (size_t i = 0; i < n; i++) {
        t->cells[i] = (Cell){ ' ', COL_DEFAULT, COL_DEFAULT, 0 };
        t->prev[i]  = (Cell){ 0, 0, 0, 0 };   /* impossible -> full repaint */
    }
    t->force_full = true;
}

bool term_init(Term *t, bool interactive, int w, int h) {
    memset(t, 0, sizeof *t);
    t->interactive = interactive;
    t->truecolor = true;   /* overridable via --256 */
    if (w > 0 && h > 0) { t->w = w; t->h = h; }
    else if (!detect_size(&t->w, &t->h)) { t->w = 110; t->h = 30; }
    alloc_buffers(t);
    if (interactive) {
        if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return false;
        struct termios raw = g_saved;
        raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG);
        raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        g_raw = true;
        const char *enter = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J";
        (void)!write(STDOUT_FILENO, enter, strlen(enter));
    }
    return true;
}

void term_shutdown(Term *t) {
    if (t->interactive) {
        const char *leave = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
        (void)!write(STDOUT_FILENO, leave, strlen(leave));
        if (g_raw) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = false;
    }
    free(t->cells); free(t->prev); free(t->out);
    t->cells = t->prev = NULL; t->out = NULL;
    t->out_len = t->out_cap = 0;
}

bool term_resize(Term *t) {
    int w, h;
    if (!detect_size(&w, &h)) return false;
    if (w == t->w && h == t->h) { t->force_full = true; return true; }
    t->w = w; t->h = h;
    alloc_buffers(t);
    return true;
}

void term_clear(Term *t) {
    size_t n = (size_t)t->w * (size_t)t->h;
    for (size_t i = 0; i < n; i++)
        t->cells[i] = (Cell){ ' ', COL_DEFAULT, COL_DEFAULT, 0 };
}

/* ---------- SGR emission ---------- */

static int to256(uint32_t rgb) {
    int r = (int)((rgb >> 16) & 0xFF) * 6 / 256;
    int g = (int)((rgb >> 8)  & 0xFF) * 6 / 256;
    int b = (int)(rgb & 0xFF) * 6 / 256;
    return 16 + 36 * r + 6 * g + b;
}

static void emit_sgr(Term *t, uint32_t fg, uint32_t bg, uint8_t attr) {
    out_s(t, "\x1b[0");
    if (attr & A_BOLD) out_s(t, ";1");
    if (attr & A_DIM)  out_s(t, ";2");
    if (fg != COL_DEFAULT) {
        if (t->truecolor)
            out_f(t, ";38;2;%u;%u;%u", (fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF);
        else
            out_f(t, ";38;5;%d", to256(fg));
    }
    if (bg != COL_DEFAULT) {
        if (t->truecolor)
            out_f(t, ";48;2;%u;%u;%u", (bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF);
        else
            out_f(t, ";48;5;%d", to256(bg));
    }
    out_s(t, "m");
}

void term_flush(Term *t) {
    t->out_len = 0;
    uint32_t cfg = 1, cbg = 1;   /* impossible values -> first cell emits SGR */
    uint8_t cattr = 0xFF;
    int cx = -99, cy = -99;
    for (int y = 0; y < t->h; y++) {
        for (int x = 0; x < t->w; x++) {
            size_t i = (size_t)y * t->w + x;
            Cell *c = &t->cells[i];
            if (!t->force_full && memcmp(c, &t->prev[i], sizeof(Cell)) == 0)
                continue;
            if (cx != x || cy != y)
                out_f(t, "\x1b[%d;%dH", y + 1, x + 1);
            if (c->fg != cfg || c->bg != cbg || c->attr != cattr) {
                emit_sgr(t, c->fg, c->bg, c->attr);
                cfg = c->fg; cbg = c->bg; cattr = c->attr;
            }
            out_utf8(t, c->ch);
            t->prev[i] = *c;
            cx = x + 1; cy = y;
            if (cx >= t->w) { cx = -99; }   /* wrap disabled: position unknown */
        }
    }
    out_s(t, "\x1b[0m");
    t->force_full = false;
    if (t->out_len)
        (void)!write(STDOUT_FILENO, t->out, t->out_len);
}

void term_dump(Term *t) {
    t->out_len = 0;
    for (int y = 0; y < t->h; y++) {
        uint32_t cfg = 1, cbg = 1; uint8_t cattr = 0xFF;
        for (int x = 0; x < t->w; x++) {
            Cell *c = &t->cells[(size_t)y * t->w + x];
            if (c->fg != cfg || c->bg != cbg || c->attr != cattr) {
                emit_sgr(t, c->fg, c->bg, c->attr);
                cfg = c->fg; cbg = c->bg; cattr = c->attr;
            }
            out_utf8(t, c->ch);
        }
        out_s(t, "\x1b[0m\n");
    }
    (void)!write(STDOUT_FILENO, t->out, t->out_len);
    t->out_len = 0;
}

/* ---------- drawing ---------- */

void put_ch(Term *t, int x, int y, uint32_t ch,
            uint32_t fg, uint32_t bg, uint8_t attr) {
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return;
    t->cells[(size_t)y * t->w + x] = (Cell){ ch, fg, bg, attr };
}

int put_str(Term *t, int x, int y, const char *s, int maxw,
            uint32_t fg, uint8_t attr) {
    int n = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (maxw >= 0 && n >= maxw) break;
        put_ch(t, x + n, y, cp, fg, COL_DEFAULT, attr);
        n++;
    }
    return n;
}

int put_strf(Term *t, int x, int y, int maxw, uint32_t fg, uint8_t attr,
             const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return put_str(t, x, y, buf, maxw, fg, attr);
}

void draw_box(Term *t, int x, int y, int w, int h,
              const char *title, uint32_t accent) {
    if (w < 2 || h < 2) return;
    put_ch(t, x, y, 0x256D, C_DIM, COL_DEFAULT, 0);                 /* ╭ */
    put_ch(t, x + w - 1, y, 0x256E, C_DIM, COL_DEFAULT, 0);         /* ╮ */
    put_ch(t, x, y + h - 1, 0x2570, C_DIM, COL_DEFAULT, 0);         /* ╰ */
    put_ch(t, x + w - 1, y + h - 1, 0x256F, C_DIM, COL_DEFAULT, 0); /* ╯ */
    for (int i = 1; i < w - 1; i++) {
        put_ch(t, x + i, y, 0x2500, C_DIM, COL_DEFAULT, 0);         /* ─ */
        put_ch(t, x + i, y + h - 1, 0x2500, C_DIM, COL_DEFAULT, 0);
    }
    for (int i = 1; i < h - 1; i++) {
        put_ch(t, x, y + i, 0x2502, C_DIM, COL_DEFAULT, 0);         /* │ */
        put_ch(t, x + w - 1, y + i, 0x2502, C_DIM, COL_DEFAULT, 0);
    }
    if (title && *title) {
        int tw = put_strf(t, x + 2, y, w - 4, accent, A_BOLD, " %s ", title);
        (void)tw;
    }
}

void draw_hbar(Term *t, int x, int y, int w, double frac, uint32_t color) {
    if (w <= 0) return;
    frac = clampd(frac, 0.0, 1.0);
    double cells = frac * w;
    int full = (int)cells;
    int rem = (int)((cells - full) * 8.0 + 0.5);   /* eighths */
    for (int i = 0; i < w; i++) {
        if (i < full)
            put_ch(t, x + i, y, 0x2588, color, COL_DEFAULT, 0);           /* █ */
        else if (i == full && rem > 0)
            put_ch(t, x + i, y, 0x2590 - rem, color, COL_DEFAULT, 0);     /* ▏..▉ */
        else
            put_ch(t, x + i, y, 0x2500, C_DIM, COL_DEFAULT, A_DIM);       /* ─ */
    }
}

/* Braille columns: bits for the left half-column, bottom-up. */
static const uint8_t BR_L[5] = { 0x00, 0x40, 0x44, 0x46, 0x47 };
static const uint8_t BR_R[5] = { 0x00, 0x80, 0xA0, 0xB0, 0xB8 };

void draw_spark(Term *t, int x, int y, int w, int h, const Ring *r,
                double vmin, double vmax, uint32_t (*colorfn)(double)) {
    if (w <= 0 || h <= 0 || vmax <= vmin) return;
    int npts = w * 2;
    int maxdots = h * 4;
    for (int cx2 = 0; cx2 < w; cx2++) {
        int dots[2] = { 0, 0 };
        double maxv = NAN;
        for (int s = 0; s < 2; s++) {
            double v = ring_get(r, npts, cx2 * 2 + s);
            if (isnan(v)) continue;
            double f = clampd((v - vmin) / (vmax - vmin), 0.0, 1.0);
            int d = (int)(f * maxdots + 0.5);
            if (v > vmin && d == 0) d = 1;   /* nonzero stays visible */
            dots[s] = d;
            if (isnan(maxv) || v > maxv) maxv = v;
        }
        uint32_t col = isnan(maxv) ? C_DIM : colorfn(maxv);
        for (int row = 0; row < h; row++) {
            int base = (h - 1 - row) * 4;    /* dots consumed below this row */
            int dl = dots[0] - base; if (dl < 0) dl = 0; if (dl > 4) dl = 4;
            int dr = dots[1] - base; if (dr < 0) dr = 0; if (dr > 4) dr = 4;
            uint8_t mask = (uint8_t)(BR_L[dl] | BR_R[dr]);
            uint32_t ch = mask ? 0x2800u + mask : ' ';
            put_ch(t, x + cx2, y + row, ch, col, COL_DEFAULT, 0);
        }
    }
}

/* ---------- color ---------- */

uint32_t lerp_rgb(uint32_t a, uint32_t b, double f) {
    f = clampd(f, 0.0, 1.0);
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    uint32_t r = (uint32_t)(ar + (br - ar) * f);
    uint32_t g = (uint32_t)(ag + (bg - ag) * f);
    uint32_t bl = (uint32_t)(ab + (bb - ab) * f);
    return (r << 16) | (g << 8) | bl;
}

uint32_t temp_color(double c) {
    if (isnan(c)) return C_DIM;
    if (c <= 35) return C_BLUE;
    if (c <= 52) return lerp_rgb(C_BLUE, C_TEAL, (c - 35) / 17.0);
    if (c <= 68) return lerp_rgb(C_TEAL, C_YELLOW, (c - 52) / 16.0);
    if (c <= 84) return lerp_rgb(C_YELLOW, C_RED, (c - 68) / 16.0);
    return C_RED;
}

uint32_t util_color(double p) {
    if (isnan(p)) return C_DIM;
    if (p <= 40) return lerp_rgb(C_CYAN, C_GREEN, p / 40.0);
    if (p <= 70) return lerp_rgb(C_GREEN, C_YELLOW, (p - 40) / 30.0);
    if (p <= 90) return lerp_rgb(C_YELLOW, C_RED, (p - 70) / 20.0);
    return C_RED;
}
