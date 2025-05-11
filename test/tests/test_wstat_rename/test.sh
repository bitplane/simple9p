#!/usr/bin/env bash
set -e
mv original.txt renamed.txt
if [[ -f "original.txt" ]]; then echo "ERROR: original.txt still exists" >&2; exit 1; fi
if [[ ! -f "renamed.txt" ]]; then echo "ERROR: renamed.txt not found" >&2; exit 1; fi
CONTENT=$(cat renamed.txt)
if [[ "$CONTENT" != "Original content" ]]; then echo "ERROR: Content mismatch" >&2; exit 1; fi
echo "Rename successful, content verified."
ls -la
