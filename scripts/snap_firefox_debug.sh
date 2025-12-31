#!/bin/bash
#
# Snap Firefox Debug Script
# Tests preheat daemon's ability to track snap-installed Firefox
# Based on SNAP_FIREFOX_DEBUG_NOTES.md and ROOT_CAUSE_ANALYSIS.md
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Dividers
DIV="============================================="
SUBDIV="---------------------------------------------"

echo -e "${CYAN}${DIV}${NC}"
echo -e "${CYAN}   Snap Firefox Debug Script for Preheat${NC}"
echo -e "${CYAN}${DIV}${NC}"
echo ""

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    echo -e "${YELLOW}[WARN] Some tests require root. Re-run with sudo for full diagnostics.${NC}"
    echo ""
fi

#######################################
# Phase 1: Firefox Installation Check
#######################################
echo -e "${BLUE}[Phase 1] Firefox Installation Check${NC}"
echo "$SUBDIV"

# Check snap firefox
if snap list firefox &>/dev/null; then
    SNAP_VERSION=$(snap list firefox 2>/dev/null | tail -n1 | awk '{print $2}')
    SNAP_REV=$(snap list firefox 2>/dev/null | tail -n1 | awk '{print $3}')
    echo -e "${GREEN}✓ Firefox snap found: version=$SNAP_VERSION rev=$SNAP_REV${NC}"
    
    # Get confinement
    CONFINEMENT=$(snap info firefox 2>/dev/null | grep -E "^confinement:" | awk '{print $2}')
    echo -e "  Confinement: ${YELLOW}$CONFINEMENT${NC}"
else
    echo -e "${RED}✗ Firefox snap NOT installed${NC}"
    echo "  Install with: sudo snap install firefox"
    exit 1
fi

# Check for apt firefox
if dpkg -l firefox 2>/dev/null | grep -q '^ii'; then
    echo -e "${YELLOW}⚠ Firefox also installed via apt (may cause conflicts)${NC}"
fi

echo ""

#######################################
# Phase 2: Firefox Process Detection
#######################################
echo -e "${BLUE}[Phase 2] Firefox Process Detection${NC}"
echo "$SUBDIV"

FIREFOX_PIDS=$(pgrep -d ' ' firefox 2>/dev/null || true)
if [[ -z "$FIREFOX_PIDS" ]]; then
    echo -e "${YELLOW}⚠ Firefox not running. Starting Firefox...${NC}"
    nohup firefox &>/dev/null &
    sleep 3
    FIREFOX_PIDS=$(pgrep -d ' ' firefox 2>/dev/null || true)
fi

if [[ -z "$FIREFOX_PIDS" ]]; then
    echo -e "${RED}✗ Failed to start Firefox${NC}"
    exit 1
fi

# Get the main firefox process (usually lowest PID or parent)
MAIN_PID=$(pgrep -n firefox 2>/dev/null || echo "")
echo -e "${GREEN}✓ Firefox running with PIDs: $FIREFOX_PIDS${NC}"
echo -e "  Main PID (newest): $MAIN_PID"

echo ""

#######################################
# Phase 3: /proc Access Tests (CRITICAL)
#######################################
echo -e "${BLUE}[Phase 3] /proc Access Tests (Root Cause Verification)${NC}"
echo "$SUBDIV"

# Test ALL Firefox PIDs (oldest first - these are more stable parent processes)
ALL_PIDS=$(pgrep firefox 2>/dev/null | sort -n | head -5)
echo -e "Testing PIDs (oldest first): $ALL_PIDS"
echo ""

for PID in $ALL_PIDS; do
    # Check if process still exists
    if [[ ! -d "/proc/$PID" ]]; then
        echo -e "${YELLOW}PID $PID: Process exited (transient child)${NC}"
        continue
    fi
    
    echo -e "${CYAN}Testing PID: $PID${NC}"
    
    # Test /proc/PID/exe
    echo -n "  /proc/$PID/exe: "
    if [[ -e "/proc/$PID/exe" ]]; then
        if EXE_PATH=$(readlink "/proc/$PID/exe" 2>&1); then
            echo -e "${GREEN}✓ Readable: $EXE_PATH${NC}"
        else
            echo -e "${RED}✗ BLOCKED (Permission denied)${NC}"
        fi
    else
        echo -e "${YELLOW}⚠ Process exited${NC}"
        continue
    fi
    
    # Test /proc/PID/cmdline
    echo -n "  /proc/$PID/cmdline: "
    if [[ -f "/proc/$PID/cmdline" ]]; then
        if CMDLINE=$(cat "/proc/$PID/cmdline" 2>/dev/null | tr '\0' ' ' | cut -c1-80); then
            if [[ -n "$CMDLINE" ]]; then
                echo -e "${GREEN}✓ Readable: ${CMDLINE}${NC}"
            else
                echo -e "${YELLOW}⚠ Empty (kernel thread?)${NC}"
            fi
        else
            echo -e "${RED}✗ BLOCKED${NC}"
        fi
    else
        echo -e "${YELLOW}⚠ Process exited${NC}"
    fi
    
    # Test /proc/PID/maps (This is the critical one!)
    echo -n "  /proc/$PID/maps: "
    if [[ -f "/proc/$PID/maps" ]]; then
        if MAPS_COUNT=$(wc -l < "/proc/$PID/maps" 2>/dev/null); then
            echo -e "${GREEN}✓ Readable: $MAPS_COUNT lines${NC}"
        else
            echo -e "${RED}✗ BLOCKED (AppArmor likely preventing access)${NC}"
        fi
    else
        echo -e "${RED}✗ File not accessible (AppArmor sandbox)${NC}"
    fi
    
    # Test /proc/PID/stat  
    echo -n "  /proc/$PID/stat: "
    if [[ -f "/proc/$PID/stat" ]] && cat "/proc/$PID/stat" &>/dev/null; then
        echo -e "${GREEN}✓ Readable${NC}"
    else
        echo -e "${RED}✗ BLOCKED${NC}"
    fi
    
    # Test /proc/PID/status
    echo -n "  /proc/$PID/status: "
    if [[ -f "/proc/$PID/status" ]] && cat "/proc/$PID/status" &>/dev/null; then
        echo -e "${GREEN}✓ Readable${NC}"
    else
        echo -e "${RED}✗ BLOCKED${NC}"
    fi
    
    echo ""
done

#######################################
# Phase 4: AppArmor Status
#######################################
echo -e "${BLUE}[Phase 4] AppArmor Status${NC}"
echo "$SUBDIV"

if command -v aa-status &>/dev/null; then
    if [[ $EUID -eq 0 ]]; then
        ENFORCED=$(aa-status 2>/dev/null | grep -A1000 "profiles are in enforce" | head -20)
        FIREFOX_PROFILE=$(echo "$ENFORCED" | grep -i "firefox" || echo "None found")
        echo "  AppArmor status: $(aa-status 2>/dev/null | head -1)"
        echo "  Firefox-related profiles: $FIREFOX_PROFILE"
    else
        echo -e "${YELLOW}  Run as root to check AppArmor profiles${NC}"
    fi
else
    echo "  AppArmor tools not installed"
fi

# Check recent AppArmor denials
echo ""
echo "  Recent AppArmor denials (last 5):"
if [[ $EUID -eq 0 ]]; then
    journalctl -k 2>/dev/null | grep -i apparmor | grep -i denied | tail -5 || echo "    No denials found"
else
    dmesg 2>/dev/null | grep -i apparmor | grep -i denied | tail -5 || echo "    Run as root for kernel logs"
fi

echo ""

#######################################
# Phase 5: Preheat Daemon Check
#######################################
echo -e "${BLUE}[Phase 5] Preheat Daemon Status${NC}"
echo "$SUBDIV"

if pgrep -x preheat &>/dev/null; then
    echo -e "${GREEN}✓ Preheat daemon is running${NC}"
    PREHEAT_PID=$(pgrep -x preheat)
    echo "  PID: $PREHEAT_PID"
else
    echo -e "${YELLOW}⚠ Preheat daemon not running${NC}"
    echo "  Start with: sudo systemctl start preheat"
fi

# Check if Firefox is tracked
echo ""
echo "  Checking if Firefox is tracked..."

SNAP_PATH="/snap/firefox/${SNAP_REV}/usr/lib/firefox/firefox"
if [[ -f "$SNAP_PATH" ]]; then
    echo "  Snap binary path: $SNAP_PATH"
    
    if command -v preheat-ctl &>/dev/null; then
        echo ""
        echo "  preheat-ctl explain output:"
        preheat-ctl explain "$SNAP_PATH" 2>&1 | head -20 | sed 's/^/    /'
        
        echo ""
        echo "  Checking stats for firefox:"
        preheat-ctl stats 2>&1 | grep -i firefox || echo "    Firefox NOT in stats"
    else
        echo -e "${YELLOW}  preheat-ctl not found in PATH${NC}"
    fi
fi

echo ""

#######################################
# Phase 6: Configuration Check
#######################################
echo -e "${BLUE}[Phase 6] Preheat Configuration${NC}"
echo "$SUBDIV"

CONFIG_FILES=(
    "/etc/preheat/preheat.conf"
    "/usr/local/etc/preheat/preheat.conf"
    "/etc/preheat.conf"
)

for cf in "${CONFIG_FILES[@]}"; do
    if [[ -f "$cf" ]]; then
        echo "  Found config: $cf"
        echo "  exeprefix setting:"
        grep -E "^exeprefix" "$cf" 2>/dev/null | sed 's/^/    /' || echo "    Not found"
        break
    fi
done

# Check if /snap/ is in exeprefix
echo ""
if grep -rh "exeprefix" /etc/preheat/ /usr/local/etc/preheat/ 2>/dev/null | grep -q "/snap/"; then
    echo -e "${GREEN}✓ /snap/ is in exeprefix configuration${NC}"
else
    echo -e "${RED}✗ /snap/ may not be in exeprefix${NC}"
    echo "  Add '/snap/' to exeprefix_raw in preheat.conf"
fi

echo ""

#######################################
# Phase 7: Manual Verification Commands
#######################################
echo -e "${BLUE}[Phase 7] Manual Verification Commands${NC}"
echo "$SUBDIV"

echo "Run these commands manually for deeper investigation:"
echo ""
echo "  # Check if daemon can read maps (run as root):"
echo "  sudo cat /proc/\$(pgrep -n firefox)/maps | head"
echo ""
echo "  # Check snap confinement:"
echo "  snap info firefox | grep confinement"
echo ""
echo "  # Check AppArmor logs for maps access:"
echo "  sudo journalctl | grep -E 'apparmor.*maps|DENIED.*firefox'"
echo ""
echo "  # Force daemon rescan:"
echo "  sudo systemctl reload preheat"
echo ""
echo "  # Watch daemon logs in realtime:"
echo "  sudo journalctl -fu preheat"
echo ""

#######################################
# Phase 8: Summary
#######################################
echo -e "${BLUE}${DIV}${NC}"
echo -e "${BLUE}                    SUMMARY${NC}"
echo -e "${BLUE}${DIV}${NC}"

echo ""
echo "Based on SNAP_FIREFOX_ROOT_CAUSE_ANALYSIS.md:"
echo ""
echo "The preheat daemon fails to track snap Firefox because:"
echo "  1. AppArmor blocks /proc/PID/exe → WORKAROUND: cmdline fallback (done)"
echo "  2. AppArmor blocks /proc/PID/maps → FATAL: kp_proc_get_maps() returns 0"
echo "  3. spy.c:new_exe_callback() silently exits if size==0"
echo ""
echo "Solution options:"
echo "  - Option 1: Document as known limitation (done)"
echo "  - Option 2: Accept exe without map scanning"  
echo "  - Option 3: Desktop file fallback"
echo ""
echo -e "${CYAN}Script complete.${NC}"
