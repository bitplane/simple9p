#!/usr/bin/env bash
set -e
echo "Current PWD: $(pwd)"
ls -a

echo "--- Testing complex cd and ls ---"
cd dirA/dirB/dirC/.././../dirA/./dirB
pwd # Should be in dirA/dirB
ls -a # Should show dirC and fileA (if ls .. works)

cd ../../../ # Should be at the root of the mount
pwd
ls -a # Should show dirA

echo "--- Testing access via complex paths ---"
cat ./dirA/dirB/../fileA.txt # Should be content_A
cat dirA/./dirB/dirC/fileC.txt # Should be content_C

# Test going above root (should stay at root)
cd ..
pwd
ls -a # Should still be root content

cd ../../..
pwd
ls -a # Still root content

echo "Path traversal tests complete."
