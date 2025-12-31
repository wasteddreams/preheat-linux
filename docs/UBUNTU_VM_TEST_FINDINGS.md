# Ubuntu VM Test Findings

**Date:** 2025-12-31  
**System:** Ubuntu 24.04 with Firefox snap

---

## Summary

Tested preheat daemon on Ubuntu VM. Firefox snap IS detected and tracked, but placed in observation pool instead of priority pool.

---

## Issues Found

### 1. Snap Firefox in Observation Pool (Medium Priority)

**Symptom:** Firefox snap tracked but not in priority pool / stats output

**Root Cause:** Desktop file scanner doesn't match `/snap/firefox/6565/usr/lib/firefox/firefox` to `firefox.desktop` because:
- `.desktop` file uses `Exec=firefox` (symlink)
- Daemon sees full snap path, not symlink
- No resolution of `/snap/bin/firefox` → actual binary

**Log Evidence:**
```
Reclassified /snap/firefox/6565/usr/lib/firefox/firefox: priority → observation (reason: default (no match))
```

**Fix Options:**
1. In `desktop.c`: Resolve symlinks when scanning `.desktop` Exec= paths
2. Add `/snap/bin/` to symlink resolution
3. Check if app path starts with `/snap/` and try to find matching `.desktop` via app name

---

### 2. Missing /etc/preheat.d Directory (Low Priority)

**Symptom:** Manual apps directory doesn't exist on fresh install

**Root Cause:** `make install` doesn't create `/etc/preheat.d/`

**Fix:** Add to Makefile.am install targets:
```makefile
install-data-local:
	$(MKDIR_P) $(DESTDIR)$(sysconfdir)/preheat.d
```

---

### 3. AppArmor NOT Blocking on Ubuntu (Informational)

**Previous hypothesis was WRONG for this system:**
- Root CAN read `/proc/PID/maps` for snap Firefox (72 lines readable)
- Root CAN read `/proc/PID/exe` symlink
- AppArmor profiles exist but don't block daemon access

This contradicts the Kali findings where AppArmor blocked `/proc` access. The difference may be:
- Different snap confinement levels
- Different AppArmor profiles between distros
- Ubuntu may use less strict profiles

---

## Test Script Created

`scripts/snap_firefox_debug.sh` - Comprehensive debug script for testing snap Firefox detection:
- Phase 1: Installation check
- Phase 2: Process detection  
- Phase 3: /proc access tests
- Phase 4: AppArmor status
- Phase 5: Daemon status
- Phase 6: Config check
- Phase 7: Manual commands

---

## Configuration on Test System

```ini
exeprefix_raw = /usr/;/bin/;/opt/;/snap/   # /snap/ included ✓
mapprefix_raw = /usr/;/lib;/var/cache/;!/
cycle = 90
```

---

## Next Steps

1. [ ] Fix desktop.c snap path matching
2. [ ] Add /etc/preheat.d to install targets
3. [ ] Update SNAP_FIREFOX_DEBUG_NOTES.md with Ubuntu-specific findings
