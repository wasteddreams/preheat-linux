#!/bin/bash
# setup-whitelist.sh - Configure manual app whitelist for Preheat
#
# This script creates the whitelist directory and file, adds specified apps,
# updates the config, and reloads the daemon.

set -e

WHITELIST_DIR="/etc/preheat.d"
WHITELIST_FILE="$WHITELIST_DIR/apps.list"
CONFIG_FILE="/usr/local/etc/preheat.conf"

echo "=== Preheat Whitelist Setup ==="
echo

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo $0)"
    exit 1
fi

# Create directory
echo "[1/4] Creating whitelist directory..."
mkdir -p "$WHITELIST_DIR"

# Create whitelist file with apps
echo "[2/4] Creating whitelist file..."
cat > "$WHITELIST_FILE" << 'EOF'
# Preheat Manual Whitelist
# Apps listed here will always be preloaded with highest priority

/usr/bin/firefox
/usr/bin/antigravity
EOF

echo "     Added apps:"
grep -v '^#' "$WHITELIST_FILE" | grep -v '^$' | while read app; do
    echo "       - $app"
done

# Update config file
echo "[3/4] Updating configuration..."
if grep -q "^manualapps" "$CONFIG_FILE" 2>/dev/null; then
    # Update existing line
    sed -i "s|^manualapps.*|manualapps = $WHITELIST_FILE|" "$CONFIG_FILE"
    echo "     Updated existing manualapps setting"
elif grep -q "^# manualapps" "$CONFIG_FILE" 2>/dev/null; then
    # Uncomment and set
    sed -i "s|^# manualapps.*|manualapps = $WHITELIST_FILE|" "$CONFIG_FILE"
    echo "     Enabled manualapps setting"
else
    # Add to [system] section
    sed -i "/^\[system\]/a manualapps = $WHITELIST_FILE" "$CONFIG_FILE"
    echo "     Added manualapps to [system] section"
fi

# Reload daemon
echo "[4/4] Reloading daemon..."
if command -v preheat-ctl &> /dev/null; then
    preheat-ctl reload 2>/dev/null || echo "     Note: Daemon not running, will apply on next start"
else
    echo "     Note: preheat-ctl not found, reload manually"
fi

echo
echo "=== Setup Complete ==="
echo "Whitelist: $WHITELIST_FILE"
echo
echo "To add more apps:"
echo "  sudo nano $WHITELIST_FILE"
echo "  sudo preheat-ctl reload"
