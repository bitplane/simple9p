#!/usr/bin/env bash
set -e
# Create a moderately deep path first
DEPTH1=d1/d2/d3/d4/d5/d6/d7/d8/d9/d10 # 10 levels * 3 chars = 30 chars
mkdir -p $DEPTH1
echo "content in moderately deep path" > $DEPTH1/file_moderate.txt
cat $DEPTH1/file_moderate.txt
ls -R d1

# Attempt a very deep path, each component is short
# 100 components of "s/" = 200 chars. Plus base.
# This might hit PATH_MAX or server limits.
DEEP_PATH_BASE="deep"
CURRENT_PATH="$DEEP_PATH_BASE"
for i in {1..50}; do # Create 50 levels deep_path/s/s/s...
  CURRENT_PATH="$CURRENT_PATH/s"
done

echo "Attempting to create very deep path: $CURRENT_PATH"
mkdir -p "$CURRENT_PATH" || { echo "mkdir -p for very deep path failed. This might be expected."; ls -ld "$DEEP_PATH_BASE" 2>/dev/null || true; exit 0; }

# If mkdir -p succeeded, try to write a file
if [[ -d "$CURRENT_PATH" ]]; then
  echo "Deep path created. Writing file."
  echo "content in very deep path" > "$CURRENT_PATH/file_very_deep.txt"
  if [[ -f "$CURRENT_PATH/file_very_deep.txt" ]]; then
    echo "Successfully wrote to very deep path."
    cat "$CURRENT_PATH/file_very_deep.txt"
  else
    echo "Failed to write file to very deep path, even though directory was created."
  fi
  ls -ld "$CURRENT_PATH" "$CURRENT_PATH/file_very_deep.txt" 2>/dev/null || true
else
  echo "Very deep path was not fully created."
  ls -ld "$DEEP_PATH_BASE" 2>/dev/null || true # Show what was created
fi
echo "Max path length tests complete."
