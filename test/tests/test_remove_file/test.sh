#!/usr/bin/env bash
set -e
if [[ ! -f "delete_me.txt" ]]; then
  echo "ERROR: Pre-condition failed, delete_me.txt does not exist." >&2
  exit 1
fi
rm delete_me.txt
if [[ -f "delete_me.txt" ]]; then
  echo "ERROR: File 'delete_me.txt' still exists after rm." >&2
  exit 1
fi
echo "File 'delete_me.txt' removed successfully."
ls -a
