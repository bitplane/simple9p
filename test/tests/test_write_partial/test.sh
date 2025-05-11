#!/usr/bin/env bash
set -e
echo -n "XXXXX" | dd of=partial_write_target.txt bs=1 seek=10 count=5 conv=notrunc status=none
# Expected: AAAAAAAAAAXXXXXBBBBBCCCCCCCCCC
cat partial_write_target.txt
