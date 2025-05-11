#!/usr/bin/env bash
set -e
echo "Before truncate:"
ls -l truncate_me.txt
wc -c truncate_me.txt

# The 'truncate' command might use ftruncate syscall, which FUSE translates to setattr (wstat)
# if the file is open, or a direct truncate operation.
# We'll see if simple9p + FUSE handles the wstat to change length.
truncate -s 0 truncate_me.txt

echo "After truncate -s 0:"
ls -l truncate_me.txt
wc -c truncate_me.txt # Should be 0 if truncate worked via wstat

cat dont_touch_me.txt # Verify other files are okay
