#!/usr/bin/env bash
mkdir -p data
echo "Initial line." > data/append_test.txt
cp data/append_test.txt data/overwrite_test.txt
