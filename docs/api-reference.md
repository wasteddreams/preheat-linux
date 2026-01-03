# API Reference

Command-line interface and signal reference for preheat.

> **Note:** Preheat consists of two binaries:
> - **`preheat`** — The daemon that runs in the background and does the actual preloading
> - **`preheat-ctl`** — The control tool to manage, query, and configure the daemon

---

## preheat Daemon

### Synopsis

```
preheat [OPTIONS]
```

### Description

Adaptive readahead daemon that learns application usage patterns and preloads predicted applications into memory.

### Options

| Option | Long Form | Argument | Description |
|--------|-----------|----------|-------------|
| `-c` | `--conffile` | FILE | Configuration file path |
| `-s` | `--statefile` | FILE | State file path |
| `-l` | `--logfile` | FILE | Log file path |
| `-n` | `--nice` | LEVEL | Process nice level (0-19) |
| `-f` | `--foreground` | - | Run in foreground (don't daemonize) |
| `-t` | `--self-test` | - | Run diagnostics and exit |
| `-h` | `--help` | - | Show help and exit |
| `-v` | `--version` | - | Show version and exit |

### Default Paths

| File | Default Location |
|------|------------------|
| Configuration | `/usr/local/etc/preheat.conf` |
| State | `/usr/local/var/lib/preheat/preheat.state` |
| Log | `/usr/local/var/log/preheat.log` |
| PID | `/run/preheat.pid` |

### Examples

**Run normally (via systemd):**
```bash
sudo systemctl start preheat
```

**Run in foreground for debugging:**
```bash
sudo preheat -f
```

**Run with custom configuration:**
```bash
sudo preheat -c /path/to/custom.conf
```

**Run with custom state file:**
```bash
sudo preheat -s /tmp/preheat-test.state
```

**Run system diagnostics:**
```bash
preheat --self-test
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Normal exit (via signal) |
| 1 | Error (configuration, permissions, etc.) |

---

## preheat-ctl Control Tool

### Synopsis

```
preheat-ctl COMMAND
```

### Description

Command-line tool to control and query the running preheat daemon.

The `status`, `mem`, and `predict` commands work without root.
Signal commands (`reload`, `dump`, `save`, `stop`) require root privileges.

### Commands

#### status

Check if the daemon is running.

```bash
preheat-ctl status
```

**Output (running):**
```
preheat is running (PID 1234)
```

**Output (not running):**
```
preheat is not running
```

**Exit codes:**
- 0: Daemon is running
- 1: Daemon is not running or error

---

#### reload

Reload configuration file without restarting.

```bash
sudo preheat-ctl reload
```

**Effect:**
- Re-reads `/usr/local/etc/preheat.conf`
- Reopens log file (for log rotation)
- Does not clear learned state

**Equivalent signal:** SIGHUP

---

#### dump

Write state statistics to log file.

```bash
sudo preheat-ctl dump
```

**Effect:**
- Writes tracking statistics to `/usr/local/var/log/preheat.log`
- Useful for debugging and monitoring

**To view dumped data:**
```bash
sudo tail -100 /usr/local/var/log/preheat.log
```

**Equivalent signal:** SIGUSR1

---

#### save

Force immediate state file save.

```bash
sudo preheat-ctl save
```

**Effect:**
- Saves current state to `/usr/local/var/lib/preheat/preheat.state`
- Bypasses autosave timer
- Useful before system shutdown or upgrade

**Equivalent signal:** SIGUSR2

---

#### stop

Stop the daemon gracefully.

```bash
sudo preheat-ctl stop
```

**Effect:**
- Saves state
- Shuts down cleanly
- Waits up to 5 seconds for termination

**Equivalent signal:** SIGTERM

---

#### help

Display usage information.

```bash
preheat-ctl help
```

---

#### mem

Display system memory statistics.

```bash
preheat-ctl mem
```

**Output:**
```
Memory Statistics
=================
Total:       16384 MB
Free:         4000 MB
Available:    8000 MB
Buffers:       500 MB
Cached:       3500 MB

Usable for preloading: 8000 MB
```

**No root required.**

---

#### predict

Show top predicted applications from state file.

```bash
preheat-ctl predict
preheat-ctl predict --top 5
```

**Output:**
```
Top 10 Predicted Applications
=============================

 1. /usr/bin/firefox (run time: 45 sec)
 2. /usr/bin/code (run time: 30 sec)
...

Total tracked: 23 applications
```

**No root required.**

---

#### stats

Display preload statistics and hit rate.

```bash
sudo preheat-ctl stats
```

**Output:**
```
  Preheat Statistics
  ==================

  Uptime:       2h 30m
  Apps tracked: 45

  Preload Events:
    Total:   127
    Hits:    89
    Misses:  38

  Hit Rate:  70.1% (excellent)
```

**Requires root** (sends SIGUSR1 to daemon).

---

#### stats --verbose

Display extended statistics with detailed metrics.

```bash
sudo preheat-ctl stats --verbose
sudo preheat-ctl stats -v
```

**Output includes:**
- Daemon version and uptime
- Pool breakdown (priority vs observation)
- Memory metrics (total preloaded, pressure events)
- Top 20 apps table with weighted launches

---

#### promote

Add an application to the priority pool.

```bash
sudo preheat-ctl promote APP
sudo preheat-ctl promote firefox
sudo preheat-ctl promote /usr/bin/code
```

**Effect:**
- Adds app to `/etc/preheat.d/apps.list`
- Triggers automatic daemon reload
- App will be shown in stats and actively preloaded

**Requires root.**

---

#### demote

Move an application to the observation pool (blacklist).

```bash
sudo preheat-ctl demote APP
sudo preheat-ctl demote grep
```

**Effect:**
- Adds app to `/etc/preheat.d/blacklist`
- Triggers automatic daemon reload
- App will be tracked but hidden from stats and not preloaded

**Requires root.**

---

#### show-hidden

Display applications in the observation pool.

```bash
preheat-ctl show-hidden
```

**Output:**
```
Observation Pool Apps (hidden from stats):
==========================================

  /usr/bin/grep
  /usr/bin/cat
  /usr/bin/sed

Total: 3 apps
```

---

#### reset

Remove manual override for an application.

```bash
sudo preheat-ctl reset APP
sudo preheat-ctl reset firefox
```

**Effect:**
- Removes app from both apps.list and blacklist
- Returns app to automatic classification
- Triggers daemon reload

**Requires root.**

---

#### explain

Explain why an application is or isn't being preloaded.

```bash
preheat-ctl explain APP
preheat-ctl explain firefox
preheat-ctl explain /usr/bin/code
```

**Output includes:**
- Current status (PRELOADED / NOT PRELOADED / OBSERVATION POOL)
- Launch statistics (weighted, raw, runtime)
- Last seen / first seen timestamps
- Prediction scores (frequency, recency, combined)
- Decision reasoning and recommendations

---

#### health

Quick system health check with monitoring-friendly exit codes.

```bash
preheat-ctl health
```

**Exit codes:**
- `0`: Healthy (EXCELLENT or GOOD status)
- `1`: Degraded (issues detected)
- `2`: Critical (daemon not running)

**Output:**
```
✅ EXCELLENT - Preheat is operating optimally

  Daemon:       Running (PID 1234)
  Hit Rate:     72.5% (excellent)
  Uptime:       2d 5h

  Status: All systems operational
```

**Use cases:**
- Monitoring scripts
- Health checks in cron jobs
- Integration with Prometheus/alerting systems

---

#### pause

Temporarily pause preloading.

```bash
sudo preheat-ctl pause         # Default: 1 hour
sudo preheat-ctl pause 30m     # 30 minutes
sudo preheat-ctl pause 2h      # 2 hours
sudo preheat-ctl pause 1h30m   # 1 hour 30 minutes
sudo preheat-ctl pause until-reboot
```

**Effect:**
- Preloading stops immediately
- Pattern learning continues
- Pause state persists across daemon restarts
- Useful before gaming or benchmarks

**Requires root.**

---

#### resume

Resume preloading after pause.

```bash
preheat-ctl resume
```

**Effect:**
- Removes pause state
- Preloading resumes immediately

---

#### export

Export learned patterns to JSON file.

```bash
sudo preheat-ctl export                    # Default: preheat-profile.json
sudo preheat-ctl export ~/backup.json       # Custom path
```

**Use cases:**
- Backup before system reinstall
- Transfer patterns to another machine

**Requires root** (reads state file).

---

#### import

Validate and display apps from export file.

```bash
preheat-ctl import                    # Default: preheat-profile.json
preheat-ctl import ~/backup.json       # Custom path
```

---

#### update

Update preheat to latest version.

```bash
sudo preheat-ctl update
```

**Effect:**
- Backs up current state
- Downloads and installs latest version
- Automatic rollback on failure

**Requires root.**

---

#### help

Display usage information.

```bash
preheat-ctl help
```

---

### systemd Equivalents

| preheat-ctl | systemctl |
|-------------|-----------|
| `status` | `systemctl status preheat` |
| `reload` | `systemctl reload preheat` |
| `stop` | `systemctl stop preheat` |
| (n/a) | `systemctl start preheat` |
| (n/a) | `systemctl restart preheat` |

`preheat-ctl` provides faster, direct signal access for scripting.

---

## Signal Reference

The preheat daemon responds to the following signals:

### SIGHUP (1) - Reload

```bash
sudo kill -HUP $(cat /run/preheat.pid)
# Or: sudo preheat-ctl reload
```

**Actions:**
1. Reload configuration file
2. Reopen log file
3. Continue with new settings

**Use case:** After editing configuration or for log rotation.

---

### SIGUSR1 (10) - Dump State

```bash
sudo kill -USR1 $(cat /run/preheat.pid)
# Or: sudo preheat-ctl dump
```

**Actions:**
1. Write statistics to log file
2. Include tracked applications
3. Include Markov chain summary

**Use case:** Debugging, monitoring, verifying operation.

---

### SIGUSR2 (12) - Save State

```bash
sudo kill -USR2 $(cat /run/preheat.pid)
# Or: sudo preheat-ctl save
```

**Actions:**
1. Save state to state file immediately
2. Bypass autosave timer

**Use case:** Before shutdown, backup, or upgrade.

---

### SIGTERM (15) - Graceful Shutdown

```bash
sudo kill -TERM $(cat /run/preheat.pid)
# Or: sudo preheat-ctl stop
# Or: sudo systemctl stop preheat
```

**Actions:**
1. Stop monitoring loop
2. Save state to file
3. Clean up and exit

**Use case:** Normal shutdown.

---

### SIGINT (2) - Interrupt

```bash
# Ctrl+C when running in foreground
sudo preheat -f
^C
```

**Actions:** Same as SIGTERM.

---

## Log File Format

### Location

```
/usr/local/var/log/preheat.log
```

### Entry Format

```
[Day Mon DD HH:MM:SS YYYY] message
```

### Common Log Messages

**Startup:**
```
[Mon Dec 15 10:00:00 2024] loading configuration from /usr/local/etc/preheat.conf
[Mon Dec 15 10:00:00 2024] loading state from /usr/local/var/lib/preheat/preheat.state
[Mon Dec 15 10:00:00 2024] preheat 1.0.1 started
```

**Periodic scan:**
```
[Mon Dec 15 10:00:20 2024] 47 running processes, 23 tracked applications
```

**State operations:**
```
[Mon Dec 15 11:00:00 2024] saving state to /usr/local/var/lib/preheat/preheat.state
```

**Signals:**
```
[Mon Dec 15 12:00:00 2024] SIGHUP received - reloading configuration
[Mon Dec 15 12:00:00 2024] SIGUSR1 received - dumping state
[Mon Dec 15 12:00:00 2024] SIGUSR2 received - saving state
```

**Shutdown:**
```
[Mon Dec 15 18:00:00 2024] Exit signal received (15) - shutting down
[Mon Dec 15 18:00:00 2024] freeing state memory begin
```

---

## State File Format

### Location

```
/usr/local/var/lib/preheat/preheat.state
```

### Format

Binary format for efficient serialization. Contains:
- Tracked applications and their files
- Markov chain transition data
- Statistical counters

### Operations

**Backup:**
```bash
sudo cp /usr/local/var/lib/preheat/preheat.state /backup/preheat.state.bak
```

**Reset (clear learned data):**
```bash
sudo systemctl stop preheat
sudo rm /usr/local/var/lib/preheat/preheat.state
sudo systemctl start preheat
```

**Restore:**
```bash
sudo systemctl stop preheat
sudo cp /backup/preheat.state.bak /usr/local/var/lib/preheat/preheat.state
sudo systemctl start preheat
```

> **Note:** State file format may change between major versions. Backup before upgrading.

---

## systemd Service

### Unit File

Location: `/usr/lib/systemd/system/preheat.service`

### Commands

```bash
# Start
sudo systemctl start preheat

# Stop
sudo systemctl stop preheat

# Restart
sudo systemctl restart preheat

# Reload config
sudo systemctl reload preheat

# Enable at boot
sudo systemctl enable preheat

# Disable at boot
sudo systemctl disable preheat

# Check status
sudo systemctl status preheat

# View logs
sudo journalctl -u preheat
sudo journalctl -u preheat -f    # Follow
```

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Advanced Usage](advanced-usage.md) | [Documentation Index](index.md) | [Troubleshooting →](troubleshooting.md) |
