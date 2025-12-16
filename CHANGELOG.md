# Changelog

All notable changes to preheat will be documented in this file.

## [0.1.1] - 2025-12-16

### Added
- `preheat --self-test` command for system diagnostics
- `preheat-ctl mem` command to show memory statistics
- `preheat-ctl predict` command to show tracked applications
- CRC32 checksum footer for state file integrity
- Competing daemon detection (systemd-readahead, ureadahead, preload)
- Graceful /proc read failure handling with auto-recovery

### Fixed
- State file corruption no longer crashes daemon (renames to .broken.TIMESTAMP)
- fsync() before rename() prevents data loss on power failure
- Permission errors on state file now log warning instead of aborting

### Security
- State file integrity verified via CRC32 on load
- Backward compatible: old state files still load correctly

## [0.1.0] - 2025-12-16

### Added
- Initial release based on preload 0.6.4
- Adaptive readahead daemon with Markov chain prediction
- Systemd service with security hardening
- Manual application whitelist support (`manualapps` config)
- CLI control tool (`preheat-ctl`)
- Man pages for daemon, config, and CLI
- Comprehensive documentation

### Security
- Systemd hardening (NoNewPrivileges, ProtectSystem, etc.)
- State file permissions restricted to 0600
- O_NOFOLLOW protection on state file writes

### Changed
- Renamed from preload to preheat
- Target platform: Debian-based distributions (including Kali Linux)
- Default install prefix: /usr/local

### Credits
- Based on preload by Behdad Esfahbod
- Built with Antigravity assistance
