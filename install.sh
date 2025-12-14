#!/bin/bash
#
# Kali-Preload One-Line Installer
# curl -fsSL https://raw.githubusercontent.com/wasteddreams/preheat/main/install.sh | sudo bash
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo ""
echo -e "${BLUE}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║          KALI-PRELOAD INSTALLER                           ║${NC}"
echo -e "${BLUE}║          Make your apps launch faster                     ║${NC}"
echo -e "${BLUE}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check root
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}Run this with sudo!${NC}"
    exit 1
fi

# Install deps
echo -e "${YELLOW}Installing dependencies...${NC}"
apt-get update -qq
apt-get install -y -qq git autoconf automake pkg-config libglib2.0-dev >/dev/null 2>&1
echo -e "${GREEN}✓ Dependencies installed${NC}"

# Clone
TMPDIR=$(mktemp -d)
echo -e "${YELLOW}Downloading...${NC}"
git clone --quiet https://github.com/wasteddreams/preheat.git "$TMPDIR/preheat"
cd "$TMPDIR/preheat"
echo -e "${GREEN}✓ Downloaded${NC}"

# Build
echo -e "${YELLOW}Building...${NC}"
autoreconf --install --force >/dev/null 2>&1
./configure --quiet >/dev/null 2>&1
make -j$(nproc) --quiet >/dev/null 2>&1
echo -e "${GREEN}✓ Built${NC}"

# Install
echo -e "${YELLOW}Installing...${NC}"
make install --quiet >/dev/null 2>&1
echo -e "${GREEN}✓ Installed${NC}"

# Systemd
systemctl daemon-reload

# Ask about autostart
echo ""
read -p "Enable autostart on boot? [Y/n]: " choice
choice=${choice:-Y}

if [[ "$choice" =~ ^[Yy]$ ]]; then
    systemctl enable preheat.service >/dev/null 2>&1
    systemctl start preheat.service
    echo -e "${GREEN}✓ Service enabled and started${NC}"
else
    echo -e "${YELLOW}Skipped autostart. Run 'systemctl enable preheat' later.${NC}"
fi

# Cleanup
rm -rf "$TMPDIR"

echo ""
echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  DONE! Kali-Preload is now running.                       ║${NC}"
echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Useful commands:"
echo "  systemctl status preheat    - Check status"
echo "  preheat-ctl reload          - Reload config"
echo "  tail -f /usr/local/var/log/preheat.log  - Watch logs"
echo ""
echo "Give it a few hours to learn your patterns. Enjoy faster apps!"
echo ""
