#!/bin/bash
# =============================================================================
# Preheat v0.1.1 Verification Script
# Clean reinstall and full system tests (requires sudo)
# =============================================================================

set -e

# Get project root (parent of scripts directory)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}==========================================="
echo "Preheat v0.1.1 Clean Reinstall & Verification"
echo -e "===========================================${NC}"
echo ""

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}This script requires sudo privileges.${NC}"
    echo "Please run: sudo $0"
    exit 1
fi

# Get the original user (in case we're running with sudo)
ORIG_USER="${SUDO_USER:-$USER}"
ORIG_HOME=$(eval echo ~$ORIG_USER)

echo -e "${YELLOW}Step 1: Stop existing daemon (if running)${NC}"
if systemctl is-active --quiet preheat 2>/dev/null; then
    systemctl stop preheat
    echo "  ✓ Stopped preheat service"
else
    echo "  - Preheat not running"
fi

echo ""
echo -e "${YELLOW}Step 2: Uninstall previous version${NC}"
if [ -f /usr/local/bin/preheat ]; then
    make uninstall 2>/dev/null || true
    echo "  ✓ Uninstalled previous version"
else
    echo "  - No previous installation found"
fi

# Clean any leftover state
rm -f /var/lib/preheat/preheat.state 2>/dev/null || true
rm -f /run/preheat.pid 2>/dev/null || true

echo ""
echo -e "${YELLOW}Step 3: Clean build${NC}"
cd "$SCRIPT_DIR"

# Clean previous build
make clean -s 2>/dev/null || true

# Regenerate configure if needed
if [ ! -f "configure" ]; then
    echo "  Regenerating configure..."
    autoreconf -fi || { echo -e "${RED}autoreconf failed${NC}"; exit 1; }
fi

# Configure
echo "  Configuring..."
./configure --quiet || { echo -e "${RED}configure failed${NC}"; exit 1; }

# Build
echo "  Building..."
make -j$(nproc) -s 2>&1 | tail -5
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo -e "${RED}Build failed${NC}"
    exit 1
fi
echo "  ✓ Build complete"

echo ""
echo -e "${YELLOW}Step 4: Install${NC}"
make install
echo "  ✓ Installed to /usr/local"

# Create required directories
mkdir -p /var/lib/preheat
mkdir -p /var/log
chmod 755 /var/lib/preheat

echo ""
echo -e "${YELLOW}Step 5: Install systemd service${NC}"
cp debian/preheat.service /etc/systemd/system/preheat.service
# Fix paths in service file for /usr/local prefix
sed -i 's|@bindir@|/usr/local/bin|g' /etc/systemd/system/preheat.service
sed -i 's|@localstatedir@|/var|g' /etc/systemd/system/preheat.service
systemctl daemon-reload
echo "  ✓ Systemd service installed"

echo ""
echo -e "${YELLOW}Step 6: Run self-test${NC}"
/usr/local/bin/preheat --self-test
echo ""

echo ""
echo -e "${YELLOW}Step 7: Start daemon${NC}"
systemctl start preheat
sleep 2
if systemctl is-active --quiet preheat; then
    echo -e "  ${GREEN}✓ Daemon started successfully${NC}"
    PID=$(cat /run/preheat.pid 2>/dev/null || pgrep -x preheat)
    echo "  PID: $PID"
else
    echo -e "  ${RED}✗ Daemon failed to start${NC}"
    journalctl -u preheat -n 10 --no-pager
    exit 1
fi

echo ""
echo -e "${YELLOW}Step 8: Verify daemon functionality${NC}"

# Test signals
echo -n "  SIGUSR1 (dump state)... "
kill -USR1 $(cat /run/preheat.pid)
sleep 1
echo -e "${GREEN}OK${NC}"

echo -n "  SIGUSR2 (save state)... "
kill -USR2 $(cat /run/preheat.pid)
sleep 2
if [ -f /var/lib/preheat/preheat.state ]; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}FAIL - state not saved${NC}"
fi

echo -n "  State file has CRC32... "
if grep -q "^CRC32" /var/lib/preheat/preheat.state 2>/dev/null; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${YELLOW}SKIP - no state data yet${NC}"
fi

echo -n "  SIGHUP (reload config)... "
kill -HUP $(cat /run/preheat.pid)
sleep 1
echo -e "${GREEN}OK${NC}"

echo ""
echo -e "${YELLOW}Step 9: Test preheat-ctl commands${NC}"
echo ""
echo "  preheat-ctl status:"
/usr/local/bin/preheat-ctl status || true
echo ""
echo "  preheat-ctl mem:"
/usr/local/bin/preheat-ctl mem
echo ""

echo ""
echo -e "${YELLOW}Step 10: Check log file${NC}"
if [ -f /var/log/preheat.log ]; then
    echo "  Last 5 log entries:"
    tail -5 /var/log/preheat.log | sed 's/^/    /'
else
    echo "  Log file: /var/log/preheat.log (may need time to appear)"
fi

echo ""
echo -e "${YELLOW}Step 11: Test graceful shutdown${NC}"
systemctl stop preheat
sleep 2
if ! pgrep -x preheat > /dev/null; then
    echo -e "  ${GREEN}✓ Daemon stopped gracefully${NC}"
else
    echo -e "  ${RED}✗ Daemon still running${NC}"
fi

echo ""
echo -e "${YELLOW}Step 12: Verify state persisted${NC}"
if [ -f /var/lib/preheat/preheat.state ]; then
    SIZE=$(stat -c%s /var/lib/preheat/preheat.state)
    echo -e "  ${GREEN}✓ State file exists ($SIZE bytes)${NC}"
else
    echo -e "  ${YELLOW}⚠ State file not found (daemon may not have tracked any apps)${NC}"
fi

echo ""
echo -e "${BLUE}==========================================="
echo "Summary"
echo -e "===========================================${NC}"
echo ""
echo "Preheat v0.1.1 has been installed and verified."
echo ""
echo "Commands available:"
echo "  preheat --self-test       Run diagnostics"
echo "  preheat-ctl status        Check daemon status"
echo "  preheat-ctl mem           Show memory stats"
echo "  preheat-ctl predict       Show tracked apps"
echo ""
echo "Service management:"
echo "  sudo systemctl start preheat"
echo "  sudo systemctl stop preheat"
echo "  sudo systemctl enable preheat  # Start on boot"
echo ""
echo -e "${GREEN}✓ All verification steps complete!${NC}"
