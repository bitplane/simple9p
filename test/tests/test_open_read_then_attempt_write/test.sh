#!/usr/bin/env bash
# set -e # Expecting write to potentially fail or behave differently
echo "Content before any operations:"
cat ro_file.txt
ls -l ro_file.txt

echo "--- Attempting append after a read (cat) ---"
# 'cat' opens read-only. Then 'echo >>' opens for append.
# If the server's FID state from 'cat' somehow lingered and was read-only, '>>' might fail.
# More likely, '>>' gets a new FID or re-opens with write/append flags.
# The behavior depends on how FUSE translates this and how simple9p handles open modes.
(cat ro_file.txt > /dev/null; echo "Appended after cat." >> ro_file.txt) || echo "Append command failed (expected if RO enforcement is strict)"

echo "Content after attempted append:"
cat ro_file.txt
ls -l ro_file.txt

echo "--- Attempting overwrite after a read (cat) ---"
(cat ro_file.txt > /dev/null; echo "Overwritten after cat." > ro_file.txt) || echo "Overwrite command failed (expected if RO enforcement is strict)"

echo "Content after attempted overwrite:"
cat ro_file.txt
ls -l ro_file.txt
echo "Read-then-write test complete."
