<div align="center">

# Preheat

[![Typing SVG](https://readme-typing-svg.demolab.com?font=JetBrains+Mono&weight=500&size=18&duration=3000&pause=1000&color=2E86AB&center=true&vCenter=true&multiline=true&width=600&height=60&lines=Warm+up+your+apps+before+you+need+them;Preload+%E2%80%A2+Predict+%E2%80%A2+Perform)](https://github.com/wasteddreams/preheat)

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Linux](https://img.shields.io/badge/Linux-FCC624?logo=linux&logoColor=black)](https://www.kernel.org/)
[![Systemd](https://img.shields.io/badge/Systemd-Ready-green?logo=systemd&logoColor=white)](https://systemd.io/)
[![Performance](https://img.shields.io/badge/Speedup-30--60%25-orange?logo=speedtest&logoColor=white)]()
[![🎁 Easter Egg](https://img.shields.io/badge/🎁-Click_Me-purple)](https://www.youtube.com/watch?v=dQw4w9WgXcQ)

*Reduces application cold-start times by 30-60% through intelligent prediction and preloading*

</div>

---

## Overview

Preheat monitors application usage patterns and preloads frequently-used applications into memory before they are launched. This significantly reduces startup times, particularly on systems with slower storage devices.

**Features:**
- Adaptive learning of application usage patterns
- Correlation-based prediction engine
- Manual whitelist support for priority applications
- Systemd integration with security hardening
- Low memory footprint (~5MB)

---

## Motivation

This project exists thanks to an aging laptop, a painfully slow HDD, and my decision to daily-drive Kali Linux anyway.

I use Kali regularly while learning cybersecurity fundamentals and strengthening my Linux basics. Unfortunately, on older hardware, even simple tasks can feel slower than they should—especially when launching applications. The system itself isn't completely starved of resources though; a decent amount of RAM often sits idle, doing absolutely nothing useful. Inspired by the idea behind the classic [preload](https://pkgs.org/download/preload) daemon, I decided to build a lightweight solution of my own. The goal is simple: make better use of available memory to reduce application startup times and improve overall responsiveness, without introducing unnecessary bloat or complexity.

---

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/wasteddreams/preheat/main/install.sh | sudo bash
```

Or clone and install manually:

```bash
git clone https://github.com/wasteddreams/preheat.git
cd preheat
sudo ./scripts/install.sh
```

---

## Manual Installation

### Prerequisites

```bash
sudo apt-get install autoconf automake pkg-config libglib2.0-dev
```

### Build & Install

```bash
autoreconf --install
./configure
make
sudo make install
```

### Enable Service

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now preheat
```

---

## Usage

```bash
# Check status
sudo systemctl status preheat

# View logs
sudo tail -f /usr/local/var/log/preheat.log

# Reload configuration
sudo preheat-ctl reload
```

---

## Manual Whitelist

To always preload specific applications:

```bash
sudo mkdir -p /etc/preheat.d
echo "/usr/bin/firefox" | sudo tee -a /etc/preheat.d/apps.list
sudo preheat-ctl reload
```

---

## Configuration

Configuration file: `/usr/local/etc/preheat.conf`

| Setting | Default | Description |
|---------|---------|-------------|
| `cycle` | 20 | Scan interval (seconds) |
| `memfree` | 50 | Percentage of free RAM to use |
| `manualapps` | - | Path to whitelist file |

See [CONFIGURATION.md](CONFIGURATION.md) for full reference.

---

## Uninstall

```bash
sudo systemctl stop preheat
sudo systemctl disable preheat
sudo make uninstall
```

---

## Credits

- Based on the [preload](https://pkgs.org/download/preload) daemon
- Developed with Claude Opus 4.5 and Claude Sonnet 4.5 via [Antigravity](https://antigravity.google/)
- Works on Kali Linux and other Debian-based distributions

## License

GPL v2 — see [LICENSE](LICENSE)
