#!/usr/bin/env bash
export NO_MOUNT=1 # Tell harness not to mount
mkdir -p data
echo "This is a local file for a no_mount test." > data/local_file.txt
echo "Setup for no_mount complete. NO_MOUNT=$NO_MOUNT"
