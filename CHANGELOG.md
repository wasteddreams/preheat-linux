# Changelog

All notable changes to Preheat will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026-01-03

### 🛡️ Security Fixes

#### Critical: Command Injection Vulnerability (L-1)
- **File:** `src/utils/lib_scanner.c`
- **Impact:** Malicious filenames containing shell metacharacters (`;`, `$()`, backticks) could execute arbitrary commands when the daemon invoked `ldd` via `popen()`
- **Fix:** All executable paths are now escaped using `g_shell_quote()` before passing to shell
- **Additional Hardening:** `ldd` is now invoked via absolute path `/usr/bin/ldd` to prevent PATH manipulation attacks

### 🐛 Bug Fixes

#### High Severity

**F-1: Use-After-Free in Statistics Module**
- **File:** `src/daemon/stats.c:578`
- **Issue:** Accessed `sorted->len` after calling `g_array_free(sorted, TRUE)`
- **Fix:** Save array length to local variable before freeing

#### Medium Severity

**M-1: Wrong Timestamp in Markov Chain Initialization**
- **File:** `src/state/state_markov.c:69`
- **Issue:** Used `a->change_timestamp` instead of `b->change_timestamp`, causing incorrect chain timing
- **Fix:** Now correctly uses the timestamp from executable B

**M-3: Assertion Crash on Same-State Transition**
- **File:** `src/state/state_markov.c:108`
- **Issue:** `g_assert(old_state != new_state)` crashed when rapid process start/stop caused same-state transitions
- **Fix:** Gracefully return instead of asserting

**F-2: App Families Loaded Before State Initialization**
- **File:** `src/config/config.c`
- **Issue:** Family configuration parsed before state structures were initialized
- **Fix:** Reordered initialization sequence in `main.c`

**F-3: Duplicate Family IDs Caused Memory Leak**
- **File:** `src/state/state_io.c:463`
- **Issue:** Loading duplicate family IDs from state file leaked the duplicate
- **Fix:** Check for existing family before inserting, free duplicate if found

**L-2: Integer Overflow in /proc Maps Parsing**
- **File:** `src/monitor/proc.c:209`
- **Issue:** If `end < start` in `/proc/PID/maps`, subtraction overflowed to huge value
- **Fix:** Validate `end > start` before calculating length

**SS-1: Smart Seeding Clobbered Prior Data**
- **Files:** `src/utils/seeding.c` (8 locations)
- **Issue:** Used `exe->weighted_launches = score` instead of `+=`, resetting scores from earlier seeding sources
- **Fix:** Changed all 8 assignments to use `+=` for proper accumulation

**MC-1/MC-2: Markov Chains Not Created After Seeding**
- **Files:** `src/state/state_markov.c`, `src/state/state_exe.c`
- **Issue:** B012 limit + FALSE passed to `kp_state_register_exe()` during seeding meant no Markov chains were ever created for seeded apps
- **Fix:** Added `kp_markov_build_priority_mesh()` function that builds chains between all priority pool apps after seeding completes

**SP-1: Session Preload Skipped Map Loading**
- **File:** `src/daemon/session.c:433`
- **Issue:** Checked if `exemaps` was empty instead of checking if `size` was below minimum
- **Fix:** Changed condition to check `exe->size < kp_conf->model.minsize`

#### Low Severity

**M-4: Division by Zero in Markov Correlation**
- **File:** `src/state/state_markov.c:228`
- **Fix:** Guard against zero/negative denominator, return neutral correlation

**M-5: Integer Overflow After Extended Uptime**
- **File:** `src/state/state.h:166`
- **Issue:** `int time` field would overflow after ~2 years of continuous uptime
- **Fix:** Changed to `int64_t` for effectively unlimited range

**M-6: Memory Leak on OOM During Markov Creation**
- **File:** `src/state/state_markov.c:82`
- **Fix:** Free allocated markov struct if sets are NULL

**L-3/L-4/L-5: Memory Safety in /proc Parsing**
- **File:** `src/monitor/proc.c`
- L-3: Memory leak when `fopen()` fails (moved allocation after success check)
- L-4: Uninitialized buffer (added `file[0] = '\0'`)
- L-5: Sign mismatch in length calculation (changed to `size_t`)

**S-1: Format Specifier Mismatch**
- **File:** `src/state/state_io.c`
- **Issue:** Used `%d` for `size_t` values
- **Fix:** Use `%zu` format specifier

**HM-1: Unsafe String Copy**
- **File:** `src/daemon/stats.c`
- **Fix:** Replaced `strncpy` with `g_strlcpy` for guaranteed null-termination

**C-1/C-2: CLI Tool Bugs**
- C-1: Pool value display inverted in `explain` output
- C-2: Wrong state file path in `health` command

**CLI-1: show-hidden Showed Wrong Pool**
- **File:** `tools/ctl_cmd_apps.c:421`
- **Issue:** Checked `pool == 1` (priority) but function is for observation pool
- **Fix:** Changed to `pool == 0`

### ✨ New Features

**SI-1: Single-Instance Protection**
- **File:** `src/daemon/main.c`
- **Feature:** Daemon now uses `flock()` on `/var/run/preheat.pid` to prevent multiple instances
- **Behavior:** Second instance immediately exits with clear error message showing existing PID

### 📊 Summary

| Severity | Count |
|----------|-------|
| Critical | 1 |
| High | 1 |
| Medium | 8 |
| Low | 11 |
| **Total** | **21 bugs fixed** |

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
