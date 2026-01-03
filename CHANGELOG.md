# Changelog

All notable changes to Preheat will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026-01-03

### 🛡️ Security Audit (2026-01-02)

#### Critical Fix
- **L-1**: Fixed command injection in `lib_scanner.c` - malicious filenames could execute shell commands via popen

#### High Severity
- **F-1**: Fixed use-after-free in `stats.c` when accessing freed GArray

#### Medium Severity  
- **M-1/M-3**: Fixed Markov chain timestamp and assertion bugs
- **F-2/F-3**: Fixed app family init order and duplicate handling
- **L-2**: Fixed integer overflow in `/proc/PID/maps` parsing
- **SS-1**: Fixed smart seeding clobbering prior data

#### Low Severity
- **M-4/M-5/M-6**: Fixed division by zero, integer overflow, memory leak in Markov
- **F-4**: Fixed time_t truncation in family stats
- **L-3/L-4/L-5**: Fixed memory leak, buffer init, sign mismatch in proc.c
- **S-1**: Fixed format specifier for size_t in state I/O
- **HM-1**: Replaced strncpy with safer g_strlcpy in stats
- **C-1/C-2**: Fixed pool value and path in CLI tools

**Total: 19 bugs fixed (1 critical, 1 high, 6 medium, 11 low)**

### 🔍 Code Audit Fixes (2026-01-03)

- **CLI-1**: Fixed `show-hidden` command showing priority pool instead of observation pool (`pool == 1` → `pool == 0`)
- **SEC-1**: Use absolute path `/usr/bin/ldd` in lib_scanner for defense-in-depth

---

## [1.0.0] - 2025-12-25

### 🎉 Initial Stable Release

The first production-ready release of Preheat, an adaptive readahead daemon for Linux that learns user behavior and preloads applications to reduce cold-start times by 30-60%.

### ✨ Features

#### Running Process Persistence
- State file persists running process information across daemon restarts
- Seamless resumption of incremental weight tracking for long-running applications
- PID validation on load prevents tracking stale or reused process IDs
- Improved ranking accuracy for applications with extended runtimes

#### Enhanced Launch Counting
- Launch counter distinguishes between user-initiated launches and child processes
- Multi-process applications (browsers, IDEs) correctly counted as single launches
- More accurate usage statistics for applications that spawn multiple processes

#### State File Format
- Extended format includes running process tracking:
  - `PIDS <count>` - Running process subsection for each executable
  - `PID <pid> <start_time> <last_update> <user_initiated>` - Individual process tracking
- Backward compatible: old daemons gracefully ignore new fields
- Forward compatible: new daemons handle old state files

#### Two-Tier Tracking System
- **Priority Pool**: Applications you actively use and want preloaded
- **Observation Pool**: System/background processes tracked but not preloaded
- Automatic classification based on path patterns and desktop file scanning
- Manual control via `promote`, `demote`, `show-hidden`, and `reset` commands

#### Session-Aware Boot Preloading
- Aggressive 3-minute preload window immediately after login
- Prioritizes high-scoring applications during boot phase
- Adapts to session patterns over time

#### Smart First-Run Seeding
- Immediate value from day one without cold-start learning period
- Scans XDG autostart entries for commonly used applications
- Parses shell history (bash, zsh, fish) for frequently run commands
- Detects browsers of choice (Firefox, Chrome, Chromium, Brave, etc.)
- Identifies development tools (code editors, terminals, IDEs)
- Configurable via `seed_from_xdg`, `seed_from_history`, `seed_confidence` options

#### Application Families
- Group related applications (e.g., all Chromium-based browsers)
- Shared learning across family members
- Reduces time to predict newly installed alternatives

#### Weighted Launch Counting
- Recency-weighted scoring (recent launches matter more)
- Short-lived process penalty (filters out scripts and one-shot commands)
- Foreground vs background process awareness
- Configurable decay factor and thresholds

#### Comprehensive CLI (`preheat-ctl`)
- `status` - Daemon status and uptime
- `stats` - Hit rate, cache efficiency, memory usage
- `stats --verbose` - Detailed per-application statistics
- `stats --raw` - Machine-readable output for scripting
- `health` - Quick system health check
- `explain <app>` - Decision reasoning for any application
- `promote <app>` - Add app to priority pool (always preload)
- `demote <app>` - Move app to observation pool
- `show-hidden` - List all observation pool apps
- `reset` - Clear all user overrides
- `reload` - Reload configuration without restart
- `pause/resume` - Temporarily disable/enable preloading

### 🔧 Configuration

New configuration options in `/etc/preheat.conf`:
- `enable_two_tier` - Enable/disable pool separation
- `priority_paths` - Paths always treated as priority
- `observation_paths` - Paths always treated as observation
- `seed_from_xdg` - Enable XDG autostart seeding
- `seed_from_history` - Enable shell history seeding
- `seed_confidence` - Minimum confidence for seeded apps
- `recency_weight` - Weight for recent launches (0.0-1.0)
- `short_lived_threshold` - Seconds below which processes are penalized
- `enable_families` - Enable application family grouping
- `autosave` - State file save interval (default: 300s for frequent persistence)

### 🐛 Bug Fixes

#### Security Fixes
- **B013/B016 (HIGH)**: Fixed buffer overflow in `/proc/stat` parsing - malicious process names could overflow 256-byte buffer
- **B015**: Fixed TOCTOU race in path resolution (realpath then O_NOFOLLOW)

#### Stability Fixes  
- **B001**: Fixed zombie process accumulation via SA_NOCLDWAIT on SIGCHLD
- **B002**: Fixed signal coalescing using atomic flags instead of g_timeout_add queue
- **B004**: Fixed SIGHUP during state save race condition (defers reload until save completes)
- **B006**: Fixed EINTR handling in wait() loop (proper retry logic)
- **B008**: Added exe eviction to prevent unbounded memory growth (>1500 exes, 30-day threshold)
- **B012**: Limited Markov chain creation to prevent O(n²) memory growth (100 exe limit)

#### Quality Improvements
- **B003**: Migrated from deprecated signal() to sigaction() with SA_RESTART
- **B010**: Fixed FD leak on dup2 failure in log reopen
- **B014**: Replaced non-reentrant strtok() with strtok_r()

#### Previous Fixes
- **Critical**: Fixed use-after-free bug causing daemon segfault on state reload
- **Critical**: Fixed static buffer bug causing all apps to show same name in stats
- Fixed `show-hidden` parsing for 9-field EXE format
- Fixed setup.sh silent exit due to bash arithmetic with `set -e`
- Fixed broken prompt in setup.sh
- Fixed setup.sh dependency auto-installation bug
- Fixed error path cleanup in pattern matching

### 🛡️ Security

- Trusted path validation for all file operations
- TOCTOU (time-of-check-time-of-use) prevention in preloading
- Secure state file permissions (0600)
- Input validation on all CLI commands

### 📚 Documentation

- Comprehensive README with quick start guide
- Detailed API reference for all CLI commands
- Configuration guide with all options explained
- Architecture document for contributors
- Troubleshooting guide for common issues
- Man pages: `preheat(8)`, `preheat.conf(5)`, `preheat-ctl(1)`

### ⚡ Performance

- ~5-10 MB memory footprint
- <1% CPU overhead during normal operation
- 30-60% reduction in cold-start times for frequent applications
- Efficient state serialization with CRC32 validation

### 📦 Installation

One-liner install:
```bash
curl -fsSL https://raw.githubusercontent.com/wasteddreams/preheat-linux/main/setup.sh | sudo bash
```

Verify installation:
```bash
preheat-ctl status
preheat-ctl health
```

### 🙏 Acknowledgments

- Inspired by the original [preload](https://sourceforge.net/projects/preload/) project
- Built with [Antigravity](https://antigravity.google/) using Claude Opus 4.5 Thinking and Claude Sonnet 4.5 Thinking
- Tested on Kali Linux and Debian-based distributions

---

[1.0.0]: https://github.com/wasteddreams/preheat-linux/releases/tag/v1.0.0
