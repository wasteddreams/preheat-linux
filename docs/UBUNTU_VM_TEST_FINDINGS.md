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

1. [x] Fix desktop.c snap path matching ✅ DONE
2. [ ] Add /etc/preheat.d to install targets
3. [x] Update SNAP_FIREFOX_DEBUG_NOTES.md with Ubuntu-specific findings ✅ DONE

---

## Resolution: December 31, 2025

### 🎉 Snap Firefox Now in Priority Pool

All desktop.c fixes have been implemented and tested:

1. **Scan snap desktop directory:** `/var/lib/snapd/desktop/applications/`
2. **Handle `env VAR=value` prefix:** Skip environment variables in Exec= lines
3. **Snap wrapper resolution:** Check `/snap/bin/X` BEFORE `realpath()` to avoid resolving to `/usr/bin/snap`
4. **Pattern-based binary finding:** Try `/snap/<name>/current/usr/lib/<name>/<name>` etc.

### Test Results

```
Desktop scanner initialized: discovered 43 GUI applications
Reclassified /snap/firefox/6565/usr/lib/firefox/firefox: observation → priority (reason: .desktop (Firefox))

preheat-ctl explain /snap/firefox/6565/usr/lib/firefox/firefox:
  Pool:    priority
  Status:  ✅ In priority pool
```

### Long Test Output

```
=============================================
              TEST SUMMARY
=============================================

Pool Classification: ✓ PASS (priority pool)
Launch Tracking: ✓ PASS (1 launches recorded)

=============================================
          ALL TESTS PASSED!
=============================================
```

### Outstanding Issue: Launch Counting

Firefox raw launches don't increment despite 5 launches in testing. This appears to be caused by persistent snap helper processes (PIDs remain across Firefox restarts) that make the daemon think Firefox is "still running" rather than newly launched.

This is a **separate issue** from desktop path matching and requires investigation in `spy.c` and the PID tracking logic.

### Key Learnings

| Issue | Kali Linux | Ubuntu 24.04 |
|-------|------------|--------------|
| `/proc/PID/maps` access | Blocked by AppArmor | Allowed for root |
| Desktop file matching | N/A (maps blocked) | Was the root cause |
| `.desktop` location | Standard XDG | `/var/lib/snapd/desktop/applications/` |
| Exec= format | Standard | Uses `env BAMF_DESKTOP_FILE_HINT=...` prefix |
| `/snap/bin/X` type | Symlink to `/usr/bin/snap` | Same |

