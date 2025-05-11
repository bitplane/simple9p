#!/usr/bin/env bash
mkdir -p data
echo "File for chmod" > data/perm_test_file.txt
chmod 644 data/perm_test_file.txt
