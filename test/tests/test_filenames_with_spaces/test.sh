#!/usr/bin/env bash
set -e
FILENAME_WITH_SPACES="a file with spaces in its name.txt"
DIR_WITH_SPACES="a dir with spaces"
CONTENT="File with spaces test."

echo "$CONTENT" > "$FILENAME_WITH_SPACES"
mkdir "$DIR_WITH_SPACES"
echo "inside dir with spaces" > "$DIR_WITH_SPACES/another file.txt"

if [[ ! -f "$FILENAME_WITH_SPACES" ]]; then echo "ERROR: File with spaces not created." >&2; exit 1; fi
if [[ ! -d "$DIR_WITH_SPACES" ]]; then echo "ERROR: Dir with spaces not created." >&2; exit 1; fi
if [[ "$(cat "$FILENAME_WITH_SPACES")" != "$CONTENT" ]]; then echo "ERROR: Content mismatch." >&2; exit 1; fi

echo "Filenames with spaces test OK."
ls -l
ls -l "$DIR_WITH_SPACES"
