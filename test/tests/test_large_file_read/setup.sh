#!/usr/bin/env bash
mkdir -p data
# Create a 5MB file (5 * 1024 * 1024 bytes)
dd if=/dev/zero of=data/largefile.dat bs=1M count=5 status=none
echo "Large file created."
