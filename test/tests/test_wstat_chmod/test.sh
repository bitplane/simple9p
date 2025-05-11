#!/usr/bin/env bash
set -e
echo "Initial permissions:"
ls -l perm_test_file.txt
chmod 755 perm_test_file.txt
echo "Permissions after chmod 755:"
ls -l perm_test_file.txt
# Further checks would involve verifying the mode bits if 'stat' output is stable
