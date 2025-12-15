# Configuration Reference

Complete reference for preheat configuration options.

---

## Configuration File Location

```
/usr/local/etc/preheat.conf    # Default location (source install)
/etc/preheat.conf              # Alternative (package install)
```

Use `preheat -c /path/to/config` to specify an alternate location.

---

## Configuration Format

Preheat uses INI-style configuration:

```ini
# Comments start with #
[section]
key = value
key2 = value2

[another_section]
key = value
```

**Rules:**
- Keys are case-sensitive
- Values have no quotes (even strings)
- Boolean values: `true`, `false`
- Path lists: separated by semicolons (`;`)

---

## Section: [model]

Controls the prediction model and memory usage.

### cycle

**Description:** Interval between daemon cycles (scan, predict, preload).

| Property | Value |
|----------|-------|
| Type | Integer (seconds) |
| Default | `20` |
| Range | 5-300 |

**Recommendations:**
- **Lower values (5-15)**: More responsive, higher CPU usage
- **Higher values (30-60)**: Less responsive, lower overhead
- **Use even numbers** for predictable timing

```ini
cycle = 20
```

> **Warning:** Values below 10 may cause noticeable CPU usage on slower systems.

---

### usecorrelation

**Description:** Use statistical correlation in prediction algorithm.

| Property | Value |
|----------|-------|
| Type | Boolean |
| Default | `true` |

Correlation analysis improves prediction accuracy by measuring how strongly applications co-occur beyond random chance.

```ini
usecorrelation = true
```

---

### minsize

**Description:** Minimum total size of memory maps for tracking.

| Property | Value |
|----------|-------|
| Type | Integer (bytes) |
| Default | `2000000` (2 MB) |

Applications with smaller footprints are ignored (screen savers, tiny utilities).

```ini
minsize = 2000000
```

---

### memtotal

**Description:** Percentage of total RAM to include in preload budget.

| Property | Value |
|----------|-------|
| Type | Signed integer (percent) |
| Default | `-10` |
| Range | -100 to 100 |

Negative values subtract from the budget, providing a safety margin.

```ini
memtotal = -10
```

---

### memfree

**Description:** Percentage of free RAM to include in preload budget.

| Property | Value |
|----------|-------|
| Type | Integer (percent) |
| Default | `50` |
| Range | 0 to 100 |

```ini
memfree = 50
```

---

### memcached

**Description:** Percentage of cached RAM to include in preload budget.

| Property | Value |
|----------|-------|
| Type | Integer (percent) |
| Default | `0` |

Setting this above 0 risks evicting other useful cached data.

```ini
memcached = 0
```

---

### Memory Budget Formula

```
Available = max(0, Total × memtotal% + Free × memfree%) + Cached × memcached%
```

**Example with defaults on 8GB system, 3GB free, 2GB cached:**
```
Available = max(0, 8192 × -0.10 + 3072 × 0.50) + 2048 × 0
          = max(0, -819 + 1536) + 0
          = 717 MB
```

---

## Section: [system]

Controls daemon behavior and I/O operations.

### doscan

**Description:** Enable process monitoring.

| Property | Value |
|----------|-------|
| Type | Boolean |
| Default | `true` |

Set to `false` to stop learning (useful for debugging).

```ini
doscan = true
```

---

### dopredict

**Description:** Enable prediction and preloading.

| Property | Value |
|----------|-------|
| Type | Boolean |
| Default | `true` |

Set to `false` to monitor without preloading.

```ini
dopredict = true
```

---

### autosave

**Description:** Interval for automatic state saves.

| Property | Value |
|----------|-------|
| Type | Integer (seconds) |
| Default | `3600` (1 hour) |

More frequent saves provide better crash recovery but increase disk writes.

```ini
autosave = 3600
```

---

### mapprefix

**Description:** Path filters for shared libraries (memory maps).

| Property | Value |
|----------|-------|
| Type | Semicolon-separated paths |
| Default | `/usr/;/lib;/var/cache/;!/` |

**Syntax:**
- Paths without `!` prefix: Include
- Paths with `!` prefix: Exclude
- First match wins
- `!/` at end excludes everything else

```ini
mapprefix = /usr/;/lib;/var/cache/;!/
```

---

### exeprefix

**Description:** Path filters for executables.

| Property | Value |
|----------|-------|
| Type | Semicolon-separated paths |
| Default | `!/usr/sbin/;!/usr/local/sbin/;/usr/;!/` |

Default excludes system administration tools.

```ini
exeprefix = !/usr/sbin/;!/usr/local/sbin/;/usr/;!/
```

---

### processes

**Description:** Maximum parallel readahead workers.

| Property | Value |
|----------|-------|
| Type | Integer |
| Default | `30` |
| Range | 0-100 |

Higher values increase I/O parallelism. Set to 0 for single-process mode.

```ini
processes = 30
```

---

### sortstrategy

**Description:** File ordering strategy for preloading.

| Value | Name | Description | Best For |
|-------|------|-------------|----------|
| `0` | SORT_NONE | No sorting | SSD, NVMe |
| `1` | SORT_PATH | Sort by path | Network FS |
| `2` | SORT_INODE | Sort by inode | General use |
| `3` | SORT_BLOCK | Sort by disk block | HDD |

```ini
sortstrategy = 3
```

**Recommendation:**
- **HDD (spinning disk)**: `3` (default) - minimizes seek time
- **SSD/NVMe**: `0` - no benefit from sorting
- **Network filesystems**: `1` - groups by directory

---

### manualapps

**Description:** Path to file containing always-preload applications.

| Property | Value |
|----------|-------|
| Type | File path |
| Default | (empty/disabled) |

```ini
manualapps = /etc/preheat.d/apps.list
```

**File format:**
```
# /etc/preheat.d/apps.list
# One path per line
/usr/bin/firefox
/usr/bin/code
/usr/bin/libreoffice
```

---

## Section: [preheat]

Optional extensions (require `--enable-preheat-extensions` build flag).

### enable_preheat_scoring

**Description:** Enable enhanced scoring for priority applications.

| Property | Value |
|----------|-------|
| Type | Boolean |
| Default | `false` |

```ini
enable_preheat_scoring = false
```

---

### preheat_tool_boost

**Description:** Priority multiplier for boosted applications.

| Property | Value |
|----------|-------|
| Type | Float |
| Default | `1.0` |
| Range | 1.0-3.0 recommended |

```ini
preheat_tool_boost = 1.0
```

---

### enable_time_learning

**Description:** Enable time-of-day pattern learning.

| Property | Value |
|----------|-------|
| Type | Boolean |
| Default | `false` |

When enabled, learns that certain apps are used at certain times.

```ini
enable_time_learning = false
```

---

### manual_apps_list

**Description:** Path to manual applications list.

| Property | Value |
|----------|-------|
| Type | File path |
| Default | `/etc/preheat.d/apps.list` |

```ini
manual_apps_list = /etc/preheat.d/apps.list
```

---

### blacklist

**Description:** Path to blacklisted applications.

| Property | Value |
|----------|-------|
| Type | File path |
| Default | `/etc/preheat.d/blacklist` |

```ini
blacklist = /etc/preheat.d/blacklist
```

**File format:**
```
# /etc/preheat.d/blacklist
# Applications to never track or preload
/usr/bin/some-broken-app
/opt/problematic/binary
```

---

## Example Configurations

### Minimal (Safe Defaults)

```ini
# Minimal configuration - uses all defaults
# Just create an empty file or use defaults
[model]
cycle = 20

[system]
doscan = true
dopredict = true
```

---

### Balanced (Recommended)

```ini
# Good balance of responsiveness and resource usage
[model]
cycle = 20
usecorrelation = true
minsize = 2000000
memtotal = -10
memfree = 50
memcached = 0

[system]
doscan = true
dopredict = true
autosave = 3600
processes = 30
sortstrategy = 3
```

---

### Conservative (Low Resource Systems)

```ini
# For systems with limited RAM (2-4 GB)
[model]
cycle = 30
minsize = 5000000
memtotal = -20
memfree = 30
memcached = 0

[system]
autosave = 7200
processes = 10
sortstrategy = 3
```

---

### Aggressive (High Performance)

```ini
# For systems with plenty of RAM (8+ GB)
[model]
cycle = 15
minsize = 1000000
memtotal = 0
memfree = 70
memcached = 10

[system]
autosave = 1800
processes = 50
sortstrategy = 3
```

---

### SSD Optimized

```ini
# For SSD/NVMe storage
[model]
cycle = 20
memfree = 50

[system]
sortstrategy = 0   # No block sorting needed
processes = 50     # SSDs handle parallelism well
```

---

## Applying Configuration Changes

After editing the configuration file:

```bash
# Method 1: Reload (no restart needed)
sudo preheat-ctl reload

# Method 2: Via systemd
sudo systemctl reload preheat

# Method 3: Full restart
sudo systemctl restart preheat
```

Reload re-reads the configuration file without losing learned state.

---

## Validation

Check configuration for errors by running in foreground:

```bash
sudo preheat -f -c /path/to/config
```

The daemon will log configuration values on startup for verification.

---

## Navigation

| Previous | Up | Next |
|----------|----|----- |
| [← Architecture](architecture.md) | [Documentation Index](README.md) | [Advanced Usage →](advanced-usage.md) |
