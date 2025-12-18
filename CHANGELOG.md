# Changelog

All notable changes to the Preheat project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.2] - 2025-12-18

### Added
- **Interactive whitelist onboarding** during installation
  - Optional application whitelist configuration during install
  - TTY detection for CI/CD safety
  - Comprehensive path validation (absolute paths, executable check)
  - Graceful handling of invalid inputs

- **Smart uninstaller** with data preservation options
  - `--keep-data` flag (default behavior)
  - `--purge-data` flag for complete removal
  - Interactive prompts with double confirmation
  - Non-interactive mode defaults to preserving data
  - Help text via `--help` flag

- **Reinstall script** for easy upgrades
  - `reinstall.sh` combines uninstall + install
  - `--clean` flag for fresh installation
  - Preserves learned data by default
  - Safety confirmations for destructive operations

- **Update mechanism** via preheat-ctl
  - `preheat-ctl update` command
  - Automatic dependency checking
  - Comprehensive backup before update
  - Automatic rollback on failure
  - Preserves all configuration and state

- **Comprehensive testing suite**
  - Edge case testing (24+ scenarios)
  - Stability testing (25+ checks)
  - Runtime stress tests
  - Memory leak detection
  - File descriptor management validation

- **Production-grade audit and fixes**
  - Complete security audit (AUDIT_REPORT.md)
  - Memory safety verification
  - Signal handling review
  - State file integrity validation
  - Error handling coverage analysis

- **Enhanced documentation**
  - STATE_RESTRICTIONS.md - Security constraints documentation
  - STABILITY_TESTING_REPORT.md - Stability test results
  - Comprehensive inline code comments
  - Migration warnings in INSTALL.md

### Fixed
- **P1 Audit Issues:**
  - Update script path resolution with multiple fallback locations
  - Dependency validation before installation (prevents mid-install failures)
  - ProtectHome systemd restriction properly documented
  
- **Bash syntax error** in install.sh whitelist prompt
- **Install script** now validates all dependencies before proceeding
- **Update command** provides detailed manual instructions if script not found
- **Error messages** improved throughout for better user experience

### Changed
- Install script now checks dependencies **before** downloading or building
- Uninstall script defaults to **preserving data** (safer default)
- Update script path detection uses fallback logic for flexibility
- systemd service file includes security restriction documentation
- All lifecycle scripts support `--help` flag

### Security
- Documented ProtectHome=read-only restriction in systemd service
- Added comprehensive input validation for whitelist entries
- State files use secure permissions (600)
- No execution of user-provided paths without validation
- All shell inputs properly quoted to prevent injection

### Documentation
- Updated README.md with new lifecycle features
- Added state migration warnings (one-way compatibility)
- Created comprehensive testing reports
- Documented all P1 audit fixes
- Added inline comments for security-critical code

### Testing
- 100% pass rate on edge case testing (24 tests)
- 100% pass rate on stability testing (25+ tests)
- Syntax validation for all bash scripts
- Memory leak static analysis
- File descriptor leak testing

### Performance
- No performance regressions
- Minimal overhead from lifecycle features (install-time only)
- Daemon runtime unchanged

### Notes
- **Breaking:** None - Fully backward compatible
- **Migration:** State files from v0.1.0 are compatible
- **Upgrade Path:** Use `reinstall.sh` or `preheat-ctl update`

---

## [0.1.0] - 2024-12-14

### Added
- Initial release of Preheat daemon
- Adaptive application preloading based on Markov chains
- Process monitoring via /proc filesystem
- Manual application whitelist support
- systemd integration with security hardening
- Configuration file support
- State persistence with periodic saves
- CLI management tool (preheat-ctl)
- Self-test diagnostics (`--self-test`)
- CRC32 checksums for state file integrity
- Corruption detection and recovery
- Competing daemon detection
- Comprehensive documentation

### Features
- Markov chain-based prediction
- Frequency analysis
- Time-of-day pattern learning
- Readahead sorting strategies (path, block, inode)
- Memory pressure awareness
- Configurable cycle time and thresholds
- Signal handling (SIGHUP, SIGUSR1, SIGUSR2, SIGTERM)

### Documentation
- README.md with quick start guide
- INSTALL.md with detailed installation steps
- Configuration examples
- Man pages for daemon and CLI

---

## Links

- **Repository:** https://github.com/wasteddreams/preheat-linux
- **Issues:** https://github.com/wasteddreams/preheat-linux/issues
- **License:** GPL v2

---

## Version Tagging Convention

- **Major.Minor.Patch** (Semantic Versioning)
- **Major:** Breaking changes
- **Minor:** New features (backward compatible)
- **Patch:** Bug fixes and minor improvements
