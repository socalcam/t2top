# t2top

**Hardware telemetry for Macs running Linux.**

A dependency-free terminal dashboard for Intel Macs with the Apple T2 chip —
the machines the [t2linux](https://t2linux.org) project brought to Linux.
Generic monitors show you a CPU percentage. Your Mac knows *so much more*:
this machine exposes **~50 SMC temperature sensors** through `applesmc`,
from per-core diodes to the palm rests, plus fan controller targets, battery
cycle counts, and dual-GPU telemetry. `t2top` decodes all of it.

```
  t2top MacBookPro16,1 · CamArchy · linux-t2 · up 1:17 · load 1.32
╭─ CPU · Intel Core i9-9880H · 16 threads ──────╮╭─ SMC · 48 sensors ─────────────────╮
│                                    ⢠      ⢀   ││ TA0V  23° Ambient   TPCD 57° PCH   │
│                                    ⢸⣧⣤⣴⣧⣴⣦⣤⣼⣧ ││ TB0T  31° Battery   TTLD 44° TB L  │
│                                    ⢸⣿⣿⣿⣿⣿⣿⣿⣿⣿ ││ TC0E  96° CPU die   TW0P 54° WiFi  │
│                       ⣦⣄⣀⣄⣠⣀⣀⣠⣀⣀⣠⣀⣼⣿⣿⣿⣿⣿⣿⣿⣿⣿ ││ TC1C  76° Core 1    Th1H 62° Pipe1 │
│ ▁ █ █ █ █ █ █ █ █ ▁ █ ▁ ▁ █ ▂ ▁               ││ TC5C  77° Core 5    Ts0P 30° PalmL │
│  69% · 4.05 GHz avg · pkg 92° · hot core 96°  ││ TG0P  58° dGPU prx  Ts1P 27° PalmR │
╰───────────────────────────────────────────────╯│ ...                                │
╭─ GPU ─────────╮╭─ Fans ─────────╮╭─ Battery ───╮│                                    │
│ busy ⣀⣀⣀   8% ││ Fan 1 4467 rpm ││  20% ████▎  ││                                    │
│ 62°·66°·17.0W ││ ███████──── ↗  ││ health 89%  ││                                    │
╰───────────────╯╰────────────────╯╰─────────────╯╰────────────────────────────────────╯
```

*(Live it's truecolor: temperature-gradient heatmap colors, braille history
graphs, and it breathes at up to 5 fps.)*

## Why

Running Linux on a T2 MacBook means living with hardware Linux only half
understands. When the fans spin up you want to know *which* sensor tripped —
was it the CPU die, the dGPU VRM, or a NAND package? `t2top` puts the whole
sensor wall on screen with human names for Apple's terse four-char SMC keys
(`TC0P` → *CPU proximity*, `Th1H` → *Heatpipe 1*, `Ts0P` → *Palm rest L*),
next to the fans' actual controller targets and true battery health.

## What it shows

| Panel | Contents |
|---|---|
| **SMC wall** | Every valid `applesmc` temperature, decoded + heat-colored, live count |
| **CPU** | Braille load history, per-core bars, avg/max GHz, package + hottest core |
| **GPU** | dGPU busy/VRAM/edge/junction/watts/clocks (amdgpu) + iGPU frequency |
| **Fans** | RPM vs. the SMC controller's target, min→max gauge |
| **Battery** | %, watts in/out, time estimate, **health %, cycle count**, cell temp, AC state |
| **Memory / Disk / Network** | The essentials, with rate sparklines and WiFi dBm |

## Build

Any C11 compiler, zero dependencies:

```sh
make
sudo make install      # /usr/local/bin + man page
```

## Run

```sh
t2top                  # the dashboard
t2top --list           # plain-text sensor dump
t2top --once           # render one frame (screenshots, scripts)
```

Keys: `q` quit · `p` pause · `+`/`-` sampling rate · `f` °C/°F.
Needs a ≥96×24 terminal, UTF-8, and truecolor (`--256` fallback included).
No root required.

## Design notes

- **Zero dependencies.** One binary, ~62 KB. Pure POSIX + `/sys` + `/proc`.
- **Hand-rolled renderer.** Cell-based double buffer; each frame diffs
  against the last and emits only changed cells with minimized cursor moves
  and SGR switches — idle refreshes cost a few hundred bytes of writes.
- **Braille sparklines.** 2×4 dots per cell = 8× the vertical resolution of
  block characters.
- **Graceful degradation.** Every data source is optional; on a non-Mac it
  shows whatever hwmon offers and hides the rest. Sentinel values from dead
  SMC sensors (0°, negative, >125°) are filtered; sensors that wake under
  load appear dynamically.
- **T2 quirk handled:** on t2linux kernels `applesmc` registers a nameless
  hwmon class device with the sensor files on the parent ACPI device —
  discovery follows `key_count` to find it.

## Compatibility

Built and tested on a MacBookPro16,1 (i9-9880H, Navi 14) running
[Omarchy](https://omarchy.org) with the
[linux-t2](https://t2linux.org) kernel. Should behave on any T2-era Mac;
degrades gracefully anywhere else Linux runs.

## License

MIT
