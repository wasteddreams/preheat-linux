# State File Format Specification

**Version:** 1.0.2  
**File:** `/usr/local/var/lib/preheat/preheat.state`  
**Format:** Text-based, line-oriented  
**Encoding:** UTF-8

---

## Overview

The state file persists preheat's learned data between daemon restarts:
- Application registry (paths, launch counts, timestamps)
- Memory maps for each application
- Markov chain transition probabilities
- Application families/groups
- Preload timestamps for hit/miss tracking

**Design Goals:**
- Human-readable for debugging
- Fast to load (<100ms)
- Version-tolerant (forward/backward compatible)
- CRC32 protected against corruption

---

## File Structure

```
PRELOAD <version> <timestamp>
MAP <seq> <path> <offset> <length>
...
BADEXE <timestamp> <size> <path>
...
EXE <seq> <timestamp> ... <path>
  PIDS <count>
  PID <pid> <start> <update> <user_init>
EXEMAP <exe_seq> <map_seq> <prob>
...
MARKOV <exe_a> <exe_b> <time> <prob_matrix...>
...
FAMILY <id> <member_count>
  <member_path>
  ...
PRELOAD_TIMES <count>
PTIME <app_name> <timestamp>
...
CRC32 <checksum>
```

---

## Line Tags

| Tag | Format | Description |
|-----|--------|-------------|
| `PRELOAD` | `PRELOAD <version> <time>` | Header (first line only) |
| `MAP` | `MAP <seq> <path> <offset> <length>` | Memory map region |
| `BADEXE` | `BADEXE <time> <size> <path>` | Blacklisted executable |
| `EXE` | `EXE <seq> <time> ... <path>` | Tracked executable |
| `PIDS` | `  PIDS <count>` | Running process subsection |
| `PID` | `  PID <pid> <start> <update> <user>` | Individual process entry |
| `EXEMAP` | `EXEMAP <exe_seq> <map_seq> <prob>` | Exe-to-map association |
| `MARKOV` | `MARKOV <a> <b> <time> <matrix...>` | Correlation chain |
| `FAMILY` | `FAMILY <id> <count>` | Application family |
| `PRELOAD_TIMES` | `PRELOAD_TIMES <count>` | Preload timestamps section |
| `PTIME` | `PTIME <app_name> <timestamp>` | Individual preload time (v1.0.2+) |
| `CRC32` | `CRC32 <hex_checksum>` | Integrity footer |

---

## Example State File

```
PRELOAD 20260106 1736180413
MAP	1	/usr/lib/firefox-esr/libxul.so	0	95000000
MAP	2	/lib/x86_64-linux-gnu/libc.so.6	0	1800000
BADEXE	1736150000	500	/tmp/broken.bin
EXE	1	1736180000	100	50.5	45	3600	1	/usr/bin/firefox-esr
  PIDS	1
  PID	12345	1736180000	1736180400	1
EXEMAP	1	1	0.85
EXEMAP	1	2	0.92
MARKOV	1	2	1736180000	0	0	0	0	0	0	0	0	0	0	0	0	0	0	0	0	0	0	0	0	0
FAMILY	browsers	3
/usr/bin/firefox-esr
/usr/bin/chromium
/usr/bin/brave-browser
PRELOAD_TIMES	2
PTIME	firefox-esr	1736180500
PTIME	code	1736180400
CRC32	D88A19A9
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial text format |
| 1.0.1 | Added PIDS/PID subsections, FAMILY entries |
| 1.0.2 | Changed preload timestamp tag from `PRELOAD` to `PTIME` to avoid collision with header |

---

## Corruption Handling

### Detection

1. **CRC32 check:** Recompute checksum, compare with footer
2. **Syntax validation:** Each line must have valid tag and field count
3. **Index validation:** References must point to existing entities

### Recovery (v1.0.2+)

When corruption is detected:
1. State file renamed to `preheat.state.broken.<timestamp>`
2. Daemon starts with fresh state
3. Broken files older than 48 hours are automatically cleaned up on next startup

---

## Security

- **Permissions:** File is 600 (owner-only), owned by root
- **CRC32:** Detects accidental corruption (not cryptographically secure)
- **Privacy:** Contains user behavior history; encrypted disk recommended for sensitive environments

---

## Tools

```bash
# View current state (human-readable)
sudo preheat-ctl dump

# Check state file health
sudo preheat-ctl health

# Reset to fresh state
sudo systemctl stop preheat
sudo rm /usr/local/var/lib/preheat/preheat.state
sudo systemctl start preheat
```
