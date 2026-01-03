<div align="center">

# Preheat — Linux Application Preload Daemon

[![Typing SVG](https://readme-typing-svg.demolab.com?font=Fira+Code&weight=500&size=18&duration=3000&pause=1000&color=F7931A&center=true&vCenter=true&width=450&lines=Predict+%E2%80%A2+Preload+%E2%80%A2+Perform;Adaptive+readahead+daemon;30-60%25+faster+cold+starts)](https://github.com/wasteddreams/preheat-linux)

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Linux](https://img.shields.io/badge/Linux-FCC624?logo=linux&logoColor=black)](https://www.kernel.org/)
[![Systemd](https://img.shields.io/badge/Systemd-Ready-green?logo=systemd&logoColor=white)](https://systemd.io/)
[![Performance](https://img.shields.io/badge/Speedup-30--60%25-orange?logo=speedtest&logoColor=white)]()
[![🎁 Easter Egg](https://img.shields.io/badge/🎁-Click_Me-purple)](https://www.youtube.com/watch?v=dQw4w9WgXcQ)

*Can reduce application cold-start times by 30–60% on HDD-based systems*

</div>

Preheat is a lightweight Linux preload daemon that predicts and preloads application files to reduce cold-start times on HDD and low-resource systems. It is a userspace performance daemon, not a kernel module, and works on any systemd-based Linux distribution.

---

## Motivation

I dual-boot Kali Linux with Windows. These days, I use Kali a lot more since I'm learning cybersecurity.

My laptop isn't new, and Kali runs on an HDD, so after a cold boot, things can feel slow. Opening a browser, a terminal, or other tools I use all the time takes longer than it should. What annoyed me was that the system usually had free RAM, but it wasn't being used for anything useful.

I'd heard of [preload](https://pkgs.org/download/preload) before, and the idea clicked. If I keep opening the same applications every day, the system should notice that and prepare for it. Instead of just installing something and moving on, I decided to try building my own version to understand how it works and learn more about Linux internals.

Around that time, I came across Antigravity and the whole "vibe coding" approach, which pushed me to experiment more and move faster without overthinking everything upfront. That helped me take this beyond a rough experiment and actually turn it into something stable. I was able to build and iterate on system-level behavior while keeping the rest of the system untouched, which was important to me.

That's how Preheat came about. It's a small daemon that watches what I use, learns from it, and preloads things it thinks I'll open next. Nothing fancy, nothing hidden. You can see what it's doing and why.

It started as something I built for my own setup, but it turned out useful enough that I decided to clean it up and share it.

---

## Quick Start

```bash
# One-liner install
curl -fsSL https://raw.githubusercontent.com/wasteddreams/preheat-linux/main/setup.sh | sudo bash

# Verify
preheat-ctl status
```

→ [Full Installation Guide](docs/installation.md)

---

## Features (v1.0.1)

| Feature | Description |
|---------|-------------|
| Two-tier tracking | Priority pool (user apps) vs Observation pool (system) |
| Session-aware boot | Aggressive 3-min preload window on login |
| Smart seeding | Immediate value from XDG/shell history |
| Pool management | `promote`, `demote`, `show-hidden`, `reset` |
| Health checks | `health`, `explain`, `stats --verbose` |
| Single-instance | PID file locking prevents duplicate daemons |

---

## How Preheat Differs from preload

- Session-aware learning instead of static heuristics
- Two-tier pool separation (user vs system apps)
- Transparent decision explanations (`preheat-ctl explain`)
- Designed for modern systemd environments

---

> [!WARNING]
> **Snap applications are NOT supported.** Ubuntu's snap packages (Firefox, Chromium, etc.) run in an AppArmor sandbox that blocks the daemon from tracking them. If you use Ubuntu 22.04+, consider installing apps via `apt` or Flatpak instead. See [Known Limitations](docs/known-limitations.md) for details.

## Essential Commands

```bash
preheat-ctl status              # Daemon status
preheat-ctl health              # Quick health check
sudo preheat-ctl stats          # Hit rate & statistics
preheat-ctl explain firefox     # Decision reasoning
sudo preheat-ctl promote code   # Always preload this app
```

→ [CLI Reference](docs/api-reference.md)

---

## How It Works

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│  SCAN    │ →  │  LEARN   │ →  │ PREDICT  │ →  │ PRELOAD  │
│  /proc   │    │ Patterns │    │  Score   │    │  Cache   │
└──────────┘    └──────────┘    └──────────┘    └──────────┘
```

*A continuous feedback loop that adapts to your usage patterns.*

→ [Detailed Explanation](docs/how-it-works.md)

---

## Performance

| Metric | Value |
|--------|-------|
| Startup improvement | 30-60% for frequent apps |
| Daemon memory | ~5-10 MB RSS |
| CPU overhead | < 1% |

---

## Project Status

Preheat is stable for daily use and actively tested on Kali and Debian-based systems.

---

## Documentation

| Getting Started | Reference |
|-----------------|-----------|
| [Installation](docs/installation.md) | [Configuration](docs/configuration.md) |
| [Quick Start](docs/quick-start.md) | [API Reference](docs/api-reference.md) |
| [How It Works](docs/how-it-works.md) | [Troubleshooting](docs/troubleshooting.md) |

---

## Acknowledgments

- Inspired by the original [preload](https://sourceforge.net/projects/preload/) project
- Built with [Antigravity](https://antigravity.google/) using Claude Opus 4.5 Thinking and Claude Sonnet 4.5 Thinking

---

## License

GPL v2 — See [LICENSE](LICENSE)
