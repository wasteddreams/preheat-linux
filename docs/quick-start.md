# Quick Start Guide

Get preheat running and verified in under 5 minutes.

---

## Prerequisites

- Preheat is installed (see [Installation](installation.md))
- You have root/sudo access
- systemd is your init system

---

## Step 1: Check Service Status

First, verify preheat is running:

```bash
sudo systemctl status preheat
```

**Expected output (service running):**
```
● preheat.service - Adaptive readahead daemon
     Loaded: loaded (/usr/lib/systemd/system/preheat.service; enabled)
     Active: active (running) since Mon 2024-12-15 10:00:00 UTC
   Main PID: 1234 (preheat)
     Memory: 5.2M
```

**If not running, start it:**
```bash
sudo systemctl start preheat
```

**Or first verify system requirements:**
```bash
preheat --self-test
```

---

## Step 2: Verify Daemon Operation

Check the log file to confirm normal operation:

```bash
sudo tail -30 /var/log/preheat.log
```

**Healthy log output looks like:**
```
[Mon Dec 15 10:00:00 2024] loading configuration from /usr/local/etc/preheat.conf
[Mon Dec 15 10:00:00 2024] loading state from /usr/local/var/lib/preheat/preheat.state
[Mon Dec 15 10:00:00 2024] preheat 0.1.0 started
```

After a few cycles (20 seconds each by default), you'll see:
```
[Mon Dec 15 10:00:20 2024] 47 running processes, 23 tracked applications
```

---

## Step 3: Use the CLI Tool

Check daemon status programmatically:

```bash
preheat-ctl status
```

**Output:**
```
preheat is running (PID 1234)
```

Dump current statistics to the log:

```bash
sudo preheat-ctl dump
```

Then view:
```bash
sudo tail -50 /var/log/preheat.log
```

---

## Step 4: Understand the Learning Period

Preheat needs time to learn your usage patterns:

| Timeframe | What Happens |
|-----------|--------------|
| **First hour** | Building initial model, minimal predictions |
| **First day** | Learning primary applications |
| **First week** | Understanding daily patterns |
| **Ongoing** | Continuously refining predictions |

**During the learning period:**
- The daemon runs normally
- It monitors and records application launches
- Predictions improve automatically over time
- You don't need to do anything special

---

## Step 5: Verify Preloading is Working

After at least an hour of normal use, check if predictions are happening:

```bash
# Force a state dump
sudo preheat-ctl dump

# Check the log for prediction activity  
sudo grep -i "readahead\|preload\|predict" /var/log/preheat.log | tail -20
```

You should see entries indicating file preloading activity.

---

## Quick Command Reference

| Task | Command |
|------|---------|
| Run diagnostics | `preheat --self-test` |
| Check if running | `preheat-ctl status` |
| View memory stats | `preheat-ctl mem` |
| View tracked apps | `preheat-ctl predict` |
| Start service | `sudo systemctl start preheat` |
| Stop service | `sudo systemctl stop preheat` |
| Restart service | `sudo systemctl restart preheat` |
| View live logs | `sudo tail -f /var/log/preheat.log` |
| Dump stats to log | `sudo preheat-ctl dump` |
| Force save state | `sudo preheat-ctl save` |
| Reload config | `sudo preheat-ctl reload` |

---

## Default Behavior Explained

Out of the box, preheat uses safe, conservative defaults:

| Setting | Default | Meaning |
|---------|---------|---------|
| Scan cycle | 20 seconds | How often it checks running processes |
| Memory usage | ~25% of free RAM | Limit on preloading activity |
| Autosave | 1 hour | How often state is saved to disk |
| Sort strategy | Block-based | Optimized for HDD seek reduction |

These defaults:
- Work well on most systems
- Prioritize safety over aggressiveness
- Require no tuning for basic operation

---

## What to Expect

### Immediate Effects
- Daemon starts and begins monitoring
- Log file shows activity
- Memory usage increases slightly (5-10 MB daemon overhead)

### After Learning (Hours to Days)
- Frequently used applications start faster
- Most noticeable on HDDs
- Improvement varies by workflow predictability

### Typical Results

| Application Type | Improvement |
|------------------|-------------|
| Daily-use browsers | 40-60% faster cold start |
| Code editors/IDEs | 30-50% faster cold start |
| Office applications | 40-60% faster cold start |
| Rarely-used apps | Little to no improvement |

---

## Quick Troubleshooting

### Service won't start

```bash
# Check for errors
sudo journalctl -u preheat -n 50

# Check if something else uses the PID file
ls -la /run/preheat.pid

# Try running in foreground for debugging
sudo preheat -f
```

### Log shows errors

```bash
# Check configuration syntax
sudo preheat --help

# Verify configuration file
cat /usr/local/etc/preheat.conf
```

### No improvement noticed

- **Give it time**: Learning takes hours to days
- **Check if running**: `sudo preheat-ctl status`
- **Your workflow may be unpredictable**: Random app usage defeats prediction

See [Troubleshooting](troubleshooting.md) for detailed problem resolution.

---

## Next Steps

Now that preheat is running:

1. **[How It Works](how-it-works.md)** - Understand the daemon's behavior
2. **[Configuration](configuration.md)** - Tune for your specific needs
3. **[Advanced Usage](advanced-usage.md)** - Power-user optimizations

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Installation](installation.md) | [Documentation Index](README.md) | [How It Works →](how-it-works.md) |
