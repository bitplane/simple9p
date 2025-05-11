#!/usr/bin/env bash
set -e
echo "Running no_mount test.sh in $(pwd)"
if [[ ! -f "local_file.txt" ]]; then
  echo "ERROR: local_file.txt not found in $(pwd)" >&2
  exit 1
fi
cat local_file.txt
echo "Content from local_file.txt printed."
# Create a new file to see if it's reflected in actual/expected gathering
echo "New file in test.sh" > new_file_in_test.txt
ls -la
