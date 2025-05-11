#!/usr/bin/env bash
set -e
# Generate a filename of 255 'a's + .txt
F_NAME_250=$(printf 'a%.0s' {1..250})
LONG_FILENAME="${F_NAME_250}_long.txt" # 250 + 9 = 259. Too long for some underlying FS if served directly.
                                     # Let's try 250 total
FILENAME_250_CHARS=$(printf 'X%.0s' {1..246}).txt # 246 + 4 = 250

echo "Creating file with 250 char name: $FILENAME_250_CHARS"
echo "content for 250 char filename" > "$FILENAME_250_CHARS"
if [[ ! -f "$FILENAME_250_CHARS" ]]; then
  echo "ERROR: File with 250 char name not created." >&2
  exit 1
fi
ls -l "$FILENAME_250_CHARS"
cat "$FILENAME_250_CHARS"
rm "$FILENAME_250_CHARS"

# Attempt one that is likely too long for many systems (e.g. 300 chars)
# This might fail at 'echo >' or 'mkdir' depending on FS and FUSE handling
VERY_LONG_FILENAME=$(printf 'b%.0s' {1..296}).txt # 300 chars
echo "Attempting to create file with 300 char name (might fail): $VERY_LONG_FILENAME"
echo "content for very long filename" > "$VERY_LONG_FILENAME" || echo "Creation of 300 char filename failed as expected or partially succeeded."

# Check if it exists, behavior depends on server/fuse/kernel
if [[ -f "$VERY_LONG_FILENAME" ]]; then
  echo "File with 300 char name WAS created."
  ls -l "$VERY_LONG_FILENAME"
  rm "$VERY_LONG_FILENAME"
else
  echo "File with 300 char name was NOT created (as might be expected)."
fi
ls -A # Show directory contents
echo "Max filename length tests complete."
