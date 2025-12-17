# Configuration Guide

Configuration file: `/usr/local/etc/preheat.conf` (default installation)  
*System packages may use `/etc/preheat.conf` instead*

Format: INI-style with sections and key-value pairs

---

## Configuration Sections

### [model] - Prediction Model Parameters

Controls how preheat learns and predicts application usage.

**Inherited from preload 0.6.4** - behavioral parity maintained.

#### cycle
- **Type**: Integer (seconds)
- **Default**: 20
- **Range**: 5-300
- **Description**: Time between prediction cycles
- **Effect**: Lower = more responsive, higher CPU usage

```ini
cycle = 20  # Check every 20 seconds
```

#### minsize
- **Type**: Integer (bytes)
- **Default**: 2000000 (2MB)
- **Description**: Minimum total size for an application to be tracked
- **Effect**: Filters out tiny applications

```ini
minsize = 2000000  # Ignore apps < 2MB
```

#### memtotal
- **Type**: Integer (percentage, can be negative)
- **Default**: -10
- **Description**: Percentage of total RAM to use for preloading
- **Effect**: Negative values reduce available memory

```ini
memtotal = -10  # Leave 10% RAM untouched
```

#### memfree
- **Type**: Integer (percentage)
- **Default**: 50
- **Description**: Percentage of free RAM to use for preloading
- **Effect**: Higher = more aggressive preloading

```ini
memfree = 50  # Use up to 50% of free RAM
```

#### memcached
- **Type**: Integer (percentage)
- **Default**: 0
- **Description**: Percentage of cached RAM to use for preloading
- **Effect**: 0 = don't touch cache

```ini
memcached = 0  # Don't use cached memory
```

**Memory Formula** (from upstream preload):
```
Available = max(0, Total × memtotal/100 + Free × memfree/100) + Cached × memcached/100
```

---

### [system] - System Behavior

#### doscan
- **Type**: Boolean
- **Default**: true
- **Description**: Enable process scanning
- **Effect**: false = no monitoring (daemon idle)

```ini
doscan = true
```

#### dopredict
- **Type**: Boolean
- **Default**: true
- **Description**: Enable prediction and preloading
- **Effect**: false = monitor only, no preloading

```ini
dopredict = true
```

#### autosave
- **Type**: Integer (seconds)
- **Default**: 3600 (1 hour)
- **Description**: Interval between automatic state file saves
- **Effect**: Lower = less data loss on crash, more I/O

```ini
autosave = 3600  # Save every hour
```

#### maxprocs
- **Type**: Integer
- **Default**: 30
- **Description**: Maximum parallel readahead processes
- **Effect**: Higher = more disk I/O, faster preloading

```ini
maxprocs = 30
```

#### sortstrategy
- **Type**: Integer (0-3)
- **Default**: 3 (BLOCK)
- **Values**:
  - 0 = NONE (no sorting)
  - 1 = PATH (alphabetical)
  - 2 = INODE (inode number)
  - 3 = BLOCK (disk block order - fastest)
- **Description**: File sorting strategy for readahead

```ini
sortstrategy = 3  # BLOCK - minimize disk seeks
```

#### manualapps
- **Type**: String (file path)
- **Default**: (empty, disabled)
- **Description**: Path to a file containing apps to always preload
- **Effect**: Listed apps receive highest priority for preloading

```ini
manualapps = /etc/preheat.d/apps.list
```

**Whitelist File Format:**
```
# One app per line, lines starting with # are comments
/usr/bin/firefox
/usr/bin/code
/usr/bin/burpsuite
```

**Setup Steps:**
1. Create directory: `sudo mkdir -p /etc/preheat.d`
2. Create file: `sudo nano /etc/preheat.d/apps.list`
3. Add apps (one per line, absolute paths)
4. Add to config: `manualapps = /etc/preheat.d/apps.list`
5. Reload: `sudo preheat-ctl reload`

#### usecorrelation
- **Type**: Boolean
- **Default**: true
- **Description**: Use correlation-based Markov prediction
- **Effect**: false = simpler frequency-based prediction

```ini
usecorrelation = true
```

---

### [ignore] - Application Filtering

#### exeprefix
- **Type**: String (semicolon-separated patterns)
- **Default**: `!/usr/sbin/;!/usr/local/sbin/;/usr/;!/`
- **Description**: Executable path filters
- **Format**: `!` prefix = exclude, otherwise include
- **Evaluation**: Left-to-right, first match wins

```ini
# Default: Exclude /usr/sbin/, /usr/local/sbin/, and /
# Include everything under /usr/
exeprefix = !/usr/sbin/;!/usr/local/sbin/;/usr/;!/
```

#### mapprefix
- **Type**: String (semicolon-separated patterns)
- **Default**: `/usr/;/lib;/var/cache/;!/`
- **Description**: Memory map path filters
- **Effect**: Limits which shared libraries are preloaded

```ini
# Default: Include /usr/, /lib, /var/cache/, exclude root
mapprefix = /usr/;/lib;/var/cache/;!/
```

---

## Example Configurations

### Conservative (Low Resource)
```ini
[model]
cycle = 30
minsize = 5000000
memtotal = -20
memfree = 30
memcached = 0

[system]
doscan = true
dopredict = true
autosave = 7200
maxprocs = 10
sortstrategy = 3
```

### Aggressive (High Performance)
```ini
[model]
cycle = 10
minsize = 1000000
memtotal = 0
memfree = 75
memcached = 20

[system]
doscan = true
dopredict = true
autosave = 1800
maxprocs = 50
sortstrategy = 3
```

### Monitoring Only (No Preloading)
```ini
[model]
cycle = 20

[system]
doscan = true
dopredict = false
autosave = 3600
```

---

## Reloading Configuration

### Method 1: Signal
```bash
sudo kill -HUP $(cat /run/preheat.pid)
```

### Method 2: CLI Tool
```bash
sudo preheat-ctl reload
```

### Method 3: Systemd
```bash
sudo systemctl reload preheat
```

**Note**: Changes take effect on next prediction cycle.

---

## Tuning Recommendations

### For Low-Memory Systems (< 2GB RAM)
- Increase `minsize` to 5000000
- Decrease `memfree` to 30
- Set `memtotal` to -20
- Decrease `maxprocs` to 10

### For SSDs
- `sortstrategy = 0` (NONE) - random access is fast
- Increase `maxprocs` to 50

### For HDDs
- `sortstrategy = 3` (BLOCK) - minimize seeks
- Keep `maxprocs` at 30

### For Servers
- `doscan = true`, `dopredict = false` - monitor only
- `autosave = 600` - save frequently

---

## Behavioral Parity

**DEFAULT CONFIGURATION** produces behavior identical to upstream preload 0.6.4.

Do **NOT** modify defaults unless you understand the impact.

---

## Advanced: Preheat Extensions

**NOT YET IMPLEMENTED** - Reserved for future versions.

When available, will require:
1. Build with `--enable-preheat-extensions`
2. Explicit config flags (all default to `false`)

Planned extensions:
- Time-of-day learning patterns
- Tool priority boosting
- Enhanced multi-factor scoring

---

## Files

*Paths shown are defaults for local installation (`/usr/local` prefix). System packages may use `/etc` and `/var` instead.*

### Configuration
- `/usr/local/etc/preheat.conf` - Main config
- `/etc/preheat.d/apps.list` - Manual app list (optional)

### Data
- `/usr/local/var/lib/preheat/preheat.state` - Learned state
- `/usr/local/var/log/preheat.log` - Daemon log

### Runtime
- `/run/preheat.pid` - Process ID file

---

## See Also

- `man preheat(8)` - Daemon manual
- `man preheat.conf(5)` - This configuration reference
- `INSTALL.md` - Installation instructions
