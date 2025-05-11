#!/usr/bin/env bash
set -e
BASE="a/b/c/d/e/f/g"
mkdir -p "$BASE/h/i"
echo "Deep content" > "$BASE/h/i/deep_file.txt"
ls -R a
CONTENT=$(cat "$BASE/h/i/deep_file.txt")
if [[ "$CONTENT" != "Deep content" ]]; then
    echo "ERROR: Content mismatch in deep file." >&2
    exit 1
fi
echo "Deeply nested directory test OK."
rm -rf a # Clean up
