# Advanced Usage

Power-user features, performance tuning, and specialized configurations.

---

## Performance Tuning

### Understanding Your Workload

Before tuning, understand your usage patterns:

```bash
# What applications do you run most?
sudo preheat-ctl dump
sudo grep "tracked applications" /var/log/preheat.log

# How much memory is typically free?
free -h

# What storage type?
lsblk -d -o NAME,ROTA    # 1 = HDD, 0 = SSD
```

### Tuning for HDD Systems

Mechanical drives benefit most from preheat due to slow seek times.

```ini
[model]
cycle = 20
memfree = 60        # Use more free memory
memtotal = -5       # Smaller safety margin

[system]
sortstrategy = 3    # Block sorting crucial for HDD
processes = 20      # HDDs saturate at lower parallelism
```

### Tuning for SSD/NVMe Systems

SSDs have fast random access, reducing preheat's benefit.

```ini
[model]
cycle = 30          # Less frequent (SSDs are fast anyway)
memfree = 40        # More conservative

[system]
sortstrategy = 0    # No sorting needed
processes = 50      # SSDs handle high parallelism
```

### Tuning for Low-Memory Systems (2-4 GB)

When RAM is scarce, be conservative:

```ini
[model]
cycle = 30          # Less frequent to reduce overhead
minsize = 5000000   # Only track larger apps
memtotal = -20      # Larger safety margin
memfree = 25        # Less aggressive

[system]
processes = 5       # Minimal parallelism
autosave = 7200     # Less frequent saves
```

### Tuning for High-Memory Systems (16+ GB)

With ample RAM, be more aggressive:

```ini
[model]
cycle = 15          # More responsive
minsize = 1000000   # Track smaller apps too
memtotal = 0        # No subtraction
memfree = 80        # Use most of free memory
memcached = 10      # Even borrow from cache

[system]
processes = 60      # High parallelism
autosave = 1800     # More frequent saves
```

---

## Virtual Machine Considerations

### Running Preheat in a VM

VMs present unique challenges:

| Factor | Impact | Mitigation |
|--------|--------|------------|
| Virtual disk I/O | Already cached by host | May have reduced benefit |
| Memory ballooning | Can evict preheat's work | Use conservative settings |
| Shared resources | Other VMs compete | Lower priority |

**Recommended VM configuration:**

```ini
[model]
cycle = 30
memtotal = -15
memfree = 30

[system]
processes = 10
```

### Running on the Host (with VMs)

If the host runs VMs, reserve memory:

```ini
[model]
memtotal = -30      # Leave room for VM memory
memfree = 20        # Very conservative
```

---

## Manual Application Lists

### Creating a Whitelist

Force specific applications to always be preloaded:

```bash
# Create the list file
sudo mkdir -p /etc/preheat.d
sudo nano /etc/preheat.d/apps.list
```

**Example whitelist:**
```
# /etc/preheat.d/apps.list
# Priority applications to always preload
# One absolute path per line

# Browsers
/usr/bin/firefox
/usr/bin/chromium

# Development
/usr/bin/code
/usr/bin/vim

# Office
/usr/lib/libreoffice/program/soffice.bin
```

**Enable in configuration:**
```ini
[system]
manualapps = /etc/preheat.d/apps.list
```

### Creating a Blacklist

Prevent specific applications from being tracked or preloaded:

```bash
sudo nano /etc/preheat.d/blacklist
```

**Example blacklist:**
```
# /etc/preheat.d/blacklist
# Applications to never track

# Gaming (too large, infrequent)
/usr/games/heavy-game

# Broken applications
/opt/buggy-app/bin/app
```

**Enable in configuration:**
```ini
[preheat]
blacklist = /etc/preheat.d/blacklist
```

---

## Debugging Techniques

### Running in Foreground

See real-time activity:

```bash
# Stop the service
sudo systemctl stop preheat

# Run interactively
sudo preheat -f

# Or with custom config
sudo preheat -f -c /tmp/test-config.conf
```

Press Ctrl+C to stop.

### Log Level Analysis

The log file contains detailed information:

```bash
# View startup configuration
sudo grep "# loaded configuration" -A 50 /var/log/preheat.log

# View scanning activity
sudo grep -E "running processes|tracked applications" /var/log/preheat.log | tail -20

# View state saves
sudo grep "saving state" /var/log/preheat.log

# View errors
sudo grep -i "error\|fail\|cannot" /var/log/preheat.log
```

### State Dump Analysis

Force a state dump and analyze:

```bash
# Trigger dump
sudo preheat-ctl dump

# View dumped statistics
sudo tail -100 /var/log/preheat.log
```

### Memory Usage Monitoring

Track preheat's resource consumption:

```bash
# Memory usage
ps aux | grep preheat

# Detailed memory map
sudo pmap $(pidof preheat)

# Watch in real-time
watch -n 5 'ps -o pid,rss,vsz,comm -p $(pidof preheat)'
```

---

## Monitoring and Metrics

### Key Metrics to Watch

```bash
# Script to gather preheat metrics
#!/bin/bash

echo "=== Preheat Metrics ==="
echo

# Daemon status
if sudo preheat-ctl status >/dev/null 2>&1; then
    echo "Status: Running"
    echo "PID: $(cat /run/preheat.pid)"
else
    echo "Status: Not running"
fi
echo

# Memory usage
echo "Memory Usage:"
ps -o rss,vsz -p $(pidof preheat) 2>/dev/null | tail -1 | \
    awk '{printf "  RSS: %d MB\n  VSZ: %d MB\n", $1/1024, $2/1024}'
echo

# State file size
echo "State File:"
ls -lh /var/lib/preheat/preheat.state 2>/dev/null | awk '{print "  Size: " $5}'
echo

# Recent activity
echo "Recent Activity (last 5 scans):"
sudo grep "running processes" /var/log/preheat.log | tail -5
```

### Integration with Monitoring Systems

For Prometheus/Grafana, parse log metrics:

```bash
# Tracked applications count
grep "tracked applications" /var/log/preheat.log | tail -1 | \
    awk '{print $NF}'

# Running processes seen
grep "running processes" /var/log/preheat.log | tail -1 | \
    awk -F',' '{print $1}' | awk '{print $NF}'
```

---

## Integration with Other Tools

### With ananicy-cpp (Process Priority)

Ananicy adjusts process nice levels. Compatible with preheat:

```bash
# Both can run together
# Ananicy prioritizes running apps
# Preheat preloads predicted apps
```

### With earlyoom (Memory Management)

Earlyoom kills processes under memory pressure:

```ini
# Configure preheat conservatively to leave room
[model]
memtotal = -20
memfree = 30
```

### With zram (Compressed Swap)

Zram compresses swap in RAM. Works well with preheat:
- Preheat fills cache with useful data
- Zram handles memory pressure gracefully

---

## Security Considerations

### Process Information Exposure

Preheat reads `/proc`, which exposes:
- Process paths
- Memory maps
- User activity patterns

**Mitigations:**
- State file has restricted permissions
- Daemon runs as root (required for readahead)
- No network exposure

### File Permissions

Ensure proper permissions:

```bash
# State file
sudo chmod 600 /var/lib/preheat/preheat.state

# Log file  
sudo chmod 640 /var/log/preheat.log

# Config file
sudo chmod 644 /etc/preheat.conf
```

### Audit Trail

Log rotation preserves history:

```bash
# Check logrotate config
cat /etc/logrotate.d/preheat
```

---

## When NOT to Use Preheat

### Scenarios Where Preheat Provides Little Benefit

| Scenario | Reason |
|----------|--------|
| **Modern NVMe SSD** | Already very fast startup |
| **Server workloads** | Services run continuously |
| **Highly unpredictable usage** | Cannot learn patterns |
| **Very low memory (<2 GB)** | No headroom for preloading |

### Temporarily Disabling

```bash
# Stop without disabling (resumes on reboot)
sudo systemctl stop preheat

# Disable completely
sudo systemctl disable preheat
sudo systemctl stop preheat
```

### Using Monitoring-Only Mode

Learn patterns without preloading:

```ini
[system]
doscan = true       # Keep learning
dopredict = false   # Don't preload
```

---

## Troubleshooting Performance Issues

### High CPU Usage

**Symptoms:** Elevated CPU during preheat cycles

**Possible causes and fixes:**

| Cause | Fix |
|-------|-----|
| Cycle too short | Increase `cycle` to 30+ |
| Too many processes | Reduce `processes` |
| Large state file | Consider clearing state |

### High Memory Usage

**Symptoms:** Preheat daemon itself using too much memory

**Possible causes and fixes:**

| Cause | Fix |
|-------|-----|
| Tracking many apps | Increase `minsize` |
| Large Markov chain | Normal for active use |
| Memory leak (bug) | Report issue, restart daemon |

### No Improvement in Launch Times

**Possible causes:**

1. **Learning period**: Wait 1-2 days
2. **Unpredictable usage**: Pattern-based prediction won't help
3. **Already fast storage**: SSD may not benefit significantly
4. **Memory eviction**: Other processes using cached data

**Diagnosis:**
```bash
# Check if predictions are being made
sudo grep -i "readahead\|preload" /var/log/preheat.log | tail -20

# Check if apps are in cache before launch
vmtouch /usr/bin/your-app   # Requires vmtouch package
```

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Configuration](configuration.md) | [Documentation Index](README.md) | [API Reference →](api-reference.md) |
