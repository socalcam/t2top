#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

bool read_str(const char *path, char *buf, size_t n) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (!ok) { buf[0] = '\0'; return false; }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                       buf[len - 1] == ' '  || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    return true;
}

double read_num(const char *path) {
    char buf[64];
    if (!read_str(path, buf, sizeof buf)) return NAN;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return NAN;
    return v;
}

bool file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

void ring_push(Ring *r, double x) {
    r->v[r->head] = x;
    r->head = (r->head + 1) % RING_CAP;
    if (r->len < RING_CAP) r->len++;
}

double ring_get(const Ring *r, int n, int i) {
    if (n > RING_CAP) n = RING_CAP;
    int age = n - 1 - i;              /* 0 = newest */
    if (age < 0 || age >= r->len) return NAN;
    int idx = (r->head - 1 - age + 2 * RING_CAP) % RING_CAP;
    return r->v[idx];
}

double ring_max(const Ring *r, int n) {
    double m = 0;
    for (int i = 0; i < n; i++) {
        double v = ring_get(r, n, i);
        if (!isnan(v) && v > m) m = v;
    }
    return m;
}

double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

void fmt_size(double b, char *out, size_t n) {
    const char *u = "B";
    double v = b;
    const double K = 1024.0;
    if      (v >= K * K * K * K) { v /= K * K * K * K; u = "T"; }
    else if (v >= K * K * K)     { v /= K * K * K;     u = "G"; }
    else if (v >= K * K)         { v /= K * K;         u = "M"; }
    else if (v >= K)             { v /= K;             u = "K"; }
    if (v >= 100 || (v - floor(v)) < 0.05)
        snprintf(out, n, "%.0f%s", v, u);
    else
        snprintf(out, n, "%.1f%s", v, u);
}

void fmt_hours(double h, char *out, size_t n) {
    if (isnan(h) || h < 0 || h > 99) { snprintf(out, n, "--:--"); return; }
    int hh = (int)h;
    int mm = (int)((h - hh) * 60.0 + 0.5);
    if (mm == 60) { hh++; mm = 0; }
    snprintf(out, n, "%d:%02d", hh, mm);
}
