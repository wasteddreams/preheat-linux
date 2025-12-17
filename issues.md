# Preheat Codebase Issues

Comprehensive audit results identifying bugs, inconsistencies, unimplemented features, and improvement opportunities.

---

## Critical Issues

### C1: Integer Overflow in Comparison Functions  
**File:** `src/readahead/readahead.c:75, 76, 96, 97`  
**Severity:** High  
**Description:** Integer arithmetic on `offset` and `length` (both `size_t`) could overflow when used in comparisons.

```c
// Line 75-76
i = a->offset - b->offset;  // Potential overflow if values are large
i = b->length - a->length;  // Potential overflow
```

**Fix:** Use proper comparison logic instead of subtraction:
```c
if (a->offset < b->offset) return -1;
if (a->offset > b->offset) return 1;
```

### C2: fsync() Called After close()
**File:** `src/readahead/readahead.c:60`  
**Severity:** Medium  
**Description:** File descriptor is closed immediately in `set_block()`, but no fsync is performed in state save.

**Context:** In `src/state/state.c:1207`, fsync() is correctly used before rename, but readahead file descriptors are closed without sync.

**Recommended:** Not critical for readahead (read-only), but document intentional behavior.

### C3: Missing fsync() on GIOChannel File Descriptors  
**File:** `src/state/state.c:1197-1198`  
**Severity:** Low  
**Description:** `g_io_channel_flush()` does not guarantee kernel flush. Need to obtain underlying fd and call fsync() directly.

**Current:** 
```c
g_io_channel_flush(f, NULL);
g_io_channel_unref(f);
```

**Note:** Actually fixed at line 1207 - fsync(fd) is called. Issue closed.

---

## Code Quality Issues

### Q1: Unimplemented TODO - Log Level Checks
**File:** `src/predict/prophet.c:214, 281`  
**Severity:** Low  
**Description:** Debug logging is disabled with `if (0)` instead of proper log level check.

```c
if (0) { /* TODO: implement log level check */
    g_debug("ln(prob(~MAP)) = %13.10lf %s", map->ln prob, map->path);
}
```

**Fix:** Implement `kp_is_debugging()` macro from `src/utils/logging.h:45` (which has its own bug - see Q2).

### Q2: Broken Debug Macro
**File:** `src/utils/logging.h:45`  
**Severity:** Medium  
**Description:** Debug level check macro has incorrect logic.

```c
#define kp_is_debugging() (G_LOG_LEVEL_DEBUG <= G_LOG_LEVEL_ERROR << kp_log_level)
```

**Issue:** Bit shift operator `<<` should be comparison. This will never work correctly.

**Fix:**
```c
#define kp_is_debugging() (kp_log_level >= G_LOG_LEVEL_DEBUG)
```

### Q3: FIXME Comments in Prophet Module
**File:** `src/predict/prophet.c:78, 160`  
**Severity:** Low  
**Description:** Algorithm improvement notes left from upstream preload.

```c
/* FIXME: what should we do we correlation w.r.t. state? */
/* FIXME: use exemap->prob, needs some theory work. */
```

**Action:** Document these are intentional limitations from upstream algorithm, not bugs.

### Q4: Memory Leak in Error Path
**File:** `src/monitor/proc.c:206-208`  
**Severity:** Low  
**Description:** If `readlink()` returns error or buffer overflow, loop continues without proper cleanup. Not critical as no memory is allocated, but poor error handling style.

**Fix:** Add explicit error logging for debuggability.

### Q5: Missing NULL Check After malloc
**File:** `src/state/state.c:1098`  
**Severity:** Medium  
**Description:** `g_malloc()` can theoretically fail (though glib aborts by default).

```c
content = g_malloc(file_size);
if (!content) {
    return;  // Silent failure
}
```

**Fix:** Use `g_malloc0()` or add logging before return.

---

## Documentation Issues

### D1: Incorrect Configuration File Path
**File:** `CONFIGURATION.md:3`  
**Severity:** Medium  
**Description:** Documentation says `/etc/preheat.conf` but actual install path is `/usr/local/etc/preheat.conf`.

**Current:**
```markdown
Configuration file: `/etc/preheat.conf` (or `/usr/local/etc/preheat.conf`)
```

**Should be:**
```markdown
Configuration file: `/usr/local/etc/preheat.conf` (default) or `/etc/preheat.conf` (system package)
```

**Note:** Partially corrected in line 313-314, but line 3 should match.

### D2: Incorrect Reload Examples
**File:** `CONFIGURATION.md:249`  
**Severity:** Low  
**Description:** Uses `/var/run/preheat.pid` instead of `/run/preheat.pid`.

```bash
sudo kill -HUP $(cat /var/run/preheat.pid)
```

** Fix:**
```bash
sudo kill -HUP $(cat /run/preheat.pid)
```

### D3: Misleading Memory Documentation
**File:** multiple files
**Severity:** Low  
**Description:** Some references incorrectly suggest daemon uses 100-400MB. Clarification exists in README.md line 187 but should be consistent everywhere.

---

## Build & Installation Issues

### B1: Missing Test Implementation
**File:** `tests/` directory  
**Severity:** Medium  
**Description:** Test directories exist (`integration/`, `performance/`, `unit/`) but appear to be empty placeholders.

**Action:** Either implement tests or remove empty directories to avoid confusion.

### B2: Hardcoded Path in Autom4te Cache
**File:** Build artifacts  
**Severity:** Low  
**Description:** `autom4te.cache`, `configure~`, `config.h.in~` should be in `.gitignore`.

**Fix:** Add to `.gitignore`:
```
autom4te.cache/
configure~
config.h.in~
*.in~
```

### B3: Silent install-data-hook Failures
**File:** `Makefile.am:65-72`  
**Severity:** Low  
**Description:** systemd commands use `|| true` which hides all errors.

```bash
systemctl enable preheat.service || true
```

**Recommended:** Log failures:
```bash
systemctl enable preheat.service || echo "Warning: systemctl enable failed"
```

### B4: No Uninstall Hook for State Files
**File:** `Makefile.am`  
**Severity:** Low  
**Description:** `make uninstall` doesn't remove `/usr/local/var/lib/preheat/` state files.

**Recommended:** Add `uninstall-local` target to offer removal (with confirmation).

---

## Configuration Issues

### CF1: Missing Validation
**File:** `src/config/config.c`  
**Severity:** Medium  
**Description:** No range validation for integer config values. User could set `cycle = -1` or `memfree = 1000000`.

**Fix:** Add validation in `config_load()`:
```c
if (conf->model.cycle < 5 || conf->model.cycle > 300) {
    g_warning("Invalid cycle value %d, using default 20", conf->model.cycle);
    conf->model.cycle = 20;
}
```

### CF2: Undocumented Blacklist Feature
**File:** `src/config/config.h`, `confkeys.h`  
**Severity:** Low  
**Description:** Code references blacklist feature (#ifdef ENABLE_PREHEAT_EXTENSIONS) but it's never documented or implemented.

**Action:** Either implement or remove dead code.

---

## Security/Safety Issues

### S1: No State File Integrity on Load
**File:** `src/state/state.c`  
**Severity:** Medium  
**Description:** State file is saved with CRC32 footer (line 1084-1127) but CRC is never verified on load.

**Current Behavior:** Corrupted state file is detected by parse errors, not checksum.

**Recommended:** Add CRC32 validation in `read_state()` before parsing.

### S2: TOCTOU in State File Rename
**File:** `src/state/state.c:1213`  
**Severity:** Low  
**Description:** Time-of-check to time-of-use race between fsync and rename.

**Note:** Using O_NOFOLLOW (line 1188) mitigates symlink attacks. Risk is minimal.

### S3: PID File Race Condition
**File:** `src/daemon/daemon.c:75-92`  
**Severity:** Low  
**Description:** PID file is created without locking. Two instances could theoretically start simultaneously.

**Mitigation:** Systemd prevents this in practice, but daemon should check PID file exists and validate process.

---

## Feature Issues

### F1: Blacklist Not Implemented
**File:** Multiple files with `#ifdef ENABLE_PREHEAT_EXTENSIONS`  
**Severity:** Low  
**Description:** Blacklist feature is coded but cannot be used (build flag doesn't exist).

**Files Affected:**
- `src/config/config.c:189`
- `src/config/confkeys.h`

**Action:** Either implement `--enable-preheat-extensions` configure flag or remove dead code.

### F2: Time Learning Not Implemented
**File:** `src/config/config.c:203-205`  
**Severity:** Low  
**Description:** Time-of-day learning is logged as enabled but has no implementation.

```c
if (kp_conf->preheat.enable_time_learning) {
    g_message("Time-of-day learning ENABLED");
}
```

**Action:** Remove or mark as "reserved for future use".

### F3: Tool Scoring Extensions Not Implemented
**File:** `src/config/config.c:199-202`  
**Severity:** Low  
**Description:** Preheat-specific scoring is mentioned but not implemented.

**Action:** Remove or mark clearly as "planned feature".

---

## Man Page Issues

### M1: Outdated Path References
**File:** `man/preheat.8`, `man/preheat.conf.5`, `man/preheat-ctl.1`  
**Severity:** Low  
**Description:** Should verify man pages reference correct `/usr/local/` paths vs system `/var/` paths.

**Action:** Review all three man pages for path consistency.

---

## Minor Issues

### MIN1: Typo in Comment
**File:** `src/predict/prophet.c:78` 
**Description:** "what should we do we correlation" - double "we"

### MIN2: Unused Variable Warnings
**File:** Multiple files with `G_GNUC_UNUSED` markers  
**Description:** These are intentional (callback signatures), not bugs.

### MIN3: Magic Number in Priority
**File:** `src/predict/prophet.c:253`  
**Description:** Manual apps get hardcoded `-10.0` boost.

**Recommended:** Define constant:
```c
#define MANUAL_APP_BOOST_LNPROB -10.0
```

### MIN4: Missing Error Macro Consistency
**File:** `src/state/state.c:920-928`  
**Description:** Uses `G_FILE_ERROR_ACCES` but checks `err->code` directly instead of using `g_error_matches()`.

---

## Statistics

**Total Issues:** 32  
- **Critical:** 3  
- **Medium:** 7  
- **Low/Minor:** 22

**By Category:**
- Code Quality: 5
- Documentation: 3
- Build System: 4
- Configuration: 2
- Security: 3  
- Features: 3
- Man Pages: 1
- Minor/Cosmetic: 11

---

## Recommended Priority

### Immediate (Before v0.2.0)
1. Fix integer overflow in comparison functions (C1)
2. Fix broken debug macro (Q2)
3. Add config validation (CF1)
4. Verify/fix CRC32 loading (S1)

### Short-term (v0.2.x)
1. Fix documentation path inconsistencies (D1, D2)
2. Implement missing tests or remove directories (B1)
3. Clean up unimplemented features (F1, F2, F3)

### Long-term / Nice-to-have
1. Improve error handling throughout (Q4, Q5)
2. Enhance build system feedback (B3, B4)
3. Fix cosmetic issues (MIN1-4)
