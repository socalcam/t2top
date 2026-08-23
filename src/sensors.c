#include "sensors.h"
#include "util.h"

#include <dirent.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

/* ---------- SMC key decoding ----------
 * Apple's SMC exposes temperatures under terse 4-char keys. The kernel's
 * applesmc driver passes them through as tempN_label; this table maps the
 * keys seen on T2-era MacBooks to short human names. Unknown keys still
 * display — just without a name. */
static const struct { const char *key, *name; } SMC_NAMES[] = {
    { "TA0P", "Ambient" },   { "TA0V", "Ambient" },   { "TA1P", "Ambient 2" },
    { "TB0T", "Battery" },   { "TB1T", "Battery 1" }, { "TB2T", "Battery 2" },
    { "TC0P", "CPU prox" },  { "TC0E", "CPU die" },   { "TC0F", "CPU die f" },
    { "TC0D", "CPU die" },   { "TCXC", "CPU PECI" },  { "TCMX", "CPU max" },
    { "TC1C", "Core 1" },    { "TC2C", "Core 2" },    { "TC3C", "Core 3" },
    { "TC4C", "Core 4" },    { "TC5C", "Core 5" },    { "TC6C", "Core 6" },
    { "TC7C", "Core 7" },    { "TC8C", "Core 8" },
    { "TCGC", "iGPU" },      { "TCSA", "SysAgent" },
    { "TG0P", "dGPU prox" }, { "TG1P", "dGPU prx2" },
    { "TGDD", "dGPU die" },  { "TGDE", "dGPU die2" }, { "TGDF", "dGPU die3" },
    { "TGVP", "GPU VRM" },   { "TGVF", "GPU VRM f" },
    { "TH0a", "NAND A" },    { "TH0b", "NAND B" },
    { "TH1a", "NAND C" },    { "TH1b", "NAND D" },
    { "TH0F", "SSD filt" },  { "TH0X", "SSD max" },
    { "TI0P", "TBolt" },     { "TTLD", "TBolt L" },   { "TTRD", "TBolt R" },
    { "TL0P", "Display" },   { "TM0P", "Mem prox" },  { "Tm0P", "Board" },
    { "TPCD", "PCH die" },   { "TP0P", "PCH prox" },  { "TW0P", "WiFi" },
    { "TaLC", "Airflow L" }, { "TaRC", "Airflow R" },
    { "Th0H", "Heatpipe" },  { "Th1H", "Heatpipe 1" },{ "Th2H", "Heatpipe 2" },
    { "Ts0P", "Palm L" },    { "Ts1P", "Palm R" },
    { "Ts0S", "Skin 1" },    { "Ts1S", "Skin 2" },    { "Ts2S", "Skin 3" },
};

static const char *smc_name(const char *key) {
    for (size_t i = 0; i < sizeof SMC_NAMES / sizeof *SMC_NAMES; i++)
        if (strcmp(SMC_NAMES[i].key, key) == 0) return SMC_NAMES[i].name;
    return "";
}

/* ---------- discovery ---------- */

/* Locate hwmon interfaces. applesmc needs special handling: on T2 kernels
 * its hwmon class dir has no attributes at all — the temp/fan files sit on
 * the parent ACPI device (recognizable by its key_count file). */
static void find_hwmon(Sensors *s) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
        char base[280], p[560], name[64];
        snprintf(base, sizeof base, "/sys/class/hwmon/%s", e->d_name);
        snprintf(p, sizeof p, "%s/name", base);
        if (!read_str(p, name, sizeof name)) {
            /* legacy applesmc: attrs live on the device dir */
            snprintf(p, sizeof p, "%s/device/key_count", base);
            if (file_exists(p)) {
                snprintf(s->smc_dir, sizeof s->smc_dir, "%s/device", base);
                s->has_smc = true;
            }
            continue;
        }
        if (strcmp(name, "applesmc") == 0) {
            snprintf(s->smc_dir, sizeof s->smc_dir, "%s", base);
            s->has_smc = true;
        } else if (strcmp(name, "coretemp") == 0) {
            snprintf(s->core_dir, sizeof s->core_dir, "%s", base);
            s->has_core = true;
        } else if (strcmp(name, "amdgpu") == 0) {
            snprintf(s->amd_hwmon, sizeof s->amd_hwmon, "%s", base);
            snprintf(s->amd_dev, sizeof s->amd_dev, "%s/device", base);
            s->has_amd = true;
        } else if (strcmp(name, "nvme") == 0) {
            snprintf(s->nvme_dir, sizeof s->nvme_dir, "%s", base);
            s->has_nvme = true;
        }
    }
    closedir(d);
}

static void find_i915(Sensors *s) {
    for (int c = 0; c < 8; c++) {
        char p[128], v[16];
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/vendor", c);
        if (!read_str(p, v, sizeof v)) continue;
        if (strcmp(v, "0x8086") == 0) {
            snprintf(p, sizeof p, "/sys/class/drm/card%d/gt_cur_freq_mhz", c);
            if (file_exists(p)) {
                snprintf(s->i915_freq, sizeof s->i915_freq, "%s", p);
                s->has_i915 = true;
            }
            return;
        }
    }
}

static void read_identity(Sensors *s) {
    read_str("/sys/class/dmi/id/product_name", s->model, sizeof s->model);
    if (!s->model[0]) snprintf(s->model, sizeof s->model, "unknown");
    gethostname(s->host, sizeof s->host - 1);
    struct utsname u;
    if (uname(&u) == 0) snprintf(s->kernel, sizeof s->kernel, "%.63s", u.release);

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *c = strchr(line, ':');
                if (c) {
                    c += 2;
                    char *nl = strchr(c, '\n');
                    if (nl) *nl = 0;
                    /* tidy: drop (R)/(TM), "CPU", clock suffix */
                    char out[64] = "";
                    size_t o = 0;
                    for (char *p = c; *p && o + 1 < sizeof out; ) {
                        if (strncmp(p, "(R)", 3) == 0 || strncmp(p, "(TM)", 4) == 0) {
                            p += (p[1] == 'R') ? 3 : 4;
                        } else if (strncmp(p, " CPU", 4) == 0) {
                            p += 4;
                        } else if (strncmp(p, " @", 2) == 0) {
                            break;
                        } else {
                            out[o++] = *p++;
                        }
                    }
                    out[o] = 0;
                    snprintf(s->cpu_name, sizeof s->cpu_name, "%s", out);
                }
                break;
            }
        }
        fclose(f);
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    s->ncpu = (n > 0 && n <= MAX_CPUS) ? (int)n : 1;
}

static void find_net_if(Sensors *s) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return;
    char line[256];
    (void)!fgets(line, sizeof line, f);   /* header */
    while (fgets(line, sizeof line, f)) {
        char ifc[32], dest[16];
        if (sscanf(line, "%31s %15s", ifc, dest) == 2 &&
            strcmp(dest, "00000000") == 0) {
            snprintf(s->net_if, sizeof s->net_if, "%s", ifc);
            break;
        }
    }
    fclose(f);
}

static void find_net_ip(Sensors *s) {
    s->net_ip[0] = 0;
    if (!s->net_if[0]) return;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return;
    for (struct ifaddrs *a = ifs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(a->ifa_name, s->net_if) != 0) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)a->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, s->net_ip, sizeof s->net_ip);
        break;
    }
    freeifaddrs(ifs);
}

void sensors_init(Sensors *s) {
    memset(s, 0, sizeof *s);
    s->first = true;
    s->bat_cycles = -1;
    read_identity(s);
    find_hwmon(s);
    find_i915(s);
    find_net_if(s);
    find_net_ip(s);
    s->has_bat = file_exists("/sys/class/power_supply/BAT0/capacity");
}

/* ---------- sampling ---------- */

static void sample_cpu(Sensors *s, double dt) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        int idx = -1;   /* -1 = aggregate */
        char *p = line + 3;
        if (*p != ' ') idx = (int)strtol(p, &p, 10);
        if (idx >= MAX_CPUS) continue;
        long long v[10] = { 0 };
        int n = sscanf(p, "%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
                       &v[0], &v[1], &v[2], &v[3], &v[4],
                       &v[5], &v[6], &v[7], &v[8], &v[9]);
        if (n < 4) continue;
        long long tot = 0;
        for (int i = 0; i < n; i++) tot += v[i];
        long long idle = v[3] + v[4];
        int slot = idx + 1;   /* 0 = aggregate */
        long long dtot = tot - s->pj_tot[slot];
        long long didle = idle - s->pj_idle[slot];
        double pct = (dtot > 0) ? 100.0 * (double)(dtot - didle) / (double)dtot : 0.0;
        pct = clampd(pct, 0, 100);
        if (idx < 0) s->cpu_total = s->first ? 0 : pct;
        else         s->cpu_per[idx] = s->first ? 0 : pct;
        s->pj_tot[slot] = tot;
        s->pj_idle[slot] = idle;
    }
    fclose(f);
    (void)dt;

    double sum = 0, mx = 0;
    int cnt = 0;
    for (int i = 0; i < s->ncpu; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        double khz = read_num(p);
        if (isnan(khz)) continue;
        sum += khz; cnt++;
        if (khz > mx) mx = khz;
    }
    s->ghz_avg = cnt ? sum / cnt / 1e6 : NAN;
    s->ghz_max = cnt ? mx / 1e6 : NAN;
}

static void sample_mem(Sensors *s) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char k[48];
    double v;
    double tot = 0, freem = 0, avail = 0, cach = 0, buf = 0, stot = 0, sfree = 0;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%47[^:]: %lf", k, &v) != 2) continue;
        v *= 1024.0;
        if      (!strcmp(k, "MemTotal"))     tot = v;
        else if (!strcmp(k, "MemFree"))      freem = v;
        else if (!strcmp(k, "MemAvailable")) avail = v;
        else if (!strcmp(k, "Cached"))       cach = v;
        else if (!strcmp(k, "Buffers"))      buf = v;
        else if (!strcmp(k, "SwapTotal"))    stot = v;
        else if (!strcmp(k, "SwapFree"))     sfree = v;
    }
    fclose(f);
    s->mem_total = tot;
    s->mem_avail = avail;
    s->mem_used  = tot - avail;
    s->mem_cache = cach + buf;
    s->swap_total = stot;
    s->swap_used  = stot - sfree;
    (void)freem;
}

/* Iterate tempN_label/tempN_input pairs in a hwmon-style dir. */
typedef void (*temp_cb)(Sensors *, const char *label, double mdeg);

static void scan_temps(Sensors *s, const char *dir, temp_cb cb) {
    for (int i = 1; i <= MAX_TEMPS; i++) {
        char p[384], label[64];
        snprintf(p, sizeof p, "%s/temp%d_label", dir, i);
        if (!read_str(p, label, sizeof label)) continue;
        snprintf(p, sizeof p, "%s/temp%d_input", dir, i);
        double v = read_num(p);
        if (isnan(v)) continue;
        cb(s, label, v);
    }
}

static void smc_cb(Sensors *s, const char *label, double mdeg) {
    double c = mdeg / 1000.0;
    if (c < 5.0 || c > 125.0) return;      /* sentinel / dead sensor */
    if (s->ntemps >= MAX_TEMPS) return;
    TempS *t = &s->temps[s->ntemps++];
    snprintf(t->key, sizeof t->key, "%.7s", label);
    snprintf(t->name, sizeof t->name, "%s", smc_name(label));
    t->val = c;
}

static void core_cb(Sensors *s, const char *label, double mdeg) {
    double c = mdeg / 1000.0;
    if (strncmp(label, "Package", 7) == 0) s->pkg_temp = c;
    else if (strncmp(label, "Core", 4) == 0 && c > s->core_max) s->core_max = c;
}

static int temp_cmp(const void *a, const void *b) {
    return strcmp(((const TempS *)a)->key, ((const TempS *)b)->key);
}

static void sample_smc(Sensors *s) {
    s->ntemps = 0;
    if (s->has_smc) {
        scan_temps(s, s->smc_dir, smc_cb);
        qsort(s->temps, (size_t)s->ntemps, sizeof(TempS), temp_cmp);
    }
    s->pkg_temp = NAN;
    s->core_max = 0;
    if (s->has_core) {
        s->pkg_temp = NAN;
        scan_temps(s, s->core_dir, core_cb);
    }
    if (s->core_max == 0) s->core_max = NAN;

    s->nfans = 0;
    if (s->has_smc) {
        for (int i = 1; i <= MAX_FANS; i++) {
            char p[384];
            snprintf(p, sizeof p, "%s/fan%d_input", s->smc_dir, i);
            double rpm = read_num(p);
            if (isnan(rpm)) break;
            FanS *fn = &s->fans[s->nfans++];
            snprintf(p, sizeof p, "%s/fan%d_label", s->smc_dir, i);
            char lb[32] = "";
            read_str(p, lb, sizeof lb);
            if (lb[0]) snprintf(fn->name, sizeof fn->name, "%.12s", lb);
            else       snprintf(fn->name, sizeof fn->name, "Fan %d", i);
            fn->rpm = rpm;
            snprintf(p, sizeof p, "%s/fan%d_min", s->smc_dir, i);
            fn->min = read_num(p);
            snprintf(p, sizeof p, "%s/fan%d_max", s->smc_dir, i);
            fn->max = read_num(p);
            snprintf(p, sizeof p, "%s/fan%d_output", s->smc_dir, i);
            fn->target = read_num(p);
        }
    }
}

static void sample_battery(Sensors *s, double dt) {
    if (!s->has_bat) return;
    const char *B = "/sys/class/power_supply/BAT0";
    char p[128];
    snprintf(p, sizeof p, "%s/capacity", B);
    s->bat_pct = read_num(p);
    snprintf(p, sizeof p, "%s/status", B);
    read_str(p, s->bat_status, sizeof s->bat_status);
    snprintf(p, sizeof p, "%s/cycle_count", B);
    double cyc = read_num(p);
    s->bat_cycles = isnan(cyc) ? -1 : (int)cyc;
    snprintf(p, sizeof p, "%s/temp", B);
    double bt = read_num(p);
    s->bat_temp = isnan(bt) ? NAN : bt / 10.0;

    /* charge_* (µAh) preferred; energy_* (µWh) fallback */
    snprintf(p, sizeof p, "%s/charge_now", B);
    double now = read_num(p);
    snprintf(p, sizeof p, "%s/charge_full", B);
    double full = read_num(p);
    snprintf(p, sizeof p, "%s/charge_full_design", B);
    double design = read_num(p);
    snprintf(p, sizeof p, "%s/voltage_now", B);
    double volts = read_num(p) / 1e6;
    snprintf(p, sizeof p, "%s/current_now", B);
    double amps = read_num(p) / 1e6;
    bool coulomb = !isnan(now) && !isnan(amps) && !isnan(volts);
    if (!coulomb) {
        snprintf(p, sizeof p, "%s/energy_now", B);
        now = read_num(p);
        snprintf(p, sizeof p, "%s/energy_full", B);
        full = read_num(p);
        snprintf(p, sizeof p, "%s/energy_full_design", B);
        design = read_num(p);
        snprintf(p, sizeof p, "%s/power_now", B);
        amps = read_num(p) / 1e6;   /* watts, reusing the rate slot */
        volts = 1.0;
    }
    s->bat_health = (!isnan(full) && !isnan(design) && design > 0)
                    ? 100.0 * full / design : NAN;

    bool discharging = strcmp(s->bat_status, "Discharging") == 0;
    bool charging    = strcmp(s->bat_status, "Charging") == 0;
    double w = (isnan(amps) || isnan(volts)) ? NAN : amps * volts;
    s->bat_watts = isnan(w) ? NAN : (discharging ? -w : w);

    /* smooth the rate for a stable time estimate */
    if (!isnan(amps)) {
        double a = clampd(dt / 4.0, 0.05, 1.0);   /* ~4s time constant */
        s->ema_amps = s->first ? amps : s->ema_amps + a * (amps - s->ema_amps);
    }
    s->bat_time_h = NAN;
    if (s->ema_amps > 0.05 && !isnan(now)) {
        if (discharging)                        s->bat_time_h = now / 1e6 / s->ema_amps;
        else if (charging && !isnan(full))      s->bat_time_h = (full - now) / 1e6 / s->ema_amps;
    }

    s->ac_online = false;
    DIR *d = opendir("/sys/class/power_supply");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char tp[320], val[32];
            snprintf(tp, sizeof tp, "/sys/class/power_supply/%s/type", e->d_name);
            if (read_str(tp, val, sizeof val) && strcmp(val, "Mains") == 0) {
                snprintf(tp, sizeof tp, "/sys/class/power_supply/%s/online", e->d_name);
                if (read_num(tp) > 0) s->ac_online = true;
            }
        }
        closedir(d);
    }
}

static void sample_gpu(Sensors *s) {
    if (s->has_amd) {
        char p[384];
        snprintf(p, sizeof p, "%s/gpu_busy_percent", s->amd_dev);
        s->amd_busy = read_num(p);
        snprintf(p, sizeof p, "%s/mem_busy_percent", s->amd_dev);
        s->amd_membusy = read_num(p);
        snprintf(p, sizeof p, "%s/mem_info_vram_used", s->amd_dev);
        s->amd_vram_used = read_num(p);
        snprintf(p, sizeof p, "%s/mem_info_vram_total", s->amd_dev);
        s->amd_vram_total = read_num(p);
        snprintf(p, sizeof p, "%s/temp1_input", s->amd_hwmon);
        s->amd_edge = read_num(p) / 1000.0;
        snprintf(p, sizeof p, "%s/temp2_input", s->amd_hwmon);
        s->amd_junction = read_num(p) / 1000.0;
        snprintf(p, sizeof p, "%s/power1_average", s->amd_hwmon);
        s->amd_watts = read_num(p) / 1e6;
        snprintf(p, sizeof p, "%s/freq1_input", s->amd_hwmon);
        s->amd_sclk = read_num(p) / 1e6;
        snprintf(p, sizeof p, "%s/freq2_input", s->amd_hwmon);
        s->amd_mclk = read_num(p) / 1e6;
    }
    s->i915_mhz = s->has_i915 ? read_num(s->i915_freq) : NAN;
    if (s->has_nvme) {
        char p[384];
        snprintf(p, sizeof p, "%s/temp1_input", s->nvme_dir);
        s->nvme_temp = read_num(p) / 1000.0;
    }
}

static void sample_net(Sensors *s, double dt) {
    find_net_if(s);   /* default route can move (wifi <-> eth) */
    if (!s->net_if[0]) { s->net_rx_bps = s->net_tx_bps = 0; return; }
    char p[128];
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/rx_bytes", s->net_if);
    double rx = read_num(p);
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/tx_bytes", s->net_if);
    double tx = read_num(p);
    if (!s->first && dt > 0 && !isnan(rx) && rx >= s->prev_rx) {
        s->net_rx_bps = (rx - s->prev_rx) / dt;
        s->net_tx_bps = (tx - s->prev_tx) / dt;
    }
    if (!isnan(rx)) { s->prev_rx = rx; s->prev_tx = tx; }

    s->wifi_dbm = NAN;
    FILE *f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[256], want[40];
        snprintf(want, sizeof want, "%s:", s->net_if);
        while (fgets(line, sizeof line, f)) {
            char *c = strstr(line, want);
            if (!c) continue;
            double st, link, lvl;
            if (sscanf(c + strlen(want), "%lf %lf %lf", &st, &link, &lvl) == 3)
                s->wifi_dbm = lvl;
            break;
        }
        fclose(f);
    }
    if (dt > 1e-9 || s->first) find_net_ip(s);
}

/* Whole physical disks only: nvme0n1 yes, nvme0n1p2 no; sda yes, sda1 no. */
static bool is_whole_disk(const char *n) {
    size_t len = strlen(n);
    if (strncmp(n, "nvme", 4) == 0) return strchr(n, 'p') == NULL;
    if ((n[0] == 's' || n[0] == 'v') && n[1] == 'd')
        return len >= 3 && !(n[len - 1] >= '0' && n[len - 1] <= '9');
    if (strncmp(n, "mmcblk", 6) == 0) return strchr(n, 'p') == NULL;
    return false;
}

static void sample_disk(Sensors *s, double dt) {
    FILE *f = fopen("/proc/diskstats", "r");
    if (f) {
        char line[512];
        double rd = 0, wr = 0;
        while (fgets(line, sizeof line, f)) {
            unsigned maj, min;
            char name[64];
            unsigned long long rio, rmerge, rsect, rms, wio, wmerge, wsect;
            if (sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu",
                       &maj, &min, name, &rio, &rmerge, &rsect, &rms,
                       &wio, &wmerge, &wsect) < 10) continue;
            if (!is_whole_disk(name)) continue;
            rd += (double)rsect * 512.0;
            wr += (double)wsect * 512.0;
        }
        fclose(f);
        if (!s->first && dt > 0 && rd >= s->prev_rd) {
            s->dsk_r_bps = (rd - s->prev_rd) / dt;
            s->dsk_w_bps = (wr - s->prev_wr) / dt;
        }
        s->prev_rd = rd;
        s->prev_wr = wr;
    }
    struct statvfs sv;
    if (statvfs("/", &sv) == 0) {
        s->root_total = (double)sv.f_blocks * sv.f_frsize;
        s->root_used  = s->root_total - (double)sv.f_bfree * sv.f_frsize;
    }
}

static void sample_sys(Sensors *s) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf %lf %lf", &s->load1, &s->load5, &s->load15) != 3)
            s->load1 = s->load5 = s->load15 = 0;
        fclose(f);
    }
    s->uptime_s = read_num("/proc/uptime");
}

void sensors_sample(Sensors *s) {
    double t = now_ms() / 1000.0;
    double dt = s->first ? 0 : t - s->prev_t;
    sample_cpu(s, dt);
    sample_mem(s);
    sample_smc(s);
    sample_battery(s, dt > 0 ? dt : 0.5);
    sample_gpu(s);
    sample_net(s, dt);
    sample_disk(s, dt);
    sample_sys(s);
    s->prev_t = t;
    s->first = false;
}
