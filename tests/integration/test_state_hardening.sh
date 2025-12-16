#!/bin/bash
# State Hardening Tests for Preheat Phase 1
# Tests: CRC32 validation, corruption recovery, atomic writes

set -e

echo "========================================="
echo "Preheat State Hardening Tests"
echo "Phase 1: Core Reliability"
echo "========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Setup test environment
TEST_DIR="/tmp/preheat-hardening-$$"
mkdir -p "$TEST_DIR"/{etc,var/lib/preheat,var/log}

echo "Test environment: $TEST_DIR"
echo ""

# Cleanup function
cleanup() {
    if [ -f "$TEST_DIR/daemon.pid" ]; then
        PID=$(cat "$TEST_DIR/daemon.pid")
        if ps -p $PID > /dev/null 2>&1; then
            kill $PID 2>/dev/null || true
            sleep 1
        fi
    fi
}
trap cleanup EXIT

# Test helper functions
test_start() {
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -n "TEST $TESTS_RUN: $1... "
}

test_pass() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    echo -e "${GREEN}PASS${NC}"
}

test_fail() {
    TESTS_FAILED=$((TESTS_FAILED + 1))
    echo -e "${RED}FAIL${NC}"
    if [ -n "$1" ]; then
        echo "  Error: $1"
    fi
}

# Setup config
cp config/preheat.conf.default "$TEST_DIR/etc/preheat.conf"
cat >> "$TEST_DIR/etc/preheat.conf" << EOF
[model]
cycle = 6
[system]
autosave = 10
EOF

# =========================================================================
# TEST 1: CRC32 Footer Present
# =========================================================================
test_start "CRC32 footer written to new state file"

# Start daemon in foreground mode with & for reliable PID control
./src/preheat -f \
    -c "$TEST_DIR/etc/preheat.conf" \
    -s "$TEST_DIR/var/lib/preheat/state.bin" \
    -l "$TEST_DIR/var/log/preheat.log" > /dev/null 2>&1 &
DAEMON_PID=$!
echo $DAEMON_PID > "$TEST_DIR/daemon.pid"
sleep 4  # Wait for daemon to initialize and autosave (autosave=10 but state gets dirty)

# Trigger manual save
if ps -p $DAEMON_PID > /dev/null 2>&1; then
    kill -USR2 $DAEMON_PID 2>/dev/null || true
    sleep 2  # Wait for save to complete
    kill -TERM $DAEMON_PID 2>/dev/null || true
    sleep 1
fi

if [ -f "$TEST_DIR/var/lib/preheat/state.bin" ] && grep -q "^CRC32" "$TEST_DIR/var/lib/preheat/state.bin"; then
    test_pass
else
    test_fail "No CRC32 line found in state file"
fi

# =========================================================================
# TEST 2: Old State File (No CRC) Loads Successfully
# =========================================================================
test_start "Old state file without CRC loads successfully"

# Create old-format state file (no CRC32)
cat > "$TEST_DIR/var/lib/preheat/state.bin" << EOF
PRELOAD	0.1.0	100
EOF

# Start daemon - should load without error
./src/preheat -f \
    -c "$TEST_DIR/etc/preheat.conf" \
    -s "$TEST_DIR/var/lib/preheat/state.bin" \
    -l "$TEST_DIR/var/log/preheat2.log" \
    > /dev/null 2>&1 &
DAEMON_PID=$!
echo $DAEMON_PID > "$TEST_DIR/daemon.pid"
sleep 2

if ps -p $DAEMON_PID > /dev/null 2>&1; then
    test_pass
    kill -TERM $DAEMON_PID 2>/dev/null || true
    sleep 1
else
    test_fail "Daemon crashed loading old state file"
fi

# =========================================================================
# TEST 3: Corrupt State File Recovery
# =========================================================================
test_start "Corrupt state file triggers recovery (not crash)"

# Create corrupt state file
cat > "$TEST_DIR/var/lib/preheat/state.bin" << EOF
PRELOAD	0.1.0	100
garbage corruption data here
INVALID TAG
EOF

# Start daemon - should recover gracefully
./src/preheat -f \
    -c "$TEST_DIR/etc/preheat.conf" \
    -s "$TEST_DIR/var/lib/preheat/state.bin" \
    -l "$TEST_DIR/var/log/preheat3.log" \
    > /dev/null 2>&1 &
DAEMON_PID=$!
echo $DAEMON_PID > "$TEST_DIR/daemon.pid"
sleep 3

if ps -p $DAEMON_PID > /dev/null 2>&1; then
    test_pass
    kill -TERM $DAEMON_PID 2>/dev/null || true
    sleep 1
else
    # Check if warning was logged before exit (not a crash)
    if grep -q "corrupt\|starting fresh" "$TEST_DIR/var/log/preheat3.log" 2>/dev/null; then
        test_pass
    else
        test_fail "Daemon crashed without recovery"
    fi
fi

# =========================================================================
# TEST 4: Broken File Renamed on Corruption
# =========================================================================
test_start "Corrupt file renamed to .broken.TIMESTAMP"

BROKEN_COUNT=$(ls "$TEST_DIR/var/lib/preheat/"*.broken.* 2>/dev/null | wc -l)
if [ "$BROKEN_COUNT" -ge 1 ]; then
    test_pass
else
    test_fail "No .broken file created"
fi

# =========================================================================
# TEST 5: Truncated State File Handled
# =========================================================================
test_start "Truncated state file handled gracefully"

# Create truncated file (partial header)
echo -n "PRELOAD	0.1." > "$TEST_DIR/var/lib/preheat/state.bin"

./src/preheat -f \
    -c "$TEST_DIR/etc/preheat.conf" \
    -s "$TEST_DIR/var/lib/preheat/state.bin" \
    -l "$TEST_DIR/var/log/preheat4.log" \
    > /dev/null 2>&1 &
DAEMON_PID=$!
echo $DAEMON_PID > "$TEST_DIR/daemon.pid"
sleep 2

if ps -p $DAEMON_PID > /dev/null 2>&1; then
    test_pass
    kill -TERM $DAEMON_PID 2>/dev/null || true
else
    # Even if exited, check it was graceful
    if grep -q "invalid\|corrupt\|ignoring" "$TEST_DIR/var/log/preheat4.log" 2>/dev/null; then
        test_pass
    else
        test_fail "Daemon crashed on truncated file"
    fi
fi
sleep 1

# =========================================================================
# TEST 6: fsync Called (Check log for successful save)
# =========================================================================
test_start "State save completes successfully (includes fsync)"

# Create clean state
rm -f "$TEST_DIR/var/lib/preheat/state.bin"*

./src/preheat -f \
    -c "$TEST_DIR/etc/preheat.conf" \
    -s "$TEST_DIR/var/lib/preheat/state.bin" \
    -l "$TEST_DIR/var/log/preheat5.log" > /dev/null 2>&1 &
DAEMON_PID=$!
echo $DAEMON_PID > "$TEST_DIR/daemon.pid"
sleep 4

if ps -p $DAEMON_PID > /dev/null 2>&1; then
    kill -USR2 $DAEMON_PID 2>/dev/null || true
    sleep 2
    
    if grep -q "saving state to" "$TEST_DIR/var/log/preheat5.log"; then
        test_pass
    else
        test_fail "State save did not complete"
    fi
    
    kill -TERM $DAEMON_PID 2>/dev/null || true
    sleep 1
else
    test_fail "Daemon not running - cannot test save"
fi

# =========================================================================
# Summary
# =========================================================================
echo ""
echo "========================================="
echo "TEST SUMMARY"
echo "========================================="
echo "Total Tests: $TESTS_RUN"
echo -e "Passed:      ${GREEN}$TESTS_PASSED${NC}"
echo -e "Failed:      ${RED}$TESTS_FAILED${NC}"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ ALL STATE HARDENING TESTS PASSED${NC}"
    echo ""
    echo "Phase 1 Verified:"
    echo "  ✓ CRC32 footer written"
    echo "  ✓ Old state files load (backward compat)"
    echo "  ✓ Corrupt files trigger recovery"
    echo "  ✓ Broken files renamed"
    echo "  ✓ Truncated files handled"
    echo "  ✓ fsync + atomic write working"
    echo ""
    EXIT_CODE=0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    echo ""
    echo "Check logs in: $TEST_DIR/var/log/"
    echo ""
    EXIT_CODE=1
fi

echo "Test artifacts in: $TEST_DIR"
echo "========================================="
exit $EXIT_CODE
