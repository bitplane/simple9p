#!/usr/bin/env bash
# We expect rmdir to fail, so don't use set -e here for this command
rmdir nonempty_dir_to_rm
# $? should be non-zero
if [[ $? -eq 0 ]]; then
  echo "ERROR: rmdir succeeded on a non-empty directory." >&2
  exit 1
fi
if [[ ! -d "nonempty_dir_to_rm" ]]; then
  echo "ERROR: Directory 'nonempty_dir_to_rm' was removed, but it shouldn't have been by rmdir." >&2
  exit 1
fi
echo "rmdir correctly failed on non-empty directory."
ls -la # Show it's still there
# Now clean it up if recursive rm is supported (test for simple9p might not support -rf)
# For now, just verify it's still there.
