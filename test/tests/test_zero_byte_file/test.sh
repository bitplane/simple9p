#!/usr/bin/env bash
set -e
touch zero_byte_file.txt
ls -l zero_byte_file.txt
FILE_SIZE=$(wc -c < zero_byte_file.txt)
if [[ "$FILE_SIZE" -ne 0 ]]; then
    echo "ERROR: Expected zero byte file, got size $FILE_SIZE" >&2
    exit 1
fi
# Try to cat it (should produce no output)
cat zero_byte_file.txt
echo "Zero byte file test OK."
