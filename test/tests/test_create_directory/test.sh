#!/usr/bin/env bash
set -e
DIRNAME="newly_created_dir"
mkdir "$DIRNAME"
if [[ ! -d "$DIRNAME" ]]; then
  echo "ERROR: Directory '$DIRNAME' not created." >&2
  exit 1
fi
echo "Directory '$DIRNAME' created."
ls -ld "$DIRNAME"
