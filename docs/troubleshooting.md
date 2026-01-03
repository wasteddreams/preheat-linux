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

### Problem: Another instance running

**Symptom (v1.0.1+):**
```
Error: Another instance is already running (PID: 1234)
```

**Cause:** The daemon uses `flock()` on the PID file to ensure only one instance runs at a time.

**Solution:**
```bash
# Check if actually running
ps aux | grep preheat

# If running, stop it first
sudo systemctl stop preheat

# If not running but error persists, the lock auto-releases
# Just start normally
sudo systemctl start preheat
```

> **Note (v1.0.1):** Unlike v1.0.0, you no longer need to manually delete the PID file. The `flock()` lock is automatically released when the process exits, even after a crash.

---

### Problem: Address already in use (Legacy v1.0.0)

**Symptom:** Daemon starts but exits immediately with stale PID file error.

**Cause:** Previous instance didn't clean up properly (pre-v1.0.1 behavior).

**Solution (if upgrading from v1.0.0):**
```bash
# Kill any remaining processes
sudo pkill preheat

# Remove stale PID file (only needed once after upgrade)
sudo rm -f /run/preheat.pid

# Start fresh - v1.0.1 handles this automatically going forward
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

### Problem: Snap/Docker/Firejail apps not tracked ⚠️

**Symptom:** Apps installed via snap or run in containers/sandboxes show "NOT TRACKED".

```bash
$ preheat-ctl explain firefox
Status: ❌ NOT TRACKED
```

**Cause:** Security sandboxes (Snap, Docker, Firejail) block daemon access to `/proc/PID/exe`.

**Verification:**
```bash
# Check if Firefox is a snap
which firefox
# Output: /snap/bin/firefox → Snap!

# The daemon sees this:
$ sudo readlink /proc/$(pgrep -n firefox)/exe
readlink: permission denied  # Blocked by AppArmor!
```

**This is a known limitation.** See [Known Limitations](known-limitations.md#4-sandboxed-applications-not-fully-supported-️).

**Workaround:**
- Install apps via `apt` instead of snap:
  ```bash
  sudo snap remove firefox
  sudo add-apt-repository ppa:mozillateam/ppa
  sudo apt update && sudo apt install firefox
  ```
- Use AppImage versions (no sandbox)
- Accept that sandboxed apps won't be preloaded

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

## Application-Specific Issues (v1.0.0+)

### Problem: Manual apps not being preloaded

**Symptom:** Apps in `/etc/preheat.d/apps.list` aren't showing up in predictions or getting preloaded.

**Diagnostics:**
```bash
# Check if manual apps were loaded
sudo grep "manual app" /usr/local/var/log/preheat.log

# Verify app is tracked
sudo preheat-ctl stats | grep vim

# Check prediction explicitly
sudo preheat-ctl explain vim.basic
```

**Solutions:**

1. **Config field mismatch:**
   
   In `/usr/local/etc/preheat.conf`, use the `system` section:
   ```ini
   [system]
   manualapps = /etc/preheat.d/apps.list
   ```
   
   **Not** `manual_apps_list` (that's a legacy field).

2. **File doesn't exist or wrong permissions:**
   ```bash
   # Check file exists
   ls -la /etc/preheat.d/apps.list
   
   # Fix permissions if needed
   sudo chmod 644 /etc/preheat.d/apps.list
   ```

3. **Paths not absolute:**
   
   Apps list must contain absolute paths:
   ```
   # CORRECT:
   /usr/bin/vim
   /usr/bin/code
   
   # WRONG:
   vim
   ~/myapp
   ```

4. **Daemon not reloaded:**
   ```bash
   # After editing apps.list, reload
   sudo preheat-ctl reload
   
   # Or restart
   sudo systemctl restart preheat
   ```

5. **Maps not loading (check logs):**
   ```bash
   # Enable debug logging in preheat.conf
   [system]
   # loglevel = debug  # Not yet implemented
   
   # Check for map loading errors
   sudo grep -i "load.*map\|manual" /usr/local/var/log/preheat.log | tail -20
   ```

**Expected behavior:**
- Manual apps register at daemon startup
- Maps loaded lazily during first prediction cycle
- Always bypass 0.30 threshold (get `-10.0` boost)

---

### Problem: Launch counts seem wrong

**Symptom:** `preheat-ctl stats` shows much higher `raw_launches` than expected.

**Example:**
```
$ sudo preheat-ctl stats
...
45    code     18.2    12   priority   # I only opened VS Code once!
```

**This is usually NORMAL!** Here's why:

**Cause 1: Multi-Process Applications**

Modern apps (Electron, Chromium, Firefox) spawn multiple processes:
```
VS Code launch:
  1 main process      → raw_launches++
  5 renderer processes → raw_launches++ (×5)
  1 GPU process        → raw_launches++
  1 file watcher      → raw_launches++
  
  Total: 8 processes = 8 raw launches for ONE app start
```

**Why it's acceptable:**
- `weighted_launches` uses logarithmic scaling
- Short-lived helpers get 0.3x penalty (Issue #2)
- Net result: ~1.5-2x inflation instead of 8x

**Cause 2: Process Reuse Windows**

Some apps open new windows without new processes:
```
firefox              # First window: counted ✅
firefox -new-window  # Reuses process: NOT counted ❌
```

This is a [known limitation](known-limitations.md#1-process-reuse-not-detected).

**Verification:**
```bash
# Check weighted vs raw
sudo preheat-ctl explain code

# Expected:
#   Weighted Launches:  4.2
#   Raw Launches:       8
#   (weighted is ~50% of raw = logarithmic damping working)
```

**Action:** No fix needed - this is by design!

---

### Problem: App shows "NOT PRELOADED" despite high score

**Symptom:**
```
$ sudo preheat-ctl explain firefox
Decision: ❌ Not Preloaded
  Reason: Insufficient usage frequency
  Score: 0.28 (threshold: 0.30)
```

**Causes:**

1. **Just below threshold:**
   - Score of 0.28 vs 0.30 threshold
   - Launch a few more times to cross threshold

2. **Short-lived penalty kicking in:**
   - If app crashes frequently (<5s runtime)
   - Gets 0.3x penalty, lowering score
   
   **Solution:** Fix the crashes or add to manual apps list

3. **Not enough usage history:**
   - First 1-2 days have limited predictions
   - Wait for more data or use manual apps

**Workaround:** Add to manual apps for immediate preloading:
```bash
echo "/usr/bin/firefox" | sudo tee -a /etc/preheat.d/apps.list
sudo preheat-ctl reload
```

---

##Log File Issues

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
