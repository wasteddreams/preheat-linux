#!/bin/bash
#
# Snap Firefox Long Test Script
# Tests preheat daemon's ability to track and prioritize snap-installed Firefox
# Runs multiple Firefox sessions and verifies daemon tracking
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

DIV="============================================="
SUBDIV="---------------------------------------------"

echo -e "${CYAN}${DIV}${NC}"
echo -e "${CYAN}  Snap Firefox Long Test for Preheat${NC}"
echo -e "${CYAN}${DIV}${NC}"
echo ""

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}[ERROR] This test must be run as root (sudo)${NC}"
    exit 1
fi

#######################################
# Phase 1: Pre-Test Verification
#######################################
echo -e "${BLUE}[Phase 1] Pre-Test Verification${NC}"
echo "$SUBDIV"

# Check snap firefox
if ! snap list firefox &>/dev/null; then
    echo -e "${RED}✗ Firefox snap NOT installed${NC}"
    exit 1
fi
SNAP_REV=$(snap list firefox 2>/dev/null | tail -n1 | awk '{print $3}')
echo -e "${GREEN}✓ Firefox snap found (rev: $SNAP_REV)${NC}"

# Check daemon running
if ! pgrep -x preheat &>/dev/null; then
    echo -e "${YELLOW}Starting preheat daemon...${NC}"
    systemctl start preheat
    sleep 2
fi
echo -e "${GREEN}✓ Preheat daemon running (PID: $(pgrep -x preheat))${NC}"

# Get initial state
SNAP_PATH="/snap/firefox/${SNAP_REV}/usr/lib/firefox/firefox"
echo ""
echo "Target binary: $SNAP_PATH"

# Check desktop scanner
APPS_COUNT=$(grep "Desktop scanner initialized" /usr/local/var/log/preheat.log 2>/dev/null | tail -1 | grep -oP '\d+ GUI applications')
echo "Desktop scanner: $APPS_COUNT"

# Get initial Firefox state
echo ""
echo -e "${BLUE}Initial Firefox State:${NC}"
if preheat-ctl explain "$SNAP_PATH" 2>&1 | grep -q "NOT TRACKED"; then
    echo -e "${YELLOW}  Firefox is NOT yet tracked${NC}"
    INITIAL_STATE="NOT_TRACKED"
    INITIAL_LAUNCHES=0
else
    INITIAL_POOL=$(preheat-ctl explain "$SNAP_PATH" 2>&1 | grep "Pool:" | awk '{print $2}')
    INITIAL_LAUNCHES=$(preheat-ctl explain "$SNAP_PATH" 2>&1 | grep "Raw Launches:" | awk '{print $3}')
    INITIAL_LAUNCHES=${INITIAL_LAUNCHES:-0}
    echo "  Pool: $INITIAL_POOL"
    echo "  Raw Launches: $INITIAL_LAUNCHES"
    INITIAL_STATE="TRACKED"
fi

echo ""

#######################################
# Phase 2: Launch Firefox Multiple Times
#######################################
echo -e "${BLUE}[Phase 2] Launching Firefox Multiple Times${NC}"
echo "$SUBDIV"

LAUNCH_COUNT=5
WAIT_BETWEEN=10  # seconds between launches
RUNTIME=8        # seconds Firefox stays open each launch

echo "Will launch Firefox $LAUNCH_COUNT times"
echo "Each session: ${RUNTIME}s open, ${WAIT_BETWEEN}s cooldown"
echo ""

for i in $(seq 1 $LAUNCH_COUNT); do
    echo -e "${CYAN}Launch $i of $LAUNCH_COUNT...${NC}"
    
    # Get the logged-in user's display
    REAL_USER=$(who | grep -v root | head -1 | awk '{print $1}')
    DISPLAY_NUM=$(who | grep -v root | head -1 | awk '{print $2}' | grep -oP '\d+' || echo "0")
    
    # Launch Firefox as the logged-in user with proper display
    export DISPLAY=:${DISPLAY_NUM}
    sudo -u ${REAL_USER:-ubuntu} DISPLAY=:${DISPLAY_NUM} firefox --new-window about:blank &
    FIREFOX_PID=$!
    
    sleep 3  # Let Firefox fully start
    
    # Find all Firefox PIDs
    FIREFOX_PIDS=$(pgrep -f "firefox" 2>/dev/null | grep -v $$ | head -5 | tr '\n' ' ')
    echo "  PIDs: $FIREFOX_PIDS"
    
    # Wait for runtime
    sleep $RUNTIME
    
    # Close Firefox gracefully
    killall -q firefox 2>/dev/null || true
    sleep 1
    killall -9 -q firefox 2>/dev/null || true
    
    echo "  Closed Firefox"
    
    # Cooldown
    if [[ $i -lt $LAUNCH_COUNT ]]; then
        echo "  Waiting ${WAIT_BETWEEN}s..."
        sleep $WAIT_BETWEEN
    fi
done

echo ""
echo -e "${GREEN}✓ Completed $LAUNCH_COUNT Firefox launches${NC}"

#######################################
# Phase 3: Wait for Daemon Cycle
#######################################
echo ""
echo -e "${BLUE}[Phase 3] Waiting for Daemon Scan Cycle${NC}"
echo "$SUBDIV"

CYCLE_TIME=95
echo "Waiting ${CYCLE_TIME}s for daemon to process..."
echo ""

# Show progress
for i in $(seq 1 $CYCLE_TIME); do
    printf "\r  [%3d/%d seconds]" $i $CYCLE_TIME
    sleep 1
done
echo ""
echo ""

#######################################
# Phase 4: Verify Results
#######################################
echo -e "${BLUE}[Phase 4] Verification Results${NC}"
echo "$DIV"
echo ""

# Check Firefox in stats
echo -e "${CYAN}Firefox Status:${NC}"
preheat-ctl explain "$SNAP_PATH" 2>&1 | grep -E "Status:|Pool:|Weighted|Raw Launches:|Combined:" | sed 's/^/  /'

echo ""

# Get final values
FINAL_POOL=$(preheat-ctl explain "$SNAP_PATH" 2>&1 | grep "Pool:" | awk '{print $2}')
FINAL_LAUNCHES=$(preheat-ctl explain "$SNAP_PATH" 2>&1 | grep "Raw Launches:" | awk '{print $3}')
FINAL_WEIGHTED=$(preheat-ctl explain "$SNAP_PATH" 2>&1 | grep "Weighted Launches:" | awk '{print $2}')

# Check in top apps
echo -e "${CYAN}Top Apps Check:${NC}"
preheat-ctl stats 2>&1 | grep -i firefox || echo "  Firefox not yet in top apps list"

echo ""

#######################################
# Phase 5: Test Summary
#######################################
echo -e "${BLUE}${DIV}${NC}"
echo -e "${BLUE}              TEST SUMMARY${NC}"
echo -e "${BLUE}${DIV}${NC}"
echo ""

# Pool check
echo -n "Pool Classification: "
if [[ "$FINAL_POOL" == "priority" ]]; then
    echo -e "${GREEN}✓ PASS (priority pool)${NC}"
    POOL_PASS=true
else
    echo -e "${RED}✗ FAIL (expected: priority, got: $FINAL_POOL)${NC}"
    POOL_PASS=false
fi

# Launch count check - verify delta
echo -n "Launch Tracking: "
FINAL_LAUNCHES=${FINAL_LAUNCHES:-0}
LAUNCH_DELTA=$((FINAL_LAUNCHES - INITIAL_LAUNCHES))
if [[ "$LAUNCH_DELTA" -ge "$LAUNCH_COUNT" ]]; then
    echo -e "${GREEN}✓ PASS (delta: +$LAUNCH_DELTA launches, $INITIAL_LAUNCHES → $FINAL_LAUNCHES)${NC}"
    LAUNCH_PASS=true
elif [[ "$LAUNCH_DELTA" -gt 0 ]]; then
    echo -e "${YELLOW}⚠ PARTIAL ($LAUNCH_DELTA/$LAUNCH_COUNT launches detected, $INITIAL_LAUNCHES → $FINAL_LAUNCHES)${NC}"
    LAUNCH_PASS=true
else
    echo -e "${RED}✗ FAIL (no increment: $INITIAL_LAUNCHES → $FINAL_LAUNCHES)${NC}"
    LAUNCH_PASS=false
fi

# Desktop file match
echo -n "Desktop File Match: "
if preheat-ctl explain "$SNAP_PATH" 2>&1 | grep -q "\.desktop"; then
    echo -e "${GREEN}✓ PASS (matched .desktop file)${NC}"
    DESKTOP_PASS=true
else
    echo -e "${YELLOW}⚠ Could not verify .desktop match${NC}"
    DESKTOP_PASS=false
fi

echo ""

# Overall result
if [[ "$POOL_PASS" == "true" && "$LAUNCH_PASS" == "true" ]]; then
    echo -e "${GREEN}${DIV}${NC}"
    echo -e "${GREEN}          ALL TESTS PASSED!${NC}"
    echo -e "${GREEN}${DIV}${NC}"
    echo ""
    echo "Snap Firefox is correctly tracked by preheat daemon."
    echo "  Pool: $FINAL_POOL"
    echo "  Raw Launches: $INITIAL_LAUNCHES → $FINAL_LAUNCHES (+$LAUNCH_DELTA)"
    echo "  Weighted: $FINAL_WEIGHTED"
    exit 0
else
    echo -e "${RED}${DIV}${NC}"
    echo -e "${RED}          SOME TESTS FAILED${NC}"
    echo -e "${RED}${DIV}${NC}"
    echo ""
    echo "Check logs: /usr/local/var/log/preheat.log"
    exit 1
fi
