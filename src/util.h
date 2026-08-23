/* t2top — util.h: small helpers shared by every layer. */
#ifndef T2TOP_UTIL_H
#define T2TOP_UTIL_H

#include <stdbool.h>
#include <stddef.h>

double now_ms(void);
void   msleep(int ms);

/* Read the first line of a file, trimmed of trailing whitespace.
 * Returns false (and buf[0]='\0') if the file can't be read. */
bool read_str(const char *path, char *buf, size_t n);

/* Read a numeric file. Returns NAN on failure. */
double read_num(const char *path);

bool file_exists(const char *path);

/* Fixed-capacity history ring for sparklines. */
#define RING_CAP 512
typedef struct {
    double v[RING_CAP];
    int head;   /* index of next write */
    int len;    /* count of valid samples, <= RING_CAP */
} Ring;

void   ring_push(Ring *r, double x);
/* Value i (0 = oldest) of the window covering the last n pushes.
 * NAN where the window predates recorded history. */
double ring_get(const Ring *r, int n, int i);
double ring_max(const Ring *r, int n);

double clampd(double x, double lo, double hi);

/* "999K" / "12.3M" / "1.9G" — 4-5 chars, no trailing 'B'. */
void fmt_size(double bytes, char *out, size_t n);
/* "h:mm" from hours. */
void fmt_hours(double h, char *out, size_t n);

#endif
