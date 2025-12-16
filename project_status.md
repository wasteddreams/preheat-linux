# Preheat v0.1.1 — Project Status

## Phase 1: Core Reliability — Completed 2025-12-16

### Completion Status
- [x] State file hardening (fsync + atomic write)
- [x] CRC32 footer for integrity checking
- [x] Corruption auto-recovery with .broken.TIMESTAMP rename
- [x] Graceful /proc failure handling
- [x] All tests passed
- [x] No behavioral regressions

### Implementation Summary

**Files Modified:**
- `src/state/state.c` — Added CRC32 footer, fsync, corruption recovery
- `src/monitor/proc.c` — Graceful /proc failure handling
- `src/Makefile.am` — Added CRC32 sources

**Files Created:**
- `src/utils/crc32.c` / `crc32.h`
- `tests/integration/test_state_hardening.sh`

**Key Changes:**
1. `write_state()` now calculates and appends `CRC32\t<hex>` footer
2. `kp_state_save()` uses O_RDWR, calls fsync() before atomic rename
3. `kp_state_load()` uses `handle_corrupt_statefile()` instead of g_error()
4. `kp_proc_foreach()` logs once and skips cycle on /proc failure

### Testing Results

```
Total Tests: 6
Passed:      6
Failed:      0

✓ ALL STATE HARDENING TESTS PASSED
```

**Test Commands:**
```bash
./tests/integration/test_state_hardening.sh
./tests/integration/smoke_test.sh
```

### Verification Evidence

**CRC32 Footer:** `CRC32   60504B3F`

**Corruption Recovery:** `State file corrupt... renamed to .broken.TIMESTAMP - starting fresh`

### Decisions Made

| Decision | Rationale |
|----------|-----------|
| CRC32 as footer line | Backward compatible, simple text format |
| O_RDWR instead of O_WRONLY | Required to read file for CRC calculation |
| g_critical instead of g_error | Prevents crash on permission issues |
| Log-once for /proc failures | Avoids log spam during transient issues |

### Known Issues / Technical Debt

- Pre-existing: Format specifier warnings in upstream code
- Pre-existing: Function type cast warnings (GLib callback signatures)

### Next Phase Readiness
- [x] All phases complete
- [x] Tests passing
- [x] CHANGELOG.md updated

---

## Phase 2: Observability Foundation — Completed 2025-12-16

- [x] `preheat --self-test` - 4 diagnostic checks
- [x] `preheat-ctl mem` - Memory statistics
- [x] `preheat-ctl predict` - State file parsing

---

## Phase 3: Safety & Conflict Prevention — Completed 2025-12-16

- [x] Competing daemon detection at startup
- [x] Logs warnings with remediation steps

---

## Phase 4: Distribution Readiness — Completed 2025-12-16

- [x] Reproducible builds verified (identical hashes)
- [x] CHANGELOG.md updated for v0.1.1
- [x] Ready for release tag
