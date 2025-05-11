#!/usr/bin/env bash
set -e
UNICODE_DIR="目录_Тест_文件夹"
UNICODE_FILE="你好_Привіт_नमस्ते.txt"
CONTENT="Unicode content: 世界 Γειά σου Salve"

mkdir "$UNICODE_DIR"
echo "$CONTENT" > "$UNICODE_DIR/$UNICODE_FILE"

if [[ ! -d "$UNICODE_DIR" ]]; then echo "ERROR: Unicode dir not created." >&2; exit 1; fi
if [[ ! -f "$UNICODE_DIR/$UNICODE_FILE" ]]; then echo "ERROR: Unicode file not created." >&2; exit 1; fi

CAT_CONTENT=$(cat "$UNICODE_DIR/$UNICODE_FILE")
if [[ "$CAT_CONTENT" != "$CONTENT" ]]; then
  echo "ERROR: Unicode file content mismatch." >&2
  echo "Expected: $CONTENT" >&2
  echo "Actual: $CAT_CONTENT" >&2
  exit 1
fi
echo "Unicode names and content handling OK."
ls -R
