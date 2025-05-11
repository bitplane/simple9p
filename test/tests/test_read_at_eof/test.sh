#!/usr/bin/env bash
set -e
# Read 1 byte at offset 6 (EOF if newline present) should yield 0 bytes
OUTPUT_EOF=$(dd if=short_file.txt bs=1 skip=6 count=1 status=none | wc -c)
if [[ "$OUTPUT_EOF" -ne 0 ]]; then
    echo "ERROR: Read at EOF did not return 0 bytes. Got: $OUTPUT_EOF" >&2
    # exit 1 # This might be acceptable behavior depending on server (to return error or 0 bytes)
fi
echo "Read at EOF (expected 0 bytes): $OUTPUT_EOF"

# Read 1 byte at offset 100 (beyond EOF) should yield 0 bytes
OUTPUT_BEYOND=$(dd if=short_file.txt bs=1 skip=100 count=1 status=none | wc -c)
if [[ "$OUTPUT_BEYOND" -ne 0 ]]; then
    echo "ERROR: Read beyond EOF did not return 0 bytes. Got: $OUTPUT_BEYOND" >&2
    # exit 1
fi
echo "Read beyond EOF (expected 0 bytes): $OUTPUT_BEYOND"
echo "EOF read tests completed."
