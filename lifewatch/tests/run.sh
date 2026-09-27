#!/bin/sh
set -eu

bin=${1:-./lifewatch}
tmp=${TMPDIR:-/tmp}/lifewatch-test-$$
sleep_pid=
replacement_pid=
trap 'test -z "$sleep_pid" || kill "$sleep_pid" 2>/dev/null || true; test -z "$replacement_pid" || kill "$replacement_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

: >"$tmp/sample.txt"
sleep 30 &
sleep_pid=$!
i=0
while [ "$i" -lt 20 ]; do
    test "$(cat "/proc/$sleep_pid/comm" 2>/dev/null || true)" = sleep && break
    i=$((i + 1))
    sleep 0.05
done
start_ticks=$(awk '{print $22}' "/proc/$sleep_pid/stat")

cat >"$tmp/valid.conf" <<EOF
interval_ms=250
notify_ssh=no
terminal_bell=no
min_level=warning
ui_language=auto
notification_language=ja
log_file=$tmp/notifications.log
process=sleeper|pid|$sleep_pid@$start_ticks
file=sample|$tmp/sample.txt
EOF

"$bin" validate --config "$tmp/valid.conf"
"$bin" check --config "$tmp/valid.conf" >"$tmp/check.out"
grep -q 'sample.*present' "$tmp/check.out"
kill "$sleep_pid"
wait "$sleep_pid" 2>/dev/null || true
sleep_pid=
sleep 30 &
replacement_pid=$!
if "$bin" check --config "$tmp/valid.conf" >"$tmp/check-missing.out"; then
    echo "exited PID followed a same-name process" >&2
    exit 1
fi
grep -q 'sleeper.*missing' "$tmp/check-missing.out"
kill "$replacement_pid"
wait "$replacement_pid" 2>/dev/null || true
replacement_pid=

cat >"$tmp/invalid.conf" <<EOF
interval_ms=10
log_file=relative.log
EOF
if "$bin" validate --config "$tmp/invalid.conf" >/dev/null 2>&1; then
    echo "invalid configuration unexpectedly passed" >&2
    exit 1
fi

cat >"$tmp/invalid-language.conf" <<EOF
interval_ms=500
ui_language=de
log_file=$tmp/notifications.log
EOF
if "$bin" validate --config "$tmp/invalid-language.conf" >/dev/null 2>&1; then
    echo "invalid language unexpectedly passed" >&2
    exit 1
fi

if "$bin" setup --lang de --config "$tmp/valid.conf" >/dev/null 2>&1; then
    echo "invalid --lang unexpectedly passed" >&2
    exit 1
fi

echo "tests passed"
