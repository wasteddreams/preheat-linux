#!/bin/bash
#
# Preheat One-Line Installer
# curl -fsSL https://raw.githubusercontent.com/wasteddreams/preheat-linux/main/install.sh | sudo bash
#

set -e

# Colors and formatting
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# Spinner animation
spinner() {
    local pid=$1
    local msg=$2
    local spinstr='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
    local i=0
    
    while kill -0 $pid 2>/dev/null; do
        local temp=${spinstr:i++%${#spinstr}:1}
        printf "\r${CYAN}${temp}${NC} ${msg}..."
        sleep 0.1
    done
    printf "\r${GREEN}✓${NC} ${msg}... ${GREEN}done${NC}\n"
}

# Progress bar
progress_bar() {
    local current=$1
    local total=$2
    local width=40
    local percent=$((current * 100 / total))
    local filled=$((current * width / total))
    
    printf "\r${CYAN}Progress:${NC} ["
    printf "%${filled}s" | tr ' ' '█'
    printf "%$((width - filled))s" | tr ' ' '░'
    printf "] ${BOLD}%d%%${NC}" $percent
}

# Header
clear
echo ""
echo -e "${MAGENTA}${BOLD}╔════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${MAGENTA}${BOLD}║                                                                ║${NC}"
echo -e "${MAGENTA}${BOLD}║                    🚀 PREHEAT INSTALLER                         ║${NC}"
echo -e "${MAGENTA}${BOLD}║                                                                ║${NC}"
echo -e "${MAGENTA}${BOLD}║          Adaptive Readahead • Faster App Launches              ║${NC}"
echo -e "${MAGENTA}${BOLD}║                                                                ║${NC}"
echo -e "${MAGENTA}${BOLD}╚════════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check root
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}${BOLD}✗ Error:${NC} This script must be run as root"
    echo -e "${DIM}  Try: ${CYAN}sudo $0${NC}"
    exit 1
fi

# Validate dependencies before proceeding
echo -e "${CYAN}Checking prerequisites...${NC}"
MISSING=""
MISSING_PKGS=""

# Check for required commands
for cmd in git autoconf automake pkg-config gcc make; do
    if ! command -v $cmd &>/dev/null; then
        MISSING="$MISSING $cmd"
        case $cmd in
            git)
                MISSING_PKGS="$MISSING_PKGS git"
                ;;
            autoconf|automake)
                MISSING_PKGS="$MISSING_PKGS autoconf automake"
                ;;
            pkg-config)
                MISSING_PKGS="$MISSING_PKGS pkg-config"
                ;;
            gcc|make)
                MISSING_PKGS="$MISSING_PKGS build-essential"
                ;;
        esac
    fi
done

# Check for GLib development headers
if ! pkg-config --exists glib-2.0 2>/dev/null; then
    MISSING="$MISSING libglib2.0-dev"
    MISSING_PKGS="$MISSING_PKGS libglib2.0-dev"
fi

if [ -n "$MISSING" ]; then
    echo -e "${RED}${BOLD}✗ Missing dependencies:${NC}$MISSING"
    echo ""
    echo -e "${YELLOW}Install with:${NC}"
    # Remove duplicates and format
    UNIQUE_PKGS=$(echo "$MISSING_PKGS" | tr ' ' '\n' | sort -u | tr '\n' ' ')
    echo -e "${CYAN}  apt-get install$UNIQUE_PKGS${NC}"
    echo ""
    exit 1
fi

echo -e "${GREEN}      ✓ All prerequisites present${NC}"
echo ""

# System info
echo -e "${DIM}┌─ System Information${NC}"
echo -e "${DIM}│${NC} OS: $(lsb_release -ds 2>/dev/null || echo "Unknown")"
echo -e "${DIM}│${NC} Kernel: $(uname -r)"
echo -e "${DIM}│${NC} RAM: $(free -h | awk '/^Mem:/{print $2}')"
echo -e "${DIM}└───────────────────${NC}"
echo ""

# Step counter
STEP=0
TOTAL_STEPS=5

# Step 1: Dependencies
((STEP++))
progress_bar $STEP $TOTAL_STEPS
echo ""
echo -e "${CYAN}${BOLD}[${STEP}/${TOTAL_STEPS}]${NC} Installing dependencies..."

apt-get update -qq > /tmp/preheat_install.log 2>&1 &
spinner $! "Updating package lists"

apt-get install -y -qq git autoconf automake pkg-config libglib2.0-dev >> /tmp/preheat_install.log 2>&1 &
spinner $! "Installing build tools"

echo -e "${GREEN}      ✓ Dependencies installed${NC}"
echo ""

# Step 2: Download
((STEP++))
progress_bar $STEP $TOTAL_STEPS
echo ""
echo -e "${CYAN}${BOLD}[${STEP}/${TOTAL_STEPS}]${NC} Downloading source code..."

TMPDIR=$(mktemp -d)
git clone --quiet https://github.com/wasteddreams/preheat-linux.git "$TMPDIR/preheat" >> /tmp/preheat_install.log 2>&1 &
spinner $! "Cloning repository"

cd "$TMPDIR/preheat"
COMMIT=$(git rev-parse --short HEAD)
echo -e "${GREEN}      ✓ Downloaded${NC} ${DIM}(commit: $COMMIT)${NC}"
echo ""

# Step 3: Build
((STEP++))
progress_bar $STEP $TOTAL_STEPS
echo ""
echo -e "${CYAN}${BOLD}[${STEP}/${TOTAL_STEPS}]${NC} Building from source..."

autoreconf --install --force >> /tmp/preheat_install.log 2>&1 &
spinner $! "Generating build system"

./configure --quiet >> /tmp/preheat_install.log 2>&1 &
spinner $! "Configuring"

make -j$(nproc) --quiet >> /tmp/preheat_install.log 2>&1 &
spinner $! "Compiling (using $(nproc) cores)"

echo -e "${GREEN}      ✓ Build successful${NC}"
echo ""

# Step 4: Install
((STEP++))
progress_bar $STEP $TOTAL_STEPS
echo ""
echo -e "${CYAN}${BOLD}[${STEP}/${TOTAL_STEPS}]${NC} Installing system-wide..."

make install --quiet >> /tmp/preheat_install.log 2>&1 &
spinner $! "Installing binaries"

systemctl daemon-reload
echo -e "${GREEN}      ✓ Installed to /usr/local${NC}"
echo ""

# Step 5: Setup
((STEP++))
progress_bar $STEP $TOTAL_STEPS
echo ""
echo -e "${CYAN}${BOLD}[${STEP}/${TOTAL_STEPS}]${NC} Configuring service..."

# Ask about autostart
echo ""
echo -e "${YELLOW}Enable automatic startup on boot?${NC}"
read -p "$(echo -e ${BOLD}Choose [Y/n]:${NC} )" choice
choice=${choice:-Y}

if [[ "$choice" =~ ^[Yy]$ ]]; then
    systemctl enable preheat.service >> /tmp/preheat_install.log 2>&1
    systemctl start preheat.service
    
    # Wait a moment and check status
    sleep 1
    if systemctl is-active --quiet preheat.service; then
        echo -e "${GREEN}      ✓ Service enabled and running${NC}"
    else
        echo -e "${YELLOW}      ⚠ Service enabled but start failed${NC}"
        echo -e "${DIM}        Check: systemctl status preheat${NC}"
    fi
else
    echo -e "${YELLOW}      ○ Autostart skipped${NC}"
    echo -e "${DIM}        Enable later: ${CYAN}systemctl enable preheat${NC}"
fi

# Whitelist configuration prompt (Phase 1: Install-Time Whitelist Onboarding)
echo ""
if [ -t 0 ]; then  # Interactive TTY check
    echo -e "${YELLOW}Add applications to whitelist for priority preloading?${NC}"
    echo -e "${DIM}  Examples: /usr/bin/firefox /usr/bin/code /usr/bin/burpsuite${NC}"
    read -p "$(echo -e ${BOLD})Enter paths (space-separated) or press Enter to skip:${NC} " whitelist_input
    
    if [ -n "$whitelist_input" ]; then
        mkdir -p /etc/preheat.d
        > /etc/preheat.d/apps.list  # Create empty file
        
        valid_count=0
        invalid_count=0
        
        for app_path in $whitelist_input; do
            # Validate: absolute path, exists, executable
            if [[ "$app_path" == /* ]] && [ -x "$app_path" ]; then
                echo "$app_path" >> /etc/preheat.d/apps.list
                echo -e "${GREEN}      ✓ Added: $app_path${NC}"
                ((valid_count++))
            else
                echo -e "${YELLOW}      ⚠ Skipped (invalid): $app_path${NC}"
                ((invalid_count++))
            fi
        done
        
        if [ $valid_count -gt 0 ]; then
            echo -e "${GREEN}      ✓ Whitelist configured ($valid_count apps)${NC}"
            echo "$valid_count application(s) added to whitelist" >> /tmp/preheat_install.log
        else
            rm -f /etc/preheat.d/apps.list
            echo -e "${DIM}      No valid apps added${NC}"
        fi
        
        if [ $invalid_count -gt 0 ]; then
            echo -e "${DIM}        ($invalid_count invalid entries skipped)${NC}"
        fi
    else
        echo -e "${DIM}      Whitelist skipped${NC}"
    fi
fi

# Cleanup
rm -rf "$TMPDIR"

# Final progress
echo ""
progress_bar $TOTAL_STEPS $TOTAL_STEPS
echo ""
echo ""

# Success banner
echo -e "${GREEN}${BOLD}╔════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}${BOLD}║                                                                ║${NC}"
echo -e "${GREEN}${BOLD}║                    ✨ INSTALLATION COMPLETE ✨                   ║${NC}"
echo -e "${GREEN}${BOLD}║                                                                ║${NC}"
echo -e "${GREEN}${BOLD}║              Preheat is now optimizing your system             ║${NC}"
echo -e "${GREEN}${BOLD}║                                                                ║${NC}"
echo -e "${GREEN}${BOLD}╚════════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Useful commands box
echo -e "${CYAN}${BOLD}Quick Commands:${NC}"
echo -e "${DIM}┌────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${DIM}│${NC} ${BOLD}Check status:${NC}      systemctl status preheat                  ${DIM}│${NC}"
echo -e "${DIM}│${NC} ${BOLD}View logs:${NC}         tail -f /usr/local/var/log/preheat.log   ${DIM}│${NC}"
echo -e "${DIM}│${NC} ${BOLD}Reload config:${NC}     preheat-ctl reload                       ${DIM}│${NC}"
echo -e "${DIM}│${NC} ${BOLD}Show stats:${NC}        preheat-ctl dump                         ${DIM}│${NC}"
echo -e "${DIM}└────────────────────────────────────────────────────────────────┘${NC}"
echo ""

echo -e "${YELLOW}${BOLD}💡 Tip:${NC} Give preheat a few hours to learn your usage patterns."
echo -e "${DIM}   You'll notice faster application launches after the learning period.${NC}"
echo ""

# Installation log
echo -e "${DIM}Full installation log: /tmp/preheat_install.log${NC}"
echo ""
