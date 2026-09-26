#!/bin/sh
set -eu

bin=${1:-./strace-src}

LC_ALL=C.UTF-8 "$bin" --lang ja --help | grep -q '^使用法:'
LC_ALL=C.UTF-8 "$bin" --lang en --help | grep -q '^Usage:'

if LC_ALL=C.UTF-8 "$bin" --lang invalid --help >/dev/null 2>&1; then
    echo "invalid language unexpectedly accepted" >&2
    exit 1
fi

echo "language tests passed"
