#!/bin/bash
# Quick Smoke Test for Preheat
# Tests basic daemon functionality

set -e

# Test directory
TEST_DIR="/tmp/preheat-test-$$"
mkdir -p "$TEST_DIR"/{etc,var/lib/preheat,var/log}

echo "==================================="
echo "Preheat Smoke Test"
echo "==================================="
echo ""

# Setup config
cp config/preheat.conf.default "$TEST_DIR/etc/preheat.conf"
echo "[model]" >> "$TEST_DIR/etc/preheat.conf"
echo "cycle = 6" >> "$TEST_DIR/etc/preheat.conf"

# Test 1: Start daemon
echo "TEST 1: Starting daemon..."
./src/preheat \
    -c "$TEST_DIR/etc/preheat.conf" \
    -s "$TEST_DIR/var/lib/preheat/state.bin" \
    -l "$TEST_DIR/var/log/preheat.log" &

sleep 2

# Check if running
if pgrep -f "preheat.*$TEST_DIR" > /dev/null; then
    echo "✓ Daemon is running"
    PID=$(pgrep -f "preheat.*$TEST_DIR")
    echo "  PID: $PID"
else
    echo "✗ Daemon failed to start"
    exit 1
fi

# Test 2: Check log file
echo ""
echo "TEST 2: Checking logs..."
sleep 2
if [ -f "$TEST_DIR/var/log/preheat.log" ]; then
    if grep -q "started" "$TEST_DIR/var/log/preheat.log"; then
        echo "✓ Daemon logged startup"
    else
        echo "✗ Startup not logged"
    fi
else
    echo "✗ No log file found"
fi

# Test 3: Wait for scanning
echo ""
echo "TEST 3: Waiting for periodic tasks (15 seconds)..."
sleep 15

if grep -q "scanning" "$TEST_DIR/var/log/preheat.log"; then
    echo "✓ Periodic scanning activated"
else
    echo "⚠ No scanning yet (may need more time)"
fi

if grep -q "predicting" "$TEST_DIR/var/log/preheat.log"; then
    echo "✓ Prediction engine activated"
else
    echo "⚠ No prediction yet (may need more time)"
fi

# Test 4: Signal handling
echo ""
echo "TEST 4: Testing signals..."

# SIGUSR1 - dump state
kill -USR1 $PID
sleep 1
if grep -q "state log dump" "$TEST_DIR/var/log/preheat.log"; then
    echo "✓ SIGUSR1 (dump) works"
else
    echo "✗ SIGUSR1 failed"
fi

# SIGUSR2 - save state
kill -USR2 $PID
sleep 1
if grep -q "saving state" "$TEST_DIR/var/log/preheat.log"; then
    echo "✓ SIGUSR2 (save) works"
else
    echo "✗ SIGUSR2 failed"
fi

# Test 5: State file
echo ""
echo "TEST 5: Checking state file..."
if [ -f "$TEST_DIR/var/lib/preheat/state.bin" ]; then
    echo "✓ State file created"
    if head -1 "$TEST_DIR/var/lib/preheat/state.bin" | grep -q "PRELOAD"; then
        echo "✓ State file has valid format"
    else
        echo "✗ State file format invalid"
    fi
else
    echo "⚠ State file not yet created (autosave pending)"
fi

# Test 6: Graceful shutdown
echo ""
echo "TEST 6: Testing graceful shutdown..."
kill -TERM $PID
sleep 2

if ! pgrep -f "preheat.*$TEST_DIR" > /dev/null; then
    echo "✓ Daemon stopped gracefully"
else
    echo "✗ Daemon still running"
    kill -9 $PID 2>/dev/null || true
fi

# Final summary
echo ""
echo "==================================="
echo "Summary"
echo "==================================="
echo "Log file: $TEST_DIR/var/log/preheat.log"
echo "State file: $TEST_DIR/var/lib/preheat/state.bin"
echo ""
echo "Last 10 log lines:"
tail -10 "$TEST_DIR/var/log/preheat.log"  2>/dev/null || echo "(no log)"
echo ""
echo "✓ Smoke test complete!"
echo "Test artifacts in: $TEST_DIR"
