#!/usr/bin/env bash
set -e
FILENAME="newly_created_file.txt"
CONTENT="Content of the new file."
echo "$CONTENT" > "$FILENAME"
if [[ ! -f "$FILENAME" ]]; then
  echo "ERROR: File '$FILENAME' not created." >&2
  exit 1
fi
if [[ "$(cat "$FILENAME")" != "$CONTENT" ]]; then
  echo "ERROR: File content mismatch for '$FILENAME'." >&2
  exit 1
fi
echo "File '$FILENAME' created and content verified."
ls -l "$FILENAME"
