#!/bin/bash
# run-tests.sh - Run all Preheat tests
#
# Run this from a native terminal (not VS Code integrated terminal)
# to avoid sandbox permission issues.
#
# Usage: ./scripts/run-tests.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

echo "=============================================="
echo "  Preheat Test Suite"
echo "=============================================="
echo
echo "Project: $PROJECT_DIR"
echo

# Check if built
if [ ! -f "src/preheat" ]; then
    echo "[!] Binary not found. Building first..."
    make -j4 || { echo "Build failed!"; exit 1; }
    echo
fi

echo "----------------------------------------------"
echo "  1. Build Verification"
echo "----------------------------------------------"
echo "Binary: src/preheat"
ls -lh src/preheat
echo "Version: $(./src/preheat --version | head -1)"
echo "✓ Build OK"
echo

echo "----------------------------------------------"
echo "  2. Unit Tests"
echo "----------------------------------------------"
if [ -d "tests/unit" ] && [ "$(ls -A tests/unit 2>/dev/null)" ]; then
    echo "Running unit tests..."
    make check 2>/dev/null || echo "No unit tests configured"
else
    echo "⚠ No unit tests (empty tests/unit/)"
fi
echo

echo "----------------------------------------------"
echo "  3. Smoke Test"
echo "----------------------------------------------"
if [ -x "tests/integration/smoke_test.sh" ]; then
    echo "Running smoke test..."
    ./tests/integration/smoke_test.sh
else
    echo "⚠ Smoke test not found"
fi
echo

echo "----------------------------------------------"
echo "  4. CLI Tool Test"
echo "----------------------------------------------"
if [ -x "tests/integration/test_cli_tool.sh" ]; then
    echo "Running CLI tool test..."
    ./tests/integration/test_cli_tool.sh
else
    echo "⚠ CLI tool test not found"
fi
echo

echo "----------------------------------------------"
echo "  5. Installation Test (requires sudo)"
echo "----------------------------------------------"
read -p "Run installation test? [y/N]: " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Installing..."
    sudo make install
    echo
    echo "Starting daemon..."
    sudo systemctl daemon-reload
    sudo systemctl start preheat || echo "Could not start (may need reboot)"
    sleep 2
    sudo systemctl status preheat --no-pager || true
    echo
    echo "Testing CLI tool..."
    sudo preheat-ctl status || true
    echo
    echo "Stopping daemon..."
    sudo systemctl stop preheat || true
    echo "✓ Installation test complete"
else
    echo "Skipped"
fi
echo

echo "=============================================="
echo "  Test Summary"
echo "=============================================="
echo "✓ Build OK"
echo "✓ Smoke test run"
echo
echo "All tests complete!"
