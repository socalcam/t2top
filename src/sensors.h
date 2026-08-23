/* t2top — sensors.h: everything read from /sys and /proc.
 * Discovery happens once in sensors_init(); sensors_sample() is cheap and
 * called every tick. Every field degrades gracefully: absent hardware sets
 * has_* = false or NAN values, and panels hide accordingly. */
#ifndef T2TOP_SENSORS_H
#define T2TOP_SENSORS_H

#include <stdbool.h>

#define MAX_CPUS  64
#define MAX_TEMPS 80
#define MAX_FANS  8

typedef struct {
    char key[8];      /* SMC 4-char key, e.g. "TC0P" */
    char name[16];    /* decoded human name, or "" */
    double val;       /* celsius */
} TempS;

typedef struct {
    char name[16];
    double rpm, min, max, target;
} FanS;

typedef struct {
    /* -------- static identity -------- */
    char model[64], host[64], kernel[64], cpu_name[64];
    int  ncpu;
    bool has_smc, has_bat, has_amd, has_i915, has_nvme, has_core;

    /* discovered sysfs directories */
    char smc_dir[320], core_dir[320], amd_hwmon[320], amd_dev[320];
    char nvme_dir[320], i915_freq[320];

    /* -------- cpu -------- */
    double cpu_total;            /* 0..100 */
    double cpu_per[MAX_CPUS];
    double ghz_avg, ghz_max;
    double pkg_temp, core_max;

    /* -------- memory (bytes) -------- */
    double mem_total, mem_used, mem_avail, mem_cache;
    double swap_total, swap_used;

    /* -------- SMC wall + fans -------- */
    TempS temps[MAX_TEMPS]; int ntemps;
    FanS  fans[MAX_FANS];   int nfans;

    /* -------- battery -------- */
    double bat_pct, bat_watts;   /* watts: >0 charging, <0 discharging */
    char   bat_status[24];
    double bat_health;           /* 0..100 */
    int    bat_cycles;           /* -1 unknown */
    double bat_temp;             /* NAN unknown */
    double bat_time_h;           /* NAN unknown; to-empty or to-full */
    bool   ac_online;

    /* -------- gpu -------- */
    double amd_busy, amd_membusy;
    double amd_vram_used, amd_vram_total;
    double amd_edge, amd_junction, amd_watts, amd_sclk, amd_mclk;
    double i915_mhz;
    double nvme_temp;

    /* -------- io -------- */
    char   net_if[32];
    char   net_ip[48];
    double net_rx_bps, net_tx_bps, wifi_dbm;
    double dsk_r_bps, dsk_w_bps;
    double root_used, root_total;

    /* -------- system -------- */
    double load1, load5, load15, uptime_s;

    /* -------- internals -------- */
    double prev_t;
    long long pj_tot[MAX_CPUS + 1], pj_idle[MAX_CPUS + 1];
    double prev_rx, prev_tx, prev_rd, prev_wr;
    double ema_amps;             /* smoothed battery current */
    bool   first;
} Sensors;

void sensors_init(Sensors *s);
void sensors_sample(Sensors *s);

#endif
