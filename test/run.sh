#!/usr/bin/env bash

# --- Configuration ---
# Binaries are expected to be in the same directory as this script,
# or you can set absolute paths.
export SIMPLE9P_BINARY="$(pwd)/../build/simple9p"
export NINEP_FUSE_BINARY="$(pwd)/9pfuse" # Make sure this path is correct
export HARNESS_RESULTS_ROOT_DIR="$(pwd)/results"

# Optional: Set to 1 for detailed harness logs
export HARNESS_DEBUG=0 
# Optional: Set the base TCP port for the first test. Subsequent tests will increment this.
export HARNESS_PORT_BASE=56400 
# --- End Configuration ---

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HARNESS_LIB="$SCRIPT_DIR/test_harness.sh" 

if [[ ! -f "$HARNESS_LIB" ]]; then
    echo "ERROR: Test harness library '$HARNESS_LIB' not found." >&2
    exit 1
fi
# shellcheck source=./test_harness.sh
source "$HARNESS_LIB"

# --- Script Entry Point ---
cd "$SCRIPT_DIR/tests" || { # Changed from simple9p_test_cases to tests as per your diff
    echo "ERROR: Test cases directory '$SCRIPT_DIR/tests' not found." >&2
    # echo "Did you run a script to generate test cases (e.g., generate_simple9p_tests.sh) first?" >&2
    exit 1; 
}

echo "Running tests from: $(pwd)"
echo "Using simple9p: $SIMPLE9P_BINARY (from $(realpath "$SCRIPT_DIR/$SIMPLE9P_BINARY" 2>/dev/null || echo "$SIMPLE9P_BINARY"))"
echo "Using 9pfuse: $NINEP_FUSE_BINARY (from $(realpath "$SCRIPT_DIR/$NINEP_FUSE_BINARY" 2>/dev/null || echo "$NINEP_FUSE_BINARY"))"
echo "Base TCP Port for tests: $HARNESS_PORT_BASE"


if test_run_all; then
    echo "All tests completed successfully!"
    exit 0
else
    echo "One or more tests failed."
    exit 1
fi
