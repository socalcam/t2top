/* t2top — hardware telemetry for Macs running Linux.
 * Decodes the Apple SMC sensor wall, fans, battery health, and the rest of
 * the machine into one dependency-free terminal dashboard. */
#include "sensors.h"
#include "tui.h"
#include "util.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#ifndef T2TOP_VERSION
#define T2TOP_VERSION "dev"
#endif

static volatile sig_atomic_t g_quit = 0, g_winch = 0;
static void on_quit(int sig)  { (void)sig; g_quit = 1; }
static void on_winch(int sig) { (void)sig; g_winch = 1; }

typedef struct {
    bool fahrenheit, paused;
    int iidx;
} UI;

static const int INTERVALS[] = { 200, 300, 500, 750, 1000, 2000, 3000, 5000 };
#define NINTERVALS ((int)(sizeof INTERVALS / sizeof *INTERVALS))

static Ring R_cpu, R_gpu, R_rx, R_tx, R_rd, R_wr;

static double deg(const UI *ui, double c) {
    return ui->fahrenheit ? c * 9.0 / 5.0 + 32.0 : c;
}

/* fixed-color spark callbacks */
static uint32_t col_teal(double v)    { (void)v; return C_TEAL; }
static uint32_t col_magenta(double v) { (void)v; return C_MAGENTA; }
static uint32_t col_blue(double v)    { (void)v; return C_BLUE; }
static uint32_t col_orange(double v)  { (void)v; return C_ORANGE; }

/* ---------------- panels ---------------- */

static void draw_header(Term *t, const Sensors *s) {
    int x = 1;
    x += put_str(t, x, 0, " t2top ", -1, C_MAGENTA, A_BOLD);
    x += put_strf(t, x, 0, -1, C_CYAN, 0, "%s", s->model);
    x += put_str(t, x, 0, " · ", -1, C_DIM, 0);
    x += put_strf(t, x, 0, -1, C_FG, 0, "%s", s->host);
    x += put_str(t, x, 0, " · ", -1, C_DIM, 0);
    char up[16], right[96], clock[16];
    fmt_hours(s->uptime_s / 3600.0, up, sizeof up);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(clock, sizeof clock, "%H:%M:%S", &tm);
    snprintf(right, sizeof right, "up %s \xc2\xb7 load %.2f %.2f %.2f \xc2\xb7 %s ",
             up, s->load1, s->load5, s->load15, clock);
    int rx = t->w - 1 - (int)(strlen(right) - 4);   /* two UTF-8 dots */
    put_strf(t, x, 0, rx - x - 1, C_DIM, 0, "%s", s->kernel);
    put_str(t, rx, 0, right, -1, C_DIM, 0);
}

static void draw_footer(Term *t, const UI *ui) {
    int y = t->h - 1;
    int x = 1;
    if (ui->paused)
        x += put_str(t, x, y, " PAUSED ", -1, C_YELLOW, A_BOLD);
    x += put_strf(t, x, y, -1, C_DIM, 0,
                  " q quit · p pause · +/- rate %dms · f %s",
                  INTERVALS[ui->iidx], ui->fahrenheit ? "°F" : "°C");
    const char *v = " t2top " T2TOP_VERSION " ";
    put_str(t, t->w - 1 - (int)strlen(v) - 1, y, v, -1, C_DIM, 0);
}

static void draw_cpu(Term *t, const Sensors *s, const UI *ui,
                     int x, int y, int w, int h) {
    char title[96];
    snprintf(title, sizeof title, "CPU · %s · %d threads",
             s->cpu_name[0] ? s->cpu_name : "unknown", s->ncpu);
    draw_box(t, x, y, w, h, title, C_CYAN);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    int core_rows = (s->ncpu * 2 + iw - 1) / iw;
    if (core_rows < 1) core_rows = 1;
    int spark_h = ih - core_rows - 1;
    if (spark_h < 1) spark_h = 1;
    draw_spark(t, ix, iy, iw, spark_h, &R_cpu, 0, 100, util_color);
    /* per-core eighth-block bars */
    static const uint32_t EIGHTS[9] =
        { ' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588 };
    int bx = ix, by = iy + spark_h;
    for (int i = 0; i < s->ncpu; i++) {
        if (bx + 1 >= ix + iw) { bx = ix; by++; }
        double p = s->cpu_per[i];
        int lvl = (int)(clampd(p, 0, 100) / 100.0 * 8.0 + 0.5);
        put_ch(t, bx, by, lvl ? EIGHTS[lvl] : 0x2581,
               lvl ? util_color(p) : C_NIGHT, COL_DEFAULT, 0);
        bx += 2;
    }
    int liy = iy + ih - 1;
    int lx = ix;
    lx += put_strf(t, lx, liy, iw, C_FG, A_BOLD, "%3.0f%%", s->cpu_total);
    lx += put_strf(t, lx, liy, -1, C_DIM, 0, " · ");
    lx += put_strf(t, lx, liy, -1, C_FG, 0, "%.2f GHz", s->ghz_avg);
    lx += put_strf(t, lx, liy, -1, C_DIM, 0, " avg · ");
    lx += put_strf(t, lx, liy, -1, C_FG, 0, "%.2f", s->ghz_max);
    lx += put_strf(t, lx, liy, -1, C_DIM, 0, " max");
    if (!isnan(s->pkg_temp)) {
        lx += put_strf(t, lx, liy, -1, C_DIM, 0, " · pkg ");
        lx += put_strf(t, lx, liy, -1, temp_color(s->pkg_temp), A_BOLD,
                       "%.0f°", deg(ui, s->pkg_temp));
    }
    if (!isnan(s->core_max)) {
        lx += put_strf(t, lx, liy, -1, C_DIM, 0, " · hot core ");
        put_strf(t, lx, liy, -1, temp_color(s->core_max), A_BOLD,
                 "%.0f°", deg(ui, s->core_max));
    }
}

static void draw_wall(Term *t, const Sensors *s, const UI *ui,
                      int x, int y, int w, int h) {
    char title[48];
    snprintf(title, sizeof title, "SMC · %d sensors", s->ntemps);
    draw_box(t, x, y, w, h, s->has_smc ? title : "SMC", C_MAGENTA);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (!s->has_smc) {
        put_str(t, ix, iy, "no applesmc — not a T2 Mac?", iw, C_DIM, 0);
        return;
    }
    int cols = iw / 19;
    if (cols < 1) cols = 1;
    int colw = iw / cols;
    int cap = cols * ih;
    int n = s->ntemps <= cap ? s->ntemps : cap - 1;
    for (int i = 0; i < n; i++) {
        int cx = ix + (i / ih) * colw;
        int cy = iy + (i % ih);
        const TempS *tp = &s->temps[i];
        put_str(t, cx, cy, tp->key, 4, C_DIM, 0);
        put_strf(t, cx + 5, cy, 4, temp_color(tp->val), A_BOLD,
                 "%3.0f", deg(ui, tp->val));
        put_ch(t, cx + 8, cy, 0x00B0, C_DIM, COL_DEFAULT, 0);   /* ° */
        if (colw >= 15)
            put_str(t, cx + 10, cy, tp->name, colw - 10, C_FG, 0);
    }
    if (s->ntemps > cap)
        put_strf(t, ix + ((cap - 1) / ih) * colw, iy + ((cap - 1) % ih),
                 colw - 1, C_DIM, 0, "+%d more", s->ntemps - n);
}

static void draw_gpu(Term *t, const Sensors *s, const UI *ui,
                     int x, int y, int w, int h) {
    draw_box(t, x, y, w, h, "GPU", C_GREEN);
    int ix = x + 2, iw = w - 4, iy = y + 1;
    if (!s->has_amd && !s->has_i915) {
        put_str(t, ix, iy, "no GPU telemetry", iw, C_DIM, 0);
        return;
    }
    if (s->has_amd) {
        put_str(t, ix, iy, "busy", 5, C_DIM, 0);
        int sw = iw - 11;
        if (sw > 0) draw_spark(t, ix + 5, iy, sw, 1, &R_gpu, 0, 100, util_color);
        put_strf(t, ix + iw - 5, iy, 5, util_color(s->amd_busy), A_BOLD,
                 "%4.0f%%", s->amd_busy);
        char used[12], tot[12];
        fmt_size(s->amd_vram_used, used, sizeof used);
        fmt_size(s->amd_vram_total, tot, sizeof tot);
        put_str(t, ix, iy + 1, "vram", 5, C_DIM, 0);
        int barw = iw - 5 - 11;
        if (barw > 0 && s->amd_vram_total > 0)
            draw_hbar(t, ix + 5, iy + 1, barw,
                      s->amd_vram_used / s->amd_vram_total, C_GREEN);
        put_strf(t, ix + iw - 10, iy + 1, 10, C_FG, 0, "%s/%s", used, tot);
        int lx = ix;
        lx += put_strf(t, lx, iy + 2, -1, temp_color(s->amd_edge), A_BOLD,
                       "%.0f°", deg(ui, s->amd_edge));
        lx += put_str(t, lx, iy + 2, " edge", -1, C_DIM, 0);
        if (!isnan(s->amd_junction)) {
            lx += put_strf(t, lx, iy + 2, -1, temp_color(s->amd_junction), A_BOLD,
                           " %.0f°", deg(ui, s->amd_junction));
            lx += put_str(t, lx, iy + 2, " junc", -1, C_DIM, 0);
        }
        if (!isnan(s->amd_watts))
            put_strf(t, lx, iy + 2, iw - (lx - ix), C_FG, 0, " · %.1fW", s->amd_watts);
        put_strf(t, ix, iy + 3, iw, C_DIM, 0, "sclk %.0f · mclk %.0f MHz",
                 s->amd_sclk, s->amd_mclk);
    }
    if (s->has_i915)
        put_strf(t, ix, iy + (s->has_amd ? 4 : 0), iw, C_DIM, 0,
                 "iGPU %.0f MHz", s->i915_mhz);
}

static void draw_fans(Term *t, const Sensors *s, int x, int y, int w, int h) {
    draw_box(t, x, y, w, h, "Fans", C_BLUE);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (s->nfans == 0) {
        put_str(t, ix, iy, "no fan telemetry", iw, C_DIM, 0);
        return;
    }
    for (int i = 0; i < s->nfans && i * 2 + 1 < ih + 1; i++) {
        const FanS *fn = &s->fans[i];
        int ly = iy + i * 2;
        int lx = ix;
        lx += put_strf(t, lx, ly, -1, C_FG, A_BOLD, "%s", fn->name);
        lx += put_strf(t, lx, ly, -1, C_FG, 0, "  %4.0f", fn->rpm);
        lx += put_str(t, lx, ly, " rpm", -1, C_DIM, 0);
        if (!isnan(fn->target) && fabs(fn->target - fn->rpm) > 1)
            put_strf(t, lx, ly, iw - (lx - ix), C_DIM, 0, " → %.0f", fn->target);
        if (ly + 1 < iy + ih && !isnan(fn->min) && fn->max > fn->min) {
            double frac = (fn->rpm - fn->min) / (fn->max - fn->min);
            draw_hbar(t, ix, ly + 1, iw, clampd(frac, 0, 1),
                      util_color(frac * 100.0));
        }
    }
}

static void draw_batt(Term *t, const Sensors *s, const UI *ui,
                      int x, int y, int w, int h) {
    draw_box(t, x, y, w, h, "Battery", C_YELLOW);
    int ix = x + 2, iw = w - 4, iy = y + 1;
    if (!s->has_bat) {
        put_str(t, ix, iy, "no battery", iw, C_DIM, 0);
        return;
    }
    bool chg = strcmp(s->bat_status, "Charging") == 0;
    uint32_t pcol = chg ? C_TEAL :
                    s->bat_pct > 50 ? C_GREEN :
                    s->bat_pct > 20 ? C_YELLOW : C_RED;
    int lx = ix + put_strf(t, ix, iy, -1, pcol, A_BOLD, "%3.0f%% ", s->bat_pct);
    if (iw - (lx - ix) > 4)
        draw_hbar(t, lx, iy, iw - (lx - ix), s->bat_pct / 100.0, pcol);
    if (!isnan(s->bat_watts)) {
        uint32_t wcol = s->bat_watts < 0 ? C_ORANGE : C_TEAL;
        lx = ix + put_strf(t, ix, iy + 1, -1, wcol, A_BOLD, "%+.1fW", s->bat_watts);
        put_strf(t, lx, iy + 1, iw - (lx - ix), C_DIM, 0, " · %s", s->bat_status);
    } else {
        put_strf(t, ix, iy + 1, iw, C_DIM, 0, "%s", s->bat_status);
    }
    char tm[16];
    fmt_hours(s->bat_time_h, tm, sizeof tm);
    if (!isnan(s->bat_time_h))
        put_strf(t, ix, iy + 2, iw, C_FG, 0, "%s %s", tm,
                 chg ? "to full" : "remaining");
    int hy = iy + 3;
    if (!isnan(s->bat_health)) {
        lx = ix + put_str(t, ix, hy, "health ", -1, C_DIM, 0);
        lx += put_strf(t, lx, hy, -1,
                       s->bat_health > 80 ? C_GREEN : C_YELLOW, A_BOLD,
                       "%.0f%%", s->bat_health);
        if (s->bat_cycles >= 0)
            put_strf(t, lx, hy, iw - (lx - ix), C_DIM, 0,
                     " · %d cycles", s->bat_cycles);
    }
    lx = ix + put_strf(t, ix, iy + 4, -1,
                       s->ac_online ? C_GREEN : C_DIM, 0,
                       s->ac_online ? "AC online" : "on battery");
    if (!isnan(s->bat_temp))
        put_strf(t, lx, iy + 4, iw - (lx - ix), C_DIM, 0,
                 " · cell %.0f°", deg(ui, s->bat_temp));
}

static void draw_mem(Term *t, const Sensors *s, int x, int y, int w, int h) {
    draw_box(t, x, y, w, h, "Memory", C_TEAL);
    int ix = x + 2, iw = w - 4, iy = y + 1;
    char a[12], b[12];
    fmt_size(s->mem_used, a, sizeof a);
    fmt_size(s->mem_total, b, sizeof b);
    put_str(t, ix, iy, "used", 5, C_DIM, 0);
    int barw = iw - 5 - 11;
    double frac = s->mem_total > 0 ? s->mem_used / s->mem_total : 0;
    if (barw > 0) draw_hbar(t, ix + 5, iy, barw, frac, util_color(frac * 100));
    put_strf(t, ix + iw - 10, iy, 10, C_FG, 0, "%s/%s", a, b);
    fmt_size(s->mem_avail, a, sizeof a);
    fmt_size(s->mem_cache, b, sizeof b);
    put_strf(t, ix, iy + 1, iw, C_DIM, 0, "avail %s · cache %s", a, b);
    fmt_size(s->swap_used, a, sizeof a);
    fmt_size(s->swap_total, b, sizeof b);
    put_strf(t, ix, iy + 2, iw, C_DIM, 0, "swap  %s/%s", a, b);
}

static void draw_disk(Term *t, const Sensors *s, const UI *ui,
                      int x, int y, int w, int h) {
    draw_box(t, x, y, w, h, "Disk", C_ORANGE);
    int ix = x + 2, iw = w - 4, iy = y + 1;
    double mx = ring_max(&R_rd, 40);
    double m2 = ring_max(&R_wr, 40);
    if (m2 > mx) mx = m2;
    if (mx < 1e6) mx = 1e6;
    char v[12];
    fmt_size(s->dsk_r_bps, v, sizeof v);
    put_str(t, ix, iy, "read ", 6, C_DIM, 0);
    draw_spark(t, ix + 6, iy, iw - 14, 1, &R_rd, 0, mx, col_blue);
    put_strf(t, ix + iw - 7, iy, 7, C_BLUE, 0, "%5s/s", v);
    fmt_size(s->dsk_w_bps, v, sizeof v);
    put_str(t, ix, iy + 1, "write", 6, C_DIM, 0);
    draw_spark(t, ix + 6, iy + 1, iw - 14, 1, &R_wr, 0, mx, col_orange);
    put_strf(t, ix + iw - 7, iy + 1, 7, C_ORANGE, 0, "%5s/s", v);
    int lx = ix;
    if (!isnan(s->nvme_temp)) {
        lx += put_str(t, lx, iy + 2, "nvme ", -1, C_DIM, 0);
        lx += put_strf(t, lx, iy + 2, -1, temp_color(s->nvme_temp), A_BOLD,
                       "%.0f°", deg(ui, s->nvme_temp));
    }
    char u[12], tt[12];
    fmt_size(s->root_used, u, sizeof u);
    fmt_size(s->root_total, tt, sizeof tt);
    put_strf(t, ix, iy + 3, iw, C_DIM, 0, "root %s/%s (%.0f%%)", u, tt,
             s->root_total > 0 ? 100.0 * s->root_used / s->root_total : 0);
}

static void draw_net(Term *t, const Sensors *s, int x, int y, int w, int h) {
    draw_box(t, x, y, w, h, "Network", C_MAGENTA);
    int ix = x + 2, iw = w - 4, iy = y + 1;
    double mx = ring_max(&R_rx, 40);
    double m2 = ring_max(&R_tx, 40);
    if (m2 > mx) mx = m2;
    if (mx < 125e3) mx = 125e3;   /* 1 Mbit floor */
    char v[12];
    fmt_size(s->net_rx_bps, v, sizeof v);
    put_str(t, ix, iy, "down ", 6, C_DIM, 0);
    draw_spark(t, ix + 6, iy, iw - 14, 1, &R_rx, 0, mx, col_teal);
    put_strf(t, ix + iw - 7, iy, 7, C_TEAL, 0, "%5s/s", v);
    fmt_size(s->net_tx_bps, v, sizeof v);
    put_str(t, ix, iy + 1, "up   ", 6, C_DIM, 0);
    draw_spark(t, ix + 6, iy + 1, iw - 14, 1, &R_tx, 0, mx, col_magenta);
    put_strf(t, ix + iw - 7, iy + 1, 7, C_MAGENTA, 0, "%5s/s", v);
    int lx = ix;
    lx += put_strf(t, lx, iy + 2, iw, C_FG, 0, "%s",
                   s->net_if[0] ? s->net_if : "no route");
    if (!isnan(s->wifi_dbm))
        put_strf(t, lx, iy + 2, iw - (lx - ix), C_DIM, 0,
                 " · %.0f dBm", s->wifi_dbm);
    if (s->net_ip[0])
        put_str(t, ix, iy + 3, s->net_ip, iw, C_DIM, 0);
}

/* ---------------- frame ---------------- */

static void draw_frame(Term *t, const Sensors *s, const UI *ui) {
    term_clear(t);
    if (t->w < 96 || t->h < 24) {
        put_strf(t, 2, 1, -1, C_YELLOW, A_BOLD, "t2top needs at least 96x24");
        put_strf(t, 2, 2, -1, C_DIM, 0, "current: %dx%d", t->w, t->h);
        return;
    }
    draw_header(t, s);
    draw_footer(t, ui);

    int wallw = t->w * 34 / 100;
    wallw = wallw < 41 ? 41 : (wallw > 62 ? 62 : wallw);
    int leftw = t->w - wallw;
    int extra = (t->h - 2) - 22;
    int dB = extra / 6 > 3 ? 3 : extra / 6;
    int dC = dB;
    int ah = 9 + (extra - dB - dC), bh = 7 + dB, ch = (t->h - 2) - ah - bh;

    draw_cpu(t, s, ui, 0, 1, leftw, ah);
    int bw = leftw / 3;
    draw_gpu(t, s, ui, 0, 1 + ah, bw, bh);
    draw_fans(t, s, bw, 1 + ah, bw, bh);
    draw_batt(t, s, ui, 2 * bw, 1 + ah, leftw - 2 * bw, bh);
    int cw = leftw / 3;
    draw_mem(t, s, 0, 1 + ah + bh, cw, ch);
    draw_disk(t, s, ui, cw, 1 + ah + bh, cw, ch);
    draw_net(t, s, 2 * cw, 1 + ah + bh, leftw - 2 * cw, ch);
    draw_wall(t, s, ui, leftw, 1, t->w - leftw, t->h - 2);
}

static void push_rings(const Sensors *s) {
    ring_push(&R_cpu, s->cpu_total);
    ring_push(&R_gpu, isnan(s->amd_busy) ? 0 : s->amd_busy);
    ring_push(&R_rx, s->net_rx_bps);
    ring_push(&R_tx, s->net_tx_bps);
    ring_push(&R_rd, s->dsk_r_bps);
    ring_push(&R_wr, s->dsk_w_bps);
}

/* ---------------- modes ---------------- */

static int run_list(void) {
    Sensors s;
    sensors_init(&s);
    sensors_sample(&s);
    msleep(300);
    sensors_sample(&s);
    printf("t2top %s — %s · %s · %s\n\n", T2TOP_VERSION, s.model, s.host, s.kernel);
    printf("cpu    : %s, %d threads, %.0f%%, %.2f GHz avg, pkg %.0f°C\n",
           s.cpu_name, s.ncpu, s.cpu_total, s.ghz_avg, s.pkg_temp);
    printf("memory : %.1f/%.1f GiB used\n",
           s.mem_used / 1073741824.0, s.mem_total / 1073741824.0);
    for (int i = 0; i < s.nfans; i++)
        printf("fan    : %-10s %5.0f rpm (min %.0f, max %.0f, target %.0f)\n",
               s.fans[i].name, s.fans[i].rpm, s.fans[i].min, s.fans[i].max,
               s.fans[i].target);
    if (s.has_bat)
        printf("battery: %.0f%% %s, %+.1fW, health %.0f%%, %d cycles\n",
               s.bat_pct, s.bat_status, s.bat_watts, s.bat_health, s.bat_cycles);
    if (s.has_amd)
        printf("gpu    : busy %.0f%%, vram %.0f/%.0f MiB, edge %.0f°C, %.1fW\n",
               s.amd_busy, s.amd_vram_used / 1048576.0,
               s.amd_vram_total / 1048576.0, s.amd_edge, s.amd_watts);
    printf("smc    : %d sensors\n", s.ntemps);
    for (int i = 0; i < s.ntemps; i++)
        printf("  %-4s %6.1f°C  %s\n", s.temps[i].key, s.temps[i].val,
               s.temps[i].name[0] ? s.temps[i].name : "-");
    return 0;
}

static int run_once(int w, int h) {
    Sensors s;
    sensors_init(&s);
    sensors_sample(&s);
    msleep(300);
    sensors_sample(&s);
    push_rings(&s);
    UI ui = { 0 };
    ui.iidx = 2;
    Term t;
    term_init(&t, false, w, h);
    draw_frame(&t, &s, &ui);
    term_dump(&t);
    term_shutdown(&t);
    return 0;
}

int main(int argc, char **argv) {
    int once = 0, ow = 0, oh = 0, use256 = 0;
    UI ui = { 0 };
    ui.iidx = 2;   /* 500ms */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--list")) return run_list();
        else if (!strcmp(argv[i], "--256")) use256 = 1;
        else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fahrenheit"))
            ui.fahrenheit = true;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &ow, &oh) != 2) { ow = oh = 0; }
        } else if (!strcmp(argv[i], "--interval") && i + 1 < argc) {
            int ms = atoi(argv[++i]);
            int best = 0;
            for (int k = 0; k < NINTERVALS; k++)
                if (abs(INTERVALS[k] - ms) < abs(INTERVALS[best] - ms)) best = k;
            ui.iidx = best;
        } else if (!strcmp(argv[i], "--version")) {
            printf("t2top %s\n", T2TOP_VERSION);
            return 0;
        } else {
            printf("t2top %s — hardware telemetry for Macs running Linux\n\n"
                   "usage: t2top [options]\n"
                   "  --once           render one frame and exit\n"
                   "  --list           plain-text dump of discovered sensors\n"
                   "  --size WxH       override terminal size (with --once)\n"
                   "  --interval MS    sampling interval (default 500)\n"
                   "  -f, --fahrenheit temperatures in °F\n"
                   "  --256            256-color mode (default: truecolor)\n"
                   "  --version        print version\n\n"
                   "keys: q quit · p pause · +/- sampling rate · f °C/°F\n",
                   T2TOP_VERSION);
            return strcmp(argv[i], "--help") && strcmp(argv[i], "-h") ? 1 : 0;
        }
    }
    if (once) return run_once(ow, oh);

    struct sigaction sa = { 0 };
    sa.sa_handler = on_quit;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);

    Sensors s;
    sensors_init(&s);
    sensors_sample(&s);

    Term t;
    if (!term_init(&t, true, 0, 0)) {
        fprintf(stderr, "t2top: not a terminal\n");
        return 1;
    }
    if (use256) t.truecolor = false;

    double next = now_ms();
    while (!g_quit) {
        if (g_winch) { g_winch = 0; term_resize(&t); }
        double now = now_ms();
        if (now >= next) {
            if (!ui.paused) {
                sensors_sample(&s);
                push_rings(&s);
            }
            draw_frame(&t, &s, &ui);
            term_flush(&t);
            next = now + INTERVALS[ui.iidx];
        }
        double wait = next - now_ms();
        if (wait < 0) wait = 0;
        struct timeval tv = { (time_t)(wait / 1000),
                              (suseconds_t)((long)(wait * 1000) % 1000000) };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            char c;
            while (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'q' || c == 'Q' || c == 3) g_quit = 1;
                else if (c == 'p' || c == 'P') ui.paused = !ui.paused;
                else if (c == '+' || c == '=') { if (ui.iidx > 0) ui.iidx--; next = 0; }
                else if (c == '-' || c == '_') { if (ui.iidx < NINTERVALS - 1) ui.iidx++; next = 0; }
                else if (c == 'f' || c == 'F') { ui.fahrenheit = !ui.fahrenheit; next = 0; }
            }
        }
    }
    term_shutdown(&t);
    return 0;
}
