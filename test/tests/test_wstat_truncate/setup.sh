#!/usr/bin/env bash
mkdir -p data
echo "This is a line of text that should be truncated." > data/truncate_me.txt
echo "Another file that should remain unchanged." > data/dont_touch_me.txt
