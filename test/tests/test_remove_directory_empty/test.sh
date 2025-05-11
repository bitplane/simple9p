#!/usr/bin/env bash
set -e
rmdir empty_dir_to_rm
if [[ -d "empty_dir_to_rm" ]]; then
  echo "ERROR: Directory 'empty_dir_to_rm' still exists after rmdir." >&2
  exit 1
fi
echo "Directory 'empty_dir_to_rm' removed successfully."
ls -a
