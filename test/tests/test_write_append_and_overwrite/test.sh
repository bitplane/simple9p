#!/usr/bin/env bash
set -e
# Test append (might not be supported by simple9p O_APPEND flag directly in create/open)
# Standard redirection '>>' relies on underlying FS supporting append mode correctly.
echo "Appended line." >> append_test.txt
cat append_test.txt

# Test overwrite
echo "Overwritten content." > overwrite_test.txt
cat overwrite_test.txt
echo "Append and overwrite tests completed."
