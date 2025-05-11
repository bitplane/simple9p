#!/usr/bin/env bash
set -e
echo "Hello from basic test" > hello.txt
mkdir foodir
echo "inside foodir" > foodir/bar.txt
ls -la .
ls -la foodir
cat hello.txt
cat foodir/bar.txt
