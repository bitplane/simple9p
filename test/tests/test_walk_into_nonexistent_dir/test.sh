#!/usr/bin/env bash
# Don't set -e, we expect failures
ls this_dir_does_not_exist || echo "ls: Expected failure: dir does not exist"
cd this_dir_does_not_exist || echo "cd: Expected failure: dir does not exist"
echo "Test complete."
