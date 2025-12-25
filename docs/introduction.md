# Introduction to Preheat

Preheat is an adaptive readahead daemon that accelerates application startup times on Linux systems by intelligently preloading frequently used programs into memory.

---

## What Preheat Does

When you launch an application, the operating system must read its executable file and shared libraries from disk into memory. On mechanical hard drives, this disk I/O is the primary bottleneck causing slow startup times. Even on SSDs, cold starts can be noticeably slower than warm starts.

Preheat solves this by:

1. **Monitoring** which applications you run
2. **Learning** your usage patterns using statistical models
3. **Predicting** which applications you're likely to run next
4. **Preloading** those files into the disk cache proactively

The result: applications start faster because their files are already in memory when you launch them.

---

## The Problem It Solves

Consider a typical workflow:

```
Without Preheat:
┌──────────┐     ┌──────────┐     ┌──────────┐
│ You      │ --> │ Launch   │ --> │ Wait for │ --> App Ready
│ Click    │     │ Firefox  │     │ Disk I/O │     (3-5 sec)
└──────────┘     └──────────┘     └──────────┘

With Preheat:
┌──────────┐     ┌──────────┐     ┌──────────┐
│ Preheat  │     │ Firefox  │     │ You      │
│ Preloads │     │ Files in │ --> │ Click    │ --> App Ready
│ Firefox  │     │ Cache    │     │ Firefox  │     (< 1 sec)
└──────────┘     └──────────┘     └──────────┘
```

### Typical Improvements

| Scenario | Without Preheat | With Preheat | Improvement |
|----------|-----------------|--------------|-------------|
| Browser (cold start) | 4-8 seconds | 1-2 seconds | 60-75% |
| IDE (cold start) | 10-20 seconds | 3-6 seconds | 50-70% |
| Office suite | 5-10 seconds | 1-3 seconds | 60-70% |

*Results vary based on hardware and usage patterns.*

---

## Design Goals

### Primary Goals

1. **Automatic operation**: Once installed, preheat runs without intervention
2. **Low overhead**: Minimal CPU usage (<1%) and controlled memory consumption
3. **Adaptive learning**: Improves predictions as it learns your habits
4. **Safe defaults**: Conservative settings that work well out of the box

### Secondary Goals

- systemd integration for proper service management
- Flexible configuration for power users
- Graceful handling of low-memory conditions
- Persistent state across reboots

---

## What Preheat Does NOT Do

Understanding limitations is as important as understanding capabilities:

| Preheat Does NOT... | Reason |
|---------------------|--------|
| Speed up running applications | It only accelerates startup, not runtime |
| Help applications not in the prediction model | New or rarely-used apps aren't predicted |
| Work well with insufficient RAM | Preloading requires free memory headroom |
| Improve SSD performance significantly | SSDs are already fast; gains are marginal |
| Replace proper system optimization | It complements, not replaces, other tuning |

---

## How It Differs from Similar Tools

### vs. systemd-readahead (deprecated)

- systemd-readahead recorded boot-time file access and replayed it
- Preheat learns ongoing usage patterns, not just boot sequences
- Preheat adapts to changing workflows

### vs. preload (original)

- Preheat is based on preload's concepts
- Updated for modern systems and systemd
- Enhanced configuration options
- Maintained and actively developed

### vs. Early-OOM / zram

- These tools manage memory pressure
- Preheat proactively fills available memory with useful data
- They can work together complementarily

---

## Supported Environments

### Operating Systems

Preheat works on any Debian-based Linux distribution:
- Debian 11+
- Ubuntu 20.04+
- Linux Mint
- Other apt-based distributions

### Hardware Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| RAM | 2 GB | 4+ GB (more headroom for caching) |
| Storage | Any | Slower storage benefits most |
| Kernel | 2.6+ | 4.0+ |

### When Preheat Helps Most

- **Slower storage** (HDD, SATA SSD) with **unused RAM sitting idle**
- **Repetitive workflows** — launching the same apps daily
- **Older hardware** where cold starts feel noticeably slow
- **Desktop/laptop use** with predictable launch patterns

### When Preheat Helps Less

- **Fast NVMe storage** — already quick startup times
- **Very limited RAM** (<2 GB) — no headroom for caching
- **Server workloads** — persistent services, not interactive apps
- **Random usage patterns** — unpredictable usage defeats prediction

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                      PREHEAT DAEMON                      │
├────────────────┬────────────────┬───────────────────────┤
│   MONITORING   │   PREDICTION   │      PRELOADING       │
│                │                │                       │
│  /proc scanner │  Markov chain  │  readahead(2) calls   │
│  Process maps  │  Correlation   │  Parallel I/O         │
│  Library deps  │  Scoring       │  Memory management    │
└───────┬────────┴───────┬────────┴───────────┬───────────┘
        │                │                    │
        v                v                    v
┌───────────────┐ ┌──────────────┐ ┌─────────────────────┐
│ State File    │ │ Config File  │ │ Disk Cache (kernel) │
│ (persistent)  │ │ (user)       │ │ (memory)            │
└───────────────┘ └──────────────┘ └─────────────────────┘
```

For detailed architecture documentation, see [Architecture](architecture.md).

---

## Getting Started

Ready to try preheat? Follow these steps:

1. **[Install preheat](installation.md)** - Get it running on your system
2. **[Quick start guide](quick-start.md)** - Verify it's working
3. **[Configuration](configuration.md)** - Customize for your needs

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| - | [Documentation Index](index.md) | [Installation →](installation.md) |
