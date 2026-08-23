/* t2top — tui.h: hand-rolled terminal renderer.
 * Cell-based double buffer with diffed output: each flush writes only the
 * cells that changed, with cursor moves and SGR switches minimized. */
#ifndef T2TOP_TUI_H
#define T2TOP_TUI_H

#include <stdbool.h>
#include <stdint.h>
#include "util.h"

#define COL_DEFAULT 0xFFFFFFFFu   /* "terminal default" sentinel */

enum { A_BOLD = 1, A_DIM = 2 };

typedef struct {
    uint32_t ch;    /* Unicode codepoint */
    uint32_t fg, bg;
    uint8_t  attr;
} Cell;

typedef struct {
    int w, h;
    Cell *cells;    /* back buffer (drawn into) */
    Cell *prev;     /* front buffer (what's on screen) */
    char *out;      /* output accumulation buffer */
    size_t out_len, out_cap;
    bool truecolor;
    bool interactive;
    bool force_full;   /* next flush repaints everything */
} Term;

/* interactive=true: raw mode, altscreen, hidden cursor (restored on shutdown).
 * interactive=false: buffers only (for --once). w/h<=0 means autodetect. */
bool term_init(Term *t, bool interactive, int w, int h);
void term_shutdown(Term *t);
bool term_resize(Term *t);                 /* re-detect size */
void term_clear(Term *t);                  /* clear back buffer */
void term_flush(Term *t);                  /* diff-render to stdout */
void term_dump(Term *t);                   /* full render + newlines (--once) */

void put_ch(Term *t, int x, int y, uint32_t ch,
            uint32_t fg, uint32_t bg, uint8_t attr);
/* Writes a UTF-8 string; clips at maxw columns (<0 = no limit) and the
 * terminal edge. Returns columns written. */
int  put_str(Term *t, int x, int y, const char *s, int maxw,
             uint32_t fg, uint8_t attr);
int  put_strf(Term *t, int x, int y, int maxw, uint32_t fg, uint8_t attr,
              const char *fmt, ...);

void draw_box(Term *t, int x, int y, int w, int h,
              const char *title, uint32_t accent);
/* Horizontal gauge using eighth-blocks; track drawn dim. */
void draw_hbar(Term *t, int x, int y, int w, double frac, uint32_t color);
/* Braille sparkline over the last w*2 samples of r, scaled vmin..vmax. */
void draw_spark(Term *t, int x, int y, int w, int h, const Ring *r,
                double vmin, double vmax, uint32_t (*colorfn)(double));

uint32_t lerp_rgb(uint32_t a, uint32_t b, double f);
uint32_t temp_color(double celsius);
uint32_t util_color(double pct);

/* Palette (Tokyo Night-ish; harmonizes with Omarchy's default theme). */
#define C_FG      0xc0caf5u
#define C_DIM     0x565f89u
#define C_CYAN    0x7dcfffu
#define C_BLUE    0x7aa2f7u
#define C_GREEN   0x9ece6au
#define C_YELLOW  0xe0af68u
#define C_RED     0xf7768eu
#define C_MAGENTA 0xbb9af7u
#define C_TEAL    0x73dacau
#define C_ORANGE  0xff9e64u
#define C_NIGHT   0x3d59a1u

#endif
