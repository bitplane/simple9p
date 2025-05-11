#!/usr/bin/env bash
mkdir -p data
echo "Initial content for read-only test." > data/ro_file.txt
# Underlying actual file is writable by user
chmod 644 data/ro_file.txt
