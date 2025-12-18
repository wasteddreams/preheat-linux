# Installation Guide

## Prerequisites

### Required Packages
```bash
sudo apt-get install autoconf automake pkg-config libglib2.0-dev
```

### Minimum Requirements
- Linux kernel 2.6.18+ (for `readahead()` syscall)
- GLib 2.16 or later
- Root privileges for installation and execution
- 256MB+ RAM recommended

---

## Build from Source

### 1. Download Source
```bash
cd /path/to/preheat
```

### 2. Generate Build System
```bash
autoreconf --install --force
```

### 3. Configure
```bash
# Standard installation
./configure

# Custom prefix
./configure --prefix=/usr

# With debug symbols
./configure --enable-debug
```

### 4. Compile
```bash
make
```

### 5. Verify Build
```bash
./src/preheat --version
./tools/preheat-ctl help
```

Expected output:
```
preheat 0.1.0
Adaptive readahead daemon for Debian-based distributions
Based on the preload daemon
```

---

## System Installation

### Install Files
```bash
sudo make install
```

This installs:
- `/usr/local/sbin/preheat` - Daemon binary
- `/usr/local/sbin/preheat-ctl` - Control utility
- `/usr/local/etc/preheat.conf` - Configuration file
- `/usr/local/lib/systemd/system/preheat.service` - Systemd unit
- `/usr/local/etc/logrotate.d/preheat.logrotate` - Log rotation

Creates directories:
- `/usr/local/var/lib/preheat/` - State storage
- `/var/log/` - Log files

---

## Systemd Integration

### Enable Service
```bash
sudo systemctl daemon-reload
sudo systemctl enable preheat
```

### Start Service
```bash
sudo systemctl start preheat
```

### Verify Service
```bash
sudo systemctl status preheat
sudo preheat-ctl status
```

### View Logs
```bash
# Real-time logs
sudo journalctl -u preheat -f

# Last 50 lines
sudo journalctl -u preheat -n 50
```

---

## Configuration

Default config at `/etc/preheat.conf` (or `/usr/local/etc/preheat.conf`)

**Minimal config for testing**:
```ini
[model]
cycle = 20
minsize = 2000000

[system]
doscan = true
dopredict = true
```

See `CONFIGURATION.md` for full parameter reference.

---

## Verification

### Test Daemon Startup
```bash
# Foreground mode (Ctrl+C to stop)
sudo /usr/local/sbin/preheat -f
```

Expected: No errors, daemon runs

### Test CLI Tool
```bash
sudo preheat-ctl status
sudo preheat-ctl dump
```

Expected: "preheat is running"

### Check State File
```bash
sudo ls -lh /usr/local/var/lib/preheat/
```

Expected: `preheat.state` file created after first save

---

## Uninstallation

### Stop Service
```bash
sudo systemctl stop preheat
sudo systemctl disable preheat
```

### Remove Files
```bash
cd /path/to/preheat
sudo make uninstall
```

### Remove State (Optional)
```bash
sudo rm -rf /usr/local/var/lib/preheat/
sudo rm -f /usr/local/var/log/preheat.log
```

---

## Troubleshooting

### Daemon Won't Start

**Check prerequisites**:
```bash
# Verify readahead() syscall
grep readahead /proc/kallsyms
```

**Check permissions**:
```bash
# Must run as root
sudo preheat-ctl status
```

**Check logs**:
```bash
sudo journalctl -u preheat --no-pager
sudo tail -100 /usr/local/var/log/preheat.log
```

### Systemd Hardening Issues

**KNOWN ISSUE**: Security hardening in systemd service may interfere with /proc access.

**If daemon fails with permission errors**:
```bash
# Edit service file
sudo nano /usr/local/lib/systemd/system/preheat.service

# Comment out security options:
#PrivateTmp=yes
#NoNewPrivileges=yes
#ProtectSystem=strict
#ProtectHome=yes
#ReadWritePaths=/var/lib/preheat /var/log /var/run

# Reload and restart
sudo systemctl daemon-reload
sudo systemctl restart preheat
```

### State Migration

**One-way migration**: Preheat can import preload 0.6.4 state files, but preload **cannot** read Preheat states.

> [!WARNING]
> **Critical: State Migration is ONE-WAY**
> 
> Preheat can read preload 0.6.4 state files, but the original preload daemon
> will NOT be able to read preheat state files. Once you migrate to preheat,
> you cannot go back to preload without losing your learned data.
> 
> **Always back up your preload state before migrating!**

If migrating from preload:
```bash
# Backup preload state
sudo cp /var/lib/preload/preload.state /var/lib/preload/preload.state.bak

# Copy to preheat (will auto-migrate on first load)
sudo cp /var/lib/preload/preload.state /usr/local/var/lib/preheat/preheat.state

# Start preheat
sudo systemctl start preheat
```

**Warning**: Original preload daemon will not be able to use migrated state.

---

## Next Steps

- Configure parameters: see `CONFIGURATION.md`
- Control daemon: see `man preheat-ctl(1)`
- Understand behavior: see `man preheat(8)`
