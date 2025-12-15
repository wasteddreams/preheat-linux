# Installation Guide

This guide covers installing preheat on Debian-based Linux systems including Kali Linux, Debian, and Ubuntu.

---

## System Requirements

### Minimum Requirements

| Component | Requirement |
|-----------|-------------|
| **Operating System** | Debian-based Linux (Kali, Debian 11+, Ubuntu 20.04+) |
| **Kernel** | Linux 2.6+ with `readahead(2)` support |
| **RAM** | 2 GB minimum (4+ GB recommended) |
| **Permissions** | Root access for installation and daemon operation |

### Build Dependencies

For building from source, you need:

```bash
# Essential build tools
build-essential    # gcc, make, etc.
autoconf          # Autotools configuration
automake          # Makefile generation

# Required libraries
libglib2.0-dev    # GLib development files
pkg-config        # Library detection
```

---

## Installation Methods

### Method 1: Quick Install Script (Recommended)

The easiest way to install preheat:

```bash
# Clone the repository
git clone https://github.com/wasteddreams/preheat-linux.git
cd preheat-linux

# Run the installer (handles everything)
sudo ./scripts/install.sh
```

The install script will:
1. Check and install dependencies
2. Build preheat from source
3. Install binaries and configuration
4. Set up and start the systemd service

### Method 2: Manual Build and Install

For more control over the installation process:

#### Step 1: Install Dependencies

```bash
# Debian/Ubuntu/Kali
sudo apt update
sudo apt install -y build-essential libglib2.0-dev autoconf automake pkg-config
```

#### Step 2: Clone Repository

```bash
git clone https://github.com/wasteddreams/preheat-linux.git
cd preheat-linux
```

#### Step 3: Build

```bash
# Generate configure script
autoreconf -fi

# Configure (use defaults)
./configure

# Optional: Configure with custom prefix
# ./configure --prefix=/usr --sysconfdir=/etc

# Build
make -j$(nproc)
```

#### Step 4: Install

```bash
# Install files
sudo make install

# Enable and start service
sudo systemctl daemon-reload
sudo systemctl enable preheat
sudo systemctl start preheat
```

---

## Verify Installation

After installation, verify everything is working:

### Check Service Status

```bash
sudo systemctl status preheat
```

Expected output:
```
● preheat.service - Adaptive readahead daemon
     Loaded: loaded (/usr/lib/systemd/system/preheat.service; enabled)
     Active: active (running) since Mon 2024-12-15 10:00:00 UTC
   Main PID: 1234 (preheat)
     Memory: 5.2M
        CPU: 124ms
```

### Check Binary

```bash
preheat --version
```

Expected output:
```
preheat 0.1.0
Adaptive readahead daemon for Debian-based distributions
```

### Check Logs

```bash
sudo tail -20 /var/log/preheat.log
```

You should see startup messages and periodic scan activity.

---

## Installation Paths

After installation, preheat files are located at:

| File Type | Path |
|-----------|------|
| **Daemon binary** | `/usr/local/bin/preheat` |
| **CLI tool** | `/usr/local/bin/preheat-ctl` |
| **Configuration** | `/usr/local/etc/preheat.conf` |
| **State file** | `/usr/local/var/lib/preheat/preheat.state` |
| **Log file** | `/usr/local/var/log/preheat.log` |
| **PID file** | `/run/preheat.pid` |
| **systemd unit** | `/usr/lib/systemd/system/preheat.service` |

> **Note**: Paths may vary if you used a custom `--prefix` during configuration.

---

## Configuration After Install

The default configuration works well for most systems. However, you may want to:

1. **Review configuration**: `sudo nano /usr/local/etc/preheat.conf`
2. **Check if changes are needed** based on your hardware

See [Configuration Reference](configuration.md) for all options.

---

## Uninstallation

To completely remove preheat:

### Using Make

```bash
cd preheat-linux
sudo make uninstall
```

### Manual Removal

```bash
# Stop and disable service
sudo systemctl stop preheat
sudo systemctl disable preheat

# Remove files
sudo rm -f /usr/local/bin/preheat
sudo rm -f /usr/local/bin/preheat-ctl
sudo rm -f /usr/local/etc/preheat.conf
sudo rm -rf /usr/local/var/lib/preheat
sudo rm -f /usr/lib/systemd/system/preheat.service

# Reload systemd
sudo systemctl daemon-reload
```

---

## Common Installation Issues

### Issue: Missing Dependencies

**Symptom:**
```
configure: error: Package requirements (glib-2.0 >= 2.16) were not met
```

**Solution:**
```bash
sudo apt install libglib2.0-dev pkg-config
```

### Issue: Permission Denied

**Symptom:**
```
make install: cannot create regular file '/usr/local/bin/preheat': Permission denied
```

**Solution:**
```bash
sudo make install
```

### Issue: Service Masked

**Symptom:**
```
Failed to start preheat.service: Unit preheat.service is masked.
```

**Solution:**
```bash
sudo systemctl unmask preheat
sudo systemctl enable preheat
sudo systemctl start preheat
```

### Issue: Old State File Incompatibility

**Symptom:**
After upgrading, daemon fails to start or behaves incorrectly.

**Solution:**
```bash
# Remove old state file (daemon will rebuild it)
sudo rm /usr/local/var/lib/preheat/preheat.state
sudo systemctl restart preheat
```

---

## Upgrading

To upgrade to a newer version:

```bash
# Stop current service
sudo systemctl stop preheat

# Pull latest changes
cd preheat-linux
git pull origin main

# Rebuild
autoreconf -fi
./configure
make clean
make -j$(nproc)

# Reinstall
sudo make install

# Restart
sudo systemctl start preheat
```

Your learned state file is preserved during upgrades.

---

## Next Steps

After successful installation:

1. **[Quick Start](quick-start.md)** - Verify operation and understand basics
2. **[Configuration](configuration.md)** - Tune for your system
3. **[How It Works](how-it-works.md)** - Understand the daemon's behavior

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Introduction](introduction.md) | [Documentation Index](README.md) | [Quick Start →](quick-start.md) |
