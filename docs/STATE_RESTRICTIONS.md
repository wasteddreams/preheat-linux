# Preheat State File Restrictions

## ProtectHome Security Setting

The preheat systemd service uses `ProtectHome=read-only` as a security hardening measure. This has the following implications:

### Restrictions

**State and configuration files CANNOT be placed in:**
- User home directories (`/home/*`)
- `/root`
- Any path under home directories

### Required Locations

**All preheat files MUST use system directories:**
- State files: `/usr/local/var/lib/preheat/` or `/var/lib/preheat/`
- Configuration: `/usr/local/etc/preheat.conf` or `/etc/preheat.conf`
- Manual whitelist: `/etc/preheat.d/apps.list`
- Logs: `/usr/local/var/log/preheat.log` or `/var/log/preheat.log`

### Rationale

This restriction is **intentional and by design**:
- Prevents potential security issues with user-writable state
- Enforces system-wide operation (not per-user)
- Follows principle of least privilege
- Aligns with systemd security best practices

### Future Development

Any future features must respect this constraint. User-specific preloading is **not supported** and would require architectural changes to the security model.

## State File Migration Warning

⚠️ **IMPORTANT**: Preheat can import preload 0.6.4 state files, but the original preload daemon will NOT be able to read preheat state files.

**This migration is ONE-WAY.**

### Before Migrating from Preload

```bash
# Backup your preload state
sudo cp /var/lib/preload/preload.state /var/lib/preload/preload.state.backup

# Then proceed with preheat installation
```

### State File Format

- Preheat uses the same base format as preload 0.6.4
- Includes CRC32 checksums for integrity protection
- Corruption is detected and handled gracefully (renamed to `.broken`)
- Fresh start is automatic if state becomes corrupted

## Best Practices

1. **Never manually edit state files** - they use binary format
2. **Back up state before major updates** - use the update script which does this automatically
3. **Monitor state file size** - should typically be <10MB for normal use
4. **Check for `.broken` files** - indicates corruption occurred

## Troubleshooting

If state file issues occur:

```bash
# Check state file integrity
sudo ls -lh /usr/local/var/lib/preheat/

# Remove corrupted state (daemon will start fresh)
sudo rm /usr/local/var/lib/preheat/preheat.state

# Restart daemon
sudo systemctl restart preheat
```

The daemon handles corruption gracefully - this is just for manual intervention if needed.
