# Snap Firefox Detection Issue - Complete Context

**Project:** Preheat Linux (adaptive readahead daemon)  
**Issue:** Snap-installed Firefox not being tracked or prioritized correctly  
**Date Resolved:** December 31, 2025

---

## Problem Statement

The preheat daemon was failing to properly track Firefox when installed via snap package on Ubuntu systems. Users reported Firefox not appearing in `preheat-ctl stats` output despite daily usage.

---

## Root Cause Discovery

### Initial Hypothesis (Kali Linux)
AppArmor was blocking access to `/proc/PID/maps` for snap processes, causing `kp_proc_get_maps()` to return 0 and triggering silent exit in `new_exe_callback()`.

### Actual Root Cause (Ubuntu 24.04 - verified via VM testing)
Testing showed AppArmor was NOT blocking `/proc` access on Ubuntu. The real issues were in `desktop.c`:

1. **Missing scan directory:** Snap `.desktop` files are in `/var/lib/snapd/desktop/applications/` not standard XDG locations
2. **Exec= prefix issue:** Snap desktop files use `Exec=env BAMF_DESKTOP_FILE_HINT=... /snap/bin/firefox %u`
3. **Symlink resolution order:** `/snap/bin/firefox` is a symlink to `/usr/bin/snap` (the runner), and `realpath()` was converting it before snap detection

---

## Fixes Implemented (in `src/utils/desktop.c`)

### 1. Added Snap Desktop Directory
```c
// In kp_desktop_init()
scan_desktop_dir("/var/lib/snapd/desktop/applications");
```

### 2. Handle `env VAR=value` Prefix
```c
// In resolve_exec_path()
if (strcmp(binary, "env") == 0) {
    int i = 1;
    while (argv[i]) {
        if (strchr(argv[i], '=') != NULL && argv[i][0] != '/') {
            i++;
            continue;
        }
        binary = argv[i];
        break;
    }
}
```

### 3. Check Snap Wrapper BEFORE realpath()
```c
// BEFORE canonicalization, check for snap wrapper
if (resolved && g_str_has_prefix(resolved, "/snap/bin/")) {
    char *snap_resolved = resolve_snap_binary(resolved);
    if (snap_resolved) {
        g_free(resolved);
        resolved = snap_resolved;
        return resolved;  // Already canonicalized
    }
}
// AFTER: normal realpath() for non-snap
```

### 4. Snap Binary Resolution Function
```c
static char *resolve_snap_binary(const char *wrapper_path)
{
    // Extract snap name from /snap/bin/<name>
    // Try patterns:
    //   /snap/<name>/current/usr/lib/<name>/<name>  (Firefox)
    //   /snap/<name>/current/usr/bin/<name>
    //   /snap/<name>/current/bin/<name>
    // Return realpath() of first executable found
}
```

---

## Test Results

**Before fix:**
```
Desktop scanner initialized: discovered 38 GUI applications
Reclassified /snap/firefox/6565/usr/lib/firefox/firefox: priority → observation (reason: default (no match))
```

**After fix:**
```
Desktop scanner initialized: discovered 43 GUI applications
Reclassified /snap/firefox/6565/usr/lib/firefox/firefox: observation → priority (reason: .desktop (Firefox))

Pool Classification: ✓ PASS (priority pool)
```

---

## Key Technical Insights

| Issue | Kali Linux | Ubuntu 24.04 |
|-------|------------|--------------|
| `/proc/PID/maps` access | Blocked by AppArmor | Allowed for root |
| Desktop file location | Standard XDG | `/var/lib/snapd/desktop/applications/` |
| Exec= format | Standard | `env BAMF_DESKTOP_FILE_HINT=...` prefix |
| `/snap/bin/X` | Symlink to `/usr/bin/snap` | Same |

---

## Remaining Issue

Launch counting doesn't increment for snap Firefox because persistent helper processes (e.g., PIDs 24800, 24801) make the daemon think Firefox is "still running" rather than newly launched. This is a separate issue in `spy.c` PID tracking logic.

---

## Commits Made

1. `feat: resolve snap wrapper scripts to actual binaries in desktop.c`
2. `fix: scan /var/lib/snapd/desktop/applications for snap .desktop files`
3. `fix: skip 'env VAR=value' prefixes in Exec= lines`
4. `fix: check snap wrapper BEFORE realpath() to prevent resolution to /usr/bin/snap`

---

## Files Modified

- `src/utils/desktop.c` - Main fixes for snap path resolution
- `docs/SNAP_FIREFOX_DEBUG_NOTES.md` - Debug timeline and resolution
- `docs/SNAP_FIREFOX_ROOT_CAUSE_ANALYSIS.md` - Root cause documentation
- `docs/UBUNTU_VM_TEST_FINDINGS.md` - Ubuntu-specific test results
- `scripts/snap_firefox_debug.sh` - Test script for verification

---

## How to Reproduce Testing

```bash
# On Ubuntu with snap Firefox
git clone https://github.com/wasteddreams/preheat-linux.git
cd preheat-linux
./configure && make && sudo make install
sudo systemctl restart preheat

# Wait for scan cycle (90 seconds)
sleep 100

# Verify Firefox is in priority pool
sudo preheat-ctl explain /snap/firefox/*/usr/lib/firefox/firefox
# Expected: Pool: priority
```
