#!/usr/bin/env bash
mkdir -p data/target
echo "This is the target file" > data/target/realfile.txt
ln -s target/realfile.txt data/link_to_realfile
ln -s nosuchfile data/dangling_link
