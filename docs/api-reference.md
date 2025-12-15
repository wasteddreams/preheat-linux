# API Reference

Command-line interface and signal reference for preheat.

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

Command-line tool to control and query the running preheat daemon. Requires root privileges.

### Commands

#### status

Check if the daemon is running.

```bash
sudo preheat-ctl status
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
- Re-reads `/etc/preheat.conf`
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
- Writes tracking statistics to `/var/log/preheat.log`
- Useful for debugging and monitoring

**To view dumped data:**
```bash
sudo tail -100 /var/log/preheat.log
```

**Equivalent signal:** SIGUSR1

---

#### save

Force immediate state file save.

```bash
sudo preheat-ctl save
```

**Effect:**
- Saves current state to `/var/lib/preheat/preheat.state`
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
/var/log/preheat.log
```

### Entry Format

```
[Day Mon DD HH:MM:SS YYYY] message
```

### Common Log Messages

**Startup:**
```
[Mon Dec 15 10:00:00 2024] loading configuration from /etc/preheat.conf
[Mon Dec 15 10:00:00 2024] loading state from /var/lib/preheat/preheat.state
[Mon Dec 15 10:00:00 2024] preheat 0.1.0 started
```

**Periodic scan:**
```
[Mon Dec 15 10:00:20 2024] 47 running processes, 23 tracked applications
```

**State operations:**
```
[Mon Dec 15 11:00:00 2024] saving state to /var/lib/preheat/preheat.state
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
/var/lib/preheat/preheat.state
```

### Format

Binary format for efficient serialization. Contains:
- Tracked applications and their files
- Markov chain transition data
- Statistical counters

### Operations

**Backup:**
```bash
sudo cp /var/lib/preheat/preheat.state /backup/preheat.state.bak
```

**Reset (clear learned data):**
```bash
sudo systemctl stop preheat
sudo rm /var/lib/preheat/preheat.state
sudo systemctl start preheat
```

**Restore:**
```bash
sudo systemctl stop preheat
sudo cp /backup/preheat.state.bak /var/lib/preheat/preheat.state
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
| [← Advanced Usage](advanced-usage.md) | [Documentation Index](README.md) | [Troubleshooting →](troubleshooting.md) |
