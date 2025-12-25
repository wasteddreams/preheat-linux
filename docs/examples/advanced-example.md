# Advanced Example: Optimized Configuration

Power-user configurations for specific scenarios.

---

## Scenario 1: Developer Workstation

**System:** 16 GB RAM, NVMe SSD, heavy IDE usage

**Goals:**
- Prioritize development tools
- Fast VS Code and browser loading
- Minimal overhead since SSD is already fast

### Configuration

```ini
# /etc/preheat.conf - Developer Workstation

[model]
# Slightly less frequent since SSD is fast
cycle = 25

# Use correlation for better predictions
usecorrelation = true

# Track smaller apps too
minsize = 1000000

# Moderate memory usage (SSD benefit is smaller)
memtotal = -10
memfree = 40
memcached = 0

[system]
doscan = true
dopredict = true
autosave = 3600

# No sorting needed for SSD
sortstrategy = 0

# High parallelism (SSD handles it)
processes = 50

# Path filters
mapprefix = /usr/;/lib;/opt/;/var/cache/;!/
exeprefix = /usr/;/opt/;!/

# Whitelist common dev tools
manualapps = /etc/preheat.d/dev-apps.list
```

### Whitelist File

```bash
# /etc/preheat.d/dev-apps.list
# Priority development tools

# Editors
/usr/bin/code
/usr/bin/vim
/usr/bin/nvim

# Browsers (for testing)
/usr/bin/firefox
/usr/bin/chromium

# Development tools
/usr/bin/docker
/usr/bin/git
/usr/bin/node
/usr/bin/python3
```

---

## Scenario 2: Low-Memory Laptop

**System:** 4 GB RAM, HDD, general use

**Goals:**
- Conservative memory usage
- Don't compete with applications for RAM
- Maximize HDD benefit

### Configuration

```ini
# /etc/preheat.conf - Low Memory System

[model]
# Less frequent to reduce overhead
cycle = 30

usecorrelation = true

# Only track substantial applications
minsize = 5000000

# Very conservative memory usage
memtotal = -25
memfree = 20
memcached = 0

[system]
doscan = true
dopredict = true

# Less frequent saves
autosave = 7200

# Block sorting critical for HDD
sortstrategy = 3

# Limited parallelism
processes = 5

# Minimal paths
mapprefix = /usr/;/lib;!/
exeprefix = /usr/;!/
```

---

## Scenario 3: Security/Pentesting Workstation

**System:** Linux with security tools, HDD

**Goals:**
- Preload common security tools
- Learn pentesting workflow patterns
- Handle large tools like Burp Suite

### Configuration

```ini
# /etc/preheat.conf - Kali Security Testing

[model]
cycle = 20
usecorrelation = true
minsize = 2000000

# Moderate memory (leave room for VMs)
memtotal = -15
memfree = 40
memcached = 0

[system]
doscan = true
dopredict = true
autosave = 3600
sortstrategy = 3
processes = 20

# Include Kali tool paths
mapprefix = /usr/;/lib;/opt/;!/
exeprefix = /usr/;/opt/;!/

# Security tools whitelist
manualapps = /etc/preheat.d/kali-tools.list
```

### Whitelist File

```bash
# /etc/preheat.d/kali-tools.list
# Security tools priority list

# Browsers
/usr/bin/firefox-esr

# Terminal tools
/usr/bin/tmux
/usr/bin/zsh

# Common assessment tools
/usr/bin/nmap
/usr/bin/gobuster
/usr/bin/sqlmap

# Note: Burp Suite will be auto-learned
# if you use it frequently
```

---

## Scenario 4: Monitoring Without Preloading

**Use case:** Learn about your application usage without preloading.

### Configuration

```ini
# /etc/preheat.conf - Monitor Only Mode

[model]
cycle = 20
minsize = 1000000

[system]
doscan = true
dopredict = false    # Learn but don't preload
autosave = 1800      # More frequent saves for data collection
```

### Analysis

After running for a few days:
```bash
# Dump comprehensive statistics
sudo preheat-ctl dump
sudo tail -200 /usr/local/var/log/preheat.log

# See what would be predicted
# Look for "tracked applications" entries
```

---

## Creating a Custom Whitelist

### Step 1: Identify Your Top Applications

```bash
# What are you running most?
# (Run this over a few days)
grep "tracked applications" /usr/local/var/log/preheat.log

# Or check history
history | awk '{print $2}' | sort | uniq -c | sort -rn | head -20
```

### Step 2: Find Full Paths

```bash
which firefox
# /usr/bin/firefox

which code
# /usr/bin/code
```

### Step 3: Create Whitelist

```bash
sudo mkdir -p /etc/preheat.d
sudo nano /etc/preheat.d/apps.list
```

Add one path per line:
```
/usr/bin/firefox
/usr/bin/code
/usr/bin/thunderbird
```

### Step 4: Enable in Config

```ini
[system]
manualapps = /etc/preheat.d/apps.list
```

### Step 5: Reload

```bash
sudo preheat-ctl reload
```

---

## Creating a Blacklist

### Use Cases

- Exclude large games (too slow to preload)
- Exclude broken applications
- Exclude sensitive applications

### Steps

```bash
# Create blacklist
sudo nano /etc/preheat.d/blacklist
```

```
# Applications to never track
/usr/games/heavy-game
/opt/problematic/app
```

Enable in config:
```ini
[preheat]
blacklist = /etc/preheat.d/blacklist
```

---

## Performance Monitoring Script

Create a script to monitor preheat effectiveness:

```bash
#!/bin/bash
# /usr/local/bin/preheat-stats

echo "=== Preheat Statistics ==="
echo

# Status
if sudo preheat-ctl status > /dev/null 2>&1; then
    echo "Status: Running (PID $(cat /run/preheat.pid))"
else
    echo "Status: NOT RUNNING"
    exit 1
fi
echo

# Memory
echo "Daemon Memory:"
ps -o rss,vsz,pmem -p $(pidof preheat) | tail -1 | \
    awk '{printf "  RSS: %.1f MB, VSZ: %.1f MB, %%MEM: %s\n", $1/1024, $2/1024, $3}'
echo

# State file
echo "State File:"
ls -lh /usr/local/var/lib/preheat/preheat.state 2>/dev/null | awk '{print "  " $5}'
echo

# Activity
echo "Recent Activity:"
sudo grep "running processes" /usr/local/var/log/preheat.log | tail -5 | \
    awk '{print "  " $0}'
echo

# Uptime
echo "Daemon Uptime:"
ps -o etime -p $(pidof preheat) | tail -1 | awk '{print "  " $1}'
```

Make executable:
```bash
sudo chmod +x /usr/local/bin/preheat-stats
```

Use:
```bash
sudo preheat-stats
```

---

## Integration with Shell

### Automatic Dump Before Shutdown

Add to `/etc/rc0.d/K01preheat`:
```bash
#!/bin/bash
preheat-ctl save
```

### Alias for Quick Status

Add to `~/.bashrc` or `~/.zshrc`:
```bash
alias pstatus='sudo preheat-ctl status'
alias pdump='sudo preheat-ctl dump && sudo tail -100 /usr/local/var/log/preheat.log'
```

---

## Best Practices Summary

| Scenario | Key Settings |
|----------|--------------|
| HDD system | `sortstrategy = 3`, `processes = 20` |
| SSD system | `sortstrategy = 0`, `processes = 50` |
| Low RAM (<4GB) | `memfree = 20`, `memtotal = -25` |
| High RAM (>8GB) | `memfree = 60`, `memtotal = 0` |
| Predictable workflow | Default config, add whitelist |
| Unpredictable workflow | Consider not using preheat |

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Basic Example](basic-example.md) | [Examples Index](./) | [Documentation Index →](../index.md) |
