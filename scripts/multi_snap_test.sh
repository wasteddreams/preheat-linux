#!/bin/bash
#
# Multi-App Snap Test Script
# Tests preheat daemon's ability to track and count launches for multiple snap apps
#

# Don't exit on errors - we handle them gracefully
# set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

DIV="============================================="
SUBDIV="---------------------------------------------"

echo -e "${CYAN}${DIV}${NC}"
echo -e "${CYAN}  Multi-App Snap Test for Preheat${NC}"
echo -e "${CYAN}${DIV}${NC}"
echo ""

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}[ERROR] This test must be run as root (sudo)${NC}"
    exit 1
fi

# Get real user for launching GUI apps
REAL_USER=$(who | grep -v root | head -1 | awk '{print $1}')
DISPLAY_NUM=$(who | grep -v root | head -1 | awk '{print $2}' | grep -oP '\d+' || echo "0")
export DISPLAY=:${DISPLAY_NUM}

if [[ -z "$REAL_USER" ]]; then
    echo -e "${RED}[ERROR] Could not determine logged-in user${NC}"
    exit 1
fi

echo "Running as user: $REAL_USER on DISPLAY=:$DISPLAY_NUM"
echo ""

#######################################
# Configuration
# Note: Apps should run longer than daemon cycle (90s) for reliable detection.
# These are reasonable defaults for testing.
#######################################
LAUNCHES_PER_APP=3          # Number of launches per app
WAIT_AFTER_LAUNCH=20        # Seconds to keep app open
WAIT_BETWEEN_APPS=15        # Seconds between different apps  
WAIT_FOR_DAEMON=100         # Seconds to wait for daemon scan after all launches

# Declare associative arrays
declare -A APP_PATHS
declare -A APP_INITIAL
declare -A APP_FINAL

#######################################
# Phase 1: Discover Snap Apps
#######################################
echo -e "${BLUE}[Phase 1] Discovering Snap Apps${NC}"
echo "$SUBDIV"

# Restart daemon to pick up any new snap revisions
echo -e "${YELLOW}Restarting preheat daemon (important after snap reinstalls)...${NC}"
systemctl restart preheat
sleep 5
echo -e "${GREEN}✓ Preheat daemon running (PID: $(pgrep -x preheat))${NC}"

# Find installed snap GUI apps
SNAP_APPS=()

check_snap_app() {
    local name=$1
    local snap_rev
    local binary_path
    
    if snap list "$name" &>/dev/null; then
        snap_rev=$(snap list "$name" 2>/dev/null | tail -n1 | awk '{print $3}')
        
        # Try common binary locations
        for pattern in \
            "/snap/$name/$snap_rev/usr/lib/$name/$name" \
            "/snap/$name/$snap_rev/usr/bin/$name" \
            "/snap/$name/$snap_rev/bin/$name" \
            "/snap/$name/$snap_rev/$name"; do
            if [[ -x "$pattern" ]]; then
                APP_PATHS[$name]="$pattern"
                SNAP_APPS+=("$name")
                echo -e "${GREEN}  ✓ $name (rev $snap_rev)${NC}"
                return 0
            fi
        done
        
        # Check using realpath on wrapper
        if [[ -x "/snap/bin/$name" ]]; then
            APP_PATHS[$name]="/snap/bin/$name"
            SNAP_APPS+=("$name")
            echo -e "${GREEN}  ✓ $name (wrapper)${NC}"
            return 0
        fi
    fi
    return 1
}

echo "Checking for common snap apps..."

# Check common snap GUI apps
for app in firefox chromium vlc gimp libreoffice spotify slack discord telegram-desktop thunderbird code gnome-calculator gnome-system-monitor; do
    check_snap_app "$app" 2>/dev/null || true
done

if [[ ${#SNAP_APPS[@]} -eq 0 ]]; then
    echo -e "${RED}No snap GUI apps found!${NC}"
    echo "Install some with: snap install firefox vlc gimp"
    exit 1
fi

echo ""
echo "Found ${#SNAP_APPS[@]} snap apps to test: ${SNAP_APPS[*]}"
echo ""

#######################################
# Phase 2: Record Initial State
#######################################
echo -e "${BLUE}[Phase 2] Recording Initial State${NC}"
echo "$SUBDIV"

for app in "${SNAP_APPS[@]}"; do
    path="${APP_PATHS[$app]}"
    # Try to get initial launch count
    initial=$(preheat-ctl explain "$path" 2>&1 | grep "Raw Launches:" | awk '{print $3}' || echo "0")
    initial=${initial:-0}
    APP_INITIAL[$app]=$initial
    echo "  $app: $initial launches"
done

echo ""

#######################################
# Phase 3: Launch Apps Multiple Times
#######################################
echo -e "${BLUE}[Phase 3] Launching Apps${NC}"
echo "$SUBDIV"
echo ""
echo "Config: $LAUNCHES_PER_APP launches per app, ${WAIT_AFTER_LAUNCH}s open, ${WAIT_BETWEEN_APPS}s cooldown"
echo ""

for app in "${SNAP_APPS[@]}"; do
    echo -e "${CYAN}Testing: $app${NC}"
    
    for i in $(seq 1 $LAUNCHES_PER_APP); do
        echo -n "  Launch $i/$LAUNCHES_PER_APP... "
        
        # THOROUGHLY kill any existing instances first
        # Kill by process name patterns (covers all snap helper processes)
        pkill -9 -f "snap.*$app" 2>/dev/null || true
        pkill -9 -f "/snap/$app" 2>/dev/null || true
        pkill -9 -f "${app}_${app}" 2>/dev/null || true  # snap uses appname_appname pattern
        killall -9 -q "$app" 2>/dev/null || true
        killall -9 -q "${app}.real" 2>/dev/null || true
        
        # Wait for processes to fully terminate
        sleep 5
        
        # Launch the app
        case "$app" in
            firefox)
                sudo -u "$REAL_USER" DISPLAY=:${DISPLAY_NUM} /snap/bin/firefox --new-instance --no-remote about:blank &>/dev/null &
                ;;
            thunderbird)
                sudo -u "$REAL_USER" DISPLAY=:${DISPLAY_NUM} /snap/bin/thunderbird --new-instance &>/dev/null &
                ;;
            *)
                sudo -u "$REAL_USER" DISPLAY=:${DISPLAY_NUM} /snap/bin/$app &>/dev/null &
                ;;
        esac
        
        # Wait for app to fully start and run
        sleep $WAIT_AFTER_LAUNCH
        
        # GRACEFUL shutdown first with SIGTERM
        pkill -15 -f "snap.*$app" 2>/dev/null || true
        pkill -15 -f "/snap/$app" 2>/dev/null || true
        killall -15 -q "$app" 2>/dev/null || true
        
        # Wait for graceful shutdown
        sleep 5
        
        # Force kill anything remaining with SIGKILL
        pkill -9 -f "snap.*$app" 2>/dev/null || true
        pkill -9 -f "/snap/$app" 2>/dev/null || true
        pkill -9 -f "${app}_${app}" 2>/dev/null || true
        killall -9 -q "$app" 2>/dev/null || true
        
        # Extra wait for cleanup
        sleep 5
        
        echo "done"
        
        if [[ $i -lt $LAUNCHES_PER_APP ]]; then
            echo "    Waiting 15s before next launch..."
            sleep 15
        fi
    done
    
    echo ""
    
    # Wait between different apps
    if [[ "$app" != "${SNAP_APPS[-1]}" ]]; then
        echo "  Waiting ${WAIT_BETWEEN_APPS}s before next app..."
        sleep $WAIT_BETWEEN_APPS
        echo ""
    fi
done

#######################################
# Phase 4: Wait for Daemon
#######################################
echo -e "${BLUE}[Phase 4] Waiting for Daemon Scan${NC}"
echo "$SUBDIV"
echo "Waiting ${WAIT_FOR_DAEMON}s for daemon to process..."
echo ""

for i in $(seq 1 $WAIT_FOR_DAEMON); do
    printf "\r  [%3d/%d seconds]" $i $WAIT_FOR_DAEMON
    sleep 1
done
echo ""
echo ""

#######################################
# Phase 5: Collect Final State & Results
#######################################
echo -e "${BLUE}[Phase 5] Results${NC}"
echo "$DIV"
echo ""

TOTAL_PASS=0
TOTAL_PARTIAL=0
TOTAL_FAIL=0

printf "%-20s %10s %10s %10s %s\n" "APP" "INITIAL" "FINAL" "DELTA" "STATUS"
echo "$DIV"

for app in "${SNAP_APPS[@]}"; do
    path="${APP_PATHS[$app]}"
    initial="${APP_INITIAL[$app]:-0}"
    
    # Get final count (script runs as root, so no sudo needed)
    final=$(preheat-ctl explain "$path" 2>&1 | grep "Raw Launches:" | awk '{print $3}' 2>/dev/null || echo "0")
    final=${final:-0}
    
    # Handle non-numeric values
    if ! [[ "$final" =~ ^[0-9]+$ ]]; then
        final=0
    fi
    if ! [[ "$initial" =~ ^[0-9]+$ ]]; then
        initial=0
    fi
    
    APP_FINAL[$app]=$final
    
    delta=$((final - initial))
    
    # Determine status
    if [[ $delta -ge $LAUNCHES_PER_APP ]]; then
        status="${GREEN}✓ PASS${NC}"
        ((TOTAL_PASS++))
    elif [[ $delta -gt 0 ]]; then
        status="${YELLOW}⚠ PARTIAL (+$delta/$LAUNCHES_PER_APP)${NC}"
        ((TOTAL_PARTIAL++))
    else
        status="${RED}✗ FAIL${NC}"
        ((TOTAL_FAIL++))
    fi
    
    printf "%-20s %10s %10s %10s " "$app" "$initial" "$final" "+$delta"
    echo -e "$status"
done

echo "$DIV"
echo ""

#######################################
# Summary
#######################################
echo -e "${BLUE}${DIV}${NC}"
echo -e "${BLUE}              SUMMARY${NC}"
echo -e "${BLUE}${DIV}${NC}"
echo ""
echo "  Apps Tested:  ${#SNAP_APPS[@]}"
echo -e "  ${GREEN}Full Pass:    $TOTAL_PASS${NC}"
echo -e "  ${YELLOW}Partial:      $TOTAL_PARTIAL${NC}"
echo -e "  ${RED}Failed:       $TOTAL_FAIL${NC}"
echo ""

if [[ $TOTAL_FAIL -eq 0 ]]; then
    echo -e "${GREEN}${DIV}${NC}"
    echo -e "${GREEN}     LAUNCH COUNTING FIX VERIFIED!${NC}"
    echo -e "${GREEN}${DIV}${NC}"
    exit 0
else
    echo -e "${RED}${DIV}${NC}"
    echo -e "${RED}     SOME APPS FAILED${NC}"
    echo -e "${RED}${DIV}${NC}"
    echo ""
    echo "Check logs: /usr/local/var/log/preheat.log"
    exit 1
fi
