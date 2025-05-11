#!/usr/bin/env bash
set -e
echo "--- Testing link_to_realfile ---"
readlink link_to_realfile
cat link_to_realfile # Should output content of realfile.txt if followed

echo "--- Testing dangling_link ---"
readlink dangling_link || echo "readlink dangling_link failed as expected (or target is just 'nosuchfile')"
cat dangling_link || echo "cat dangling_link failed as expected"

ls -l 
