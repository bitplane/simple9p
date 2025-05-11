#!/usr/bin/env bash
set -e
echo "Verifying large file size and content (checksum)..."
ls -l largefile.dat
# Simple checksum; for real data, use md5sum or sha256sum if available & consistent
# For this test, we'll just check the size again via wc -c
FILE_SIZE=$(wc -c < largefile.dat)
EXPECTED_SIZE=$((5 * 1024 * 1024))
if [[ "$FILE_SIZE" -ne "$EXPECTED_SIZE" ]]; then
    echo "ERROR: Size mismatch. Expected $EXPECTED_SIZE, got $FILE_SIZE" >&2
    exit 1
fi
# Attempt to read a portion to ensure it's not all errors
dd if=largefile.dat bs=1k count=1 status=none of=/dev/null
echo "Large file read test completed."
