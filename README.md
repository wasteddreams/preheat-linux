<div align="center">

# Preheat — Linux Application Preload Daemon

[![Typing SVG](https://readme-typing-svg.demolab.com?font=Fira+Code&weight=500&size=18&duration=3000&pause=1000&color=F7931A&center=true&vCenter=true&width=450&lines=Preload+%E2%80%A2+Predict+%E2%80%A2+Perform;Adaptive+readahead+daemon;30-60%25+faster+cold+starts)](https://github.com/wasteddreams/preheat)

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Linux](https://img.shields.io/badge/Linux-FCC624?logo=linux&logoColor=black)](https://www.kernel.org/)
[![Systemd](https://img.shields.io/badge/Systemd-Ready-green?logo=systemd&logoColor=white)](https://systemd.io/)
[![Performance](https://img.shields.io/badge/Speedup-30--60%25-orange?logo=speedtest&logoColor=white)]()
[![🎁 Easter Egg](https://img.shields.io/badge/🎁-Click_Me-purple)](https://www.youtube.com/watch?v=dQw4w9WgXcQ)

*Reduces application cold-start times by 30-60% through intelligent prediction and preloading*

</div>

---

## Overview

Preheat is a lightweight Linux readahead and application preload daemon that monitors application usage patterns and preloads frequently-used binaries into memory before they are launched. It is designed to improve Linux system performance and reduce cold-start times on Kali Linux and other Debian-based distributions, especially on systems with slower HDD storage.

**Features:**
- Adaptive learning of application usage patterns
- Correlation-based prediction engine
- Manual whitelist support for priority applications
- Systemd integration with security hardening
- Low memory footprint (~5MB daemon RSS)*

---

## Motivation

This project exists thanks to an aging laptop, a painfully slow HDD, and my decision to daily-drive Kali Linux anyway.

I use Kali regularly while learning cybersecurity fundamentals and strengthening my Linux basics. Unfortunately, on older hardware, even simple tasks can feel slower than they should—especially when launching applications. The system itself isn't completely starved of resources though; a decent amount of RAM often sits idle, doing absolutely nothing useful. Inspired by the idea behind the classic [preload](https://pkgs.org/download/preload) daemon, I decided to build a lightweight solution of my own. The goal is simple: make better use of available memory to reduce application startup times and improve overall responsiveness, without introducing unnecessary bloat or complexity.

---

## Quick Start

Get preheat running on your Debian-based Linux system in under 5 minutes:

```bash
# Clone the repository
git clone https://github.com/wasteddreams/preheat-linux.git
cd preheat-linux

# Install (handles dependencies, build, and systemd setup)
sudo ./scripts/install.sh

# Verify it's running
sudo systemctl status preheat
```

For detailed installation options, see the [Installation Guide](docs/installation.md).

---

## Diagnostics & Monitoring

Run a system check before or after installation:

```bash
# Self-test: verify system requirements
preheat --self-test

# Check daemon status (no sudo needed)
preheat-ctl status

# View memory available for preloading
preheat-ctl mem

# See tracked applications
preheat-ctl predict --top 10
```

---

## How It Works

Preheat operates as a background systemd service using a four-phase cycle:

1. **Monitor** — Scans `/proc` to track running applications
2. **Learn** — Builds Markov chain models of application co-occurrence
3. **Predict** — Scores applications likely to be launched next
4. **Preload** — Uses `readahead(2)` to load predicted files into the disk cache

The daemon runs with low priority and conservative defaults, ensuring it never interferes with foreground applications.

→ [How It Works (detailed)](docs/how-it-works.md) · [Architecture](docs/architecture.md)

---

## Who Should Use Preheat

**Ideal for:**
- Systems with **slower storage** (HDD, SATA SSD) and **unused RAM sitting idle**
- Laptops and desktops with predictable, repetitive workflows
- Users who launch the same applications daily (browsers, editors, IDEs)
- Older hardware where cold starts feel noticeably slow
- Anyone frustrated by waiting for applications to load

**Less beneficial for:**
- Systems with fast NVMe storage (already quick startup times)
- Machines with very limited RAM (<2 GB) — no headroom for caching
- Server workloads running persistent, long-lived services
- Highly unpredictable, random application usage patterns

---

## Documentation

Complete documentation is available in the [`docs/`](docs/) directory:

| Guide | Description |
|-------|-------------|
| [Introduction](docs/introduction.md) | Project goals, philosophy, and design constraints |
| [Installation](docs/installation.md) | System requirements and installation methods |
| [Quick Start](docs/quick-start.md) | First-run verification and basic commands |
| [How It Works](docs/how-it-works.md) | Operational principles and prediction algorithm |
| [Architecture](docs/architecture.md) | Internal design and system interaction |
| [Configuration](docs/configuration.md) | Complete configuration reference |
| [Advanced Usage](docs/advanced-usage.md) | Performance tuning and debugging |
| [API Reference](docs/api-reference.md) | CLI commands, signals, and file formats |
| [Troubleshooting](docs/troubleshooting.md) | Common problems and solutions |
| [Examples](docs/examples/) | Real-world configuration scenarios |

---

## Configuration Overview

Default configuration works well for most systems. Key options:

| Setting | Default | Purpose |
|---------|---------|---------|
| `cycle` | 20 sec | How often to scan and predict |
| `memfree` | 50% | Percentage of free RAM to use for preloading |
| `sortstrategy` | 3 (block) | I/O optimization (use 0 for SSD) |

Configuration file: `/usr/local/etc/preheat.conf`

```bash
# Reload after editing
sudo preheat-ctl reload
```

→ [Full Configuration Reference](docs/configuration.md)

---

## Manual Application Whitelist

Force specific applications to always be preloaded with highest priority:

```bash
sudo mkdir -p /etc/preheat.d
echo "/usr/bin/firefox" | sudo tee -a /etc/preheat.d/apps.list
sudo preheat-ctl reload
```

→ [Advanced Usage: Whitelists and Blacklists](docs/advanced-usage.md)

---

## Uninstall

```bash
sudo systemctl stop preheat
sudo systemctl disable preheat
cd preheat-linux
sudo make uninstall
```

---

## Design Philosophy

- **Safety first** — Conservative defaults that work out of the box
- **Behavioral parity** — Maintains compatibility with the original preload daemon
- **Opt-in extensions** — Advanced features are disabled by default
- **Low overhead** — Minimal CPU (<1%) and memory (~5-10 MB RSS) footprint*
- **Non-intrusive** — Runs at low priority, never interferes with user tasks

---

> **\* Note on memory usage:** `systemctl status preheat` may report higher memory (100-400MB) because systemd's cgroup accounting includes page cache from `readahead()` operations. This cached data is shared system-wide and reclaimable under memory pressure. The daemon's actual private memory (RSS) is ~5-10MB—verify with `ps aux | grep preheat`.

---

## Credits

- Based on the [preload](https://pkgs.org/download/preload) daemon concept
- Developed with Claude Opus 4.5 and Claude Sonnet 4.5 via [Antigravity](https://antigravity.google/)
- Tested on Kali Linux and Debian-based distributions

## License

GPL v2 — see [LICENSE](LICENSE)
