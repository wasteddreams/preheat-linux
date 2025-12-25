# Troubleshooting

Common problems and their solutions.

---

## Quick Diagnostics

Run these commands to gather diagnostic information:

```bash
# Service status
sudo systemctl status preheat

# Recent journal entries
sudo journalctl -u preheat -n 50

# Log file tail
sudo tail -50 /usr/local/var/log/preheat.log

# Process check
ps aux | grep preheat

# CLI status check
sudo preheat-ctl status
```

---

## Service Won't Start

### Problem: Service is masked

**Symptom:**
```
Failed to start preheat.service: Unit preheat.service is masked.
```

**Cause:** The service was previously masked (disabled completely).

**Solution:**
```bash
sudo systemctl unmask preheat
sudo systemctl enable preheat
sudo systemctl start preheat
```

---

### Problem: Permission denied

**Symptom:**
```
failed to create PID file /run/preheat.pid: Permission denied
```

**Cause:** Daemon not running as root.

**Solution:**
Preheat must run as root. Use systemd or sudo:
```bash
sudo systemctl start preheat
# Or:
sudo preheat -f
```

---

### Problem: Configuration file not found

**Symptom:**
```
cannot open configuration file: No such file or directory
```

**Cause:** Missing configuration file.

**Solution:**
```bash
# Check expected location
ls -la /usr/local/etc/preheat.conf

# Create from default
sudo cp /path/to/preheat-linux/config/preheat.conf.default \
    /usr/local/etc/preheat.conf
```

---

### Problem: Another instance running

**Symptom:**
```
PID file exists: /run/preheat.pid
```

**Cause:** Stale PID file from crashed daemon or another instance.

**Solution:**
```bash
# Check if actually running
ps aux | grep preheat

# If not running, remove stale PID file
sudo rm /run/preheat.pid
sudo systemctl start preheat
```

---

### Problem: Address already in use

**Symptom:** Daemon starts but exits immediately.

**Cause:** Previous instance didn't clean up properly.

**Solution:**
```bash
# Kill any remaining processes
sudo pkill preheat

# Remove PID file
sudo rm -f /run/preheat.pid

# Start fresh
sudo systemctl start preheat
```

---

## Service Starts but Doesn't Work

### Problem: Scanning disabled

**Symptom:** Daemon runs but log shows no activity.

**Check:**
```bash
grep "doscan\|dopredict" /usr/local/var/log/preheat.log
```

**Solution:**
In `/usr/local/etc/preheat.conf`:
```ini
[system]
doscan = true
dopredict = true
```

Then reload:
```bash
sudo preheat-ctl reload
```

---

### Problem: No applications tracked

**Symptom:** Log shows "0 tracked applications".

**Possible causes:**
1. Path filters too restrictive
2. `minsize` too high
3. Learning not started yet

**Check configuration:**
```bash
grep -E "exeprefix|mapprefix|minsize" /usr/local/var/log/preheat.log
```

**Solution:**
Adjust in `/usr/local/etc/preheat.conf`:
```ini
[model]
minsize = 1000000    # Lower threshold

[system]
exeprefix = /usr/;/opt/;!/    # Include more paths
```

---

### Problem: State file errors

**Symptom:**
```
cannot open state file for reading: No such file or directory
```

**Cause:** First run or state file deleted. This is normal on first startup.

**Solution:** Wait—the daemon will create a new state file on first autosave or shutdown.

---

### Problem: State file corruption

**Symptom:**
```
Error reading state file
```

**Solution:**
```bash
# Remove corrupt state (daemon will start fresh)
sudo systemctl stop preheat
sudo rm /usr/local/var/lib/preheat/preheat.state
sudo systemctl start preheat
```

> **Warning:** This loses learned patterns. Daemon will need to relearn.

---

## Performance Issues

### Problem: High CPU usage

**Symptom:** preheat using noticeable CPU.

**Check:**
```bash
top -p $(pidof preheat)
```

**Solutions:**

1. **Increase cycle time:**
   ```ini
   [model]
   cycle = 30   # Or higher
   ```

2. **Reduce parallelism:**
   ```ini
   [system]
   processes = 10
   ```

3. **Increase minimum size threshold:**
   ```ini
   [model]
   minsize = 5000000
   ```

---

### Problem: High memory usage

**Symptom:** Daemon using more than expected memory.

**Check:**
```bash
ps -o rss,vsz -p $(pidof preheat)
```

Normal range: 5-20 MB. If significantly higher:

**Solution:**
```bash
# Check state file size
ls -lh /usr/local/var/lib/preheat/preheat.state

# If very large (>50MB), reset
sudo systemctl stop preheat
sudo rm /usr/local/var/lib/preheat/preheat.state
sudo systemctl start preheat
```

---

### Problem: No improvement in startup times

**Possible causes:**

1. **Still learning:** Wait 1-2 days of normal use
2. **Unpredictable usage:** Pattern-based prediction can't help
3. **SSD storage:** Already fast, limited room for improvement
4. **Low memory:** Preloaded data evicted before use

**Diagnostics:**
```bash
# Check if predictions happening
sudo preheat-ctl dump
sudo grep "readahead" /usr/local/var/log/preheat.log | tail -10

# Check available memory
free -h
```

---

## Log File Issues

### Problem: Log file too large

**Symptom:** `/usr/local/var/log/preheat.log` growing unbounded.

**Solution:** Configure logrotate:

```bash
sudo nano /etc/logrotate.d/preheat
```

```
/usr/local/var/log/preheat.log {
    weekly
    rotate 4
    compress
    missingok
    notifempty
    postrotate
        /usr/bin/preheat-ctl reload > /dev/null 2>&1 || true
    endscript
}
```

---

### Problem: Log permission issues

**Symptom:**
```
Cannot open log file: Permission denied
```

**Solution:**
```bash
sudo mkdir -p /var/log
sudo touch /usr/local/var/log/preheat.log
sudo chown root:root /usr/local/var/log/preheat.log
sudo chmod 640 /usr/local/var/log/preheat.log
```

---

## Configuration Issues

### Problem: Changes not taking effect

**Symptom:** Edited config but behavior unchanged.

**Solution:** Reload configuration:
```bash
sudo preheat-ctl reload
# Or
sudo systemctl reload preheat
```

---

### Problem: Invalid configuration value

**Symptom:** Daemon uses defaults instead of configured values.

**Check:** View logged configuration:
```bash
sudo grep "# loaded configuration" -A 30 /usr/local/var/log/preheat.log
```

**Common issues:**
- Typos in key names
- Missing sections
- Invalid value types

---

## systemd Issues

### Problem: Service fails dependency

**Symptom:**
```
preheat.service: Job preheat.service/start failed with result 'dependency'.
```

**Solution:**
```bash
# Check what failed
sudo systemctl status preheat

# Check journal
sudo journalctl -u preheat -b

# Reload systemd
sudo systemctl daemon-reload
sudo systemctl start preheat
```

---

### Problem: Service constantly restarting

**Symptom:** Service restarts repeatedly.

**Check:**
```bash
sudo journalctl -u preheat -n 100
```

**Common causes:**
1. Configuration error
2. Missing state directory
3. Permission issues

**Solution:**
```bash
# Ensure directories exist
sudo mkdir -p /usr/local/var/lib/preheat
sudo mkdir -p /var/log

# Check permissions
ls -la /usr/local/var/lib/preheat
ls -la /usr/local/var/log/preheat.log
```

---

## Recovery Procedures

### Full Reset

Complete fresh start:

```bash
# Stop service
sudo systemctl stop preheat

# Remove all state
sudo rm -rf /usr/local/var/lib/preheat
sudo rm -f /usr/local/var/log/preheat.log
sudo rm -f /run/preheat.pid

# Restore default config
sudo cp /path/to/preheat-linux/config/preheat.conf.default \
    /usr/local/etc/preheat.conf

# Recreate directories
sudo mkdir -p /usr/local/var/lib/preheat

# Start fresh
sudo systemctl start preheat
```

---

### Emergency Debugging

If nothing else works:

```bash
# Stop any running instances
sudo systemctl stop preheat
sudo pkill preheat

# Clean state
sudo rm -f /run/preheat.pid

# Run in foreground with verbose output
sudo preheat -f

# Watch for errors and Ctrl+C to stop
```

---

## Getting Help

If problems persist:

1. **Check documentation:** [Documentation Index](index.md)
2. **Search issues:** [GitHub Issues](https://github.com/wasteddreams/preheat-linux/issues)
3. **File new issue:** Include:
   - System information (`uname -a`, `cat /etc/os-release`)
   - Configuration file
   - Relevant log excerpts
   - Steps to reproduce

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← API Reference](api-reference.md) | [Documentation Index](index.md) | [Examples →](examples/) |
