#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
core=./bin/pps-core

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

version=$($core --version)
[[ $version == 'pps 1.0.0' ]] || fail "unexpected version: $version"

info=$($core $$)
grep -q "^PID[[:space:]]*$$$" <<<"$info" || fail 'PID detail missing'
grep -q '^Command' <<<"$info" || fail 'Command detail missing'
grep -q '^Threads' <<<"$info" || fail 'Threads detail missing'

$core $$ --fd | grep -q '^FD' || fail 'FD heading missing'
$core $$ --threads | grep -q '^TID' || fail 'thread heading missing'
$core $$ --tree | grep -q "\[$$\]" || fail 'tree root missing'
$core $$ --limits | grep -q '^Max open files' || fail 'limits missing'

bash -c 'exec -a pps-signal-test-victim sleep 30' &
victim=$!
cleanup_victim() { kill -KILL "$victim" 2>/dev/null || true; }
trap cleanup_victim EXIT
$core pps-signal-test-victim --stop
state=$(awk '{print $3}' "/proc/$victim/stat")
[[ $state == T ]] || fail "SIGSTOP did not stop victim (state=$state)"
$core "$victim" --cont
$core "$victim" --term
wait "$victim" 2>/dev/null || true
trap - EXIT

if $core this-process-name-cannot-exist-pps-test >/tmp/pps-test.out 2>/tmp/pps-test.err; then
    fail 'not-found search succeeded'
fi
grep -q 'no process matched' /tmp/pps-test.err || fail 'not-found error missing'

if ./bin/pps >/tmp/pps-test.out 2>/tmp/pps-test.err; then
    fail 'launcher succeeded without integration'
fi
grep -q 'requires interactive Bash integration' /tmp/pps-test.err || fail 'launcher error missing'

printf '%s\n' 'all non-interactive tests passed'

if command -v python3 >/dev/null 2>&1; then
    python3 tests/interactive.py "$core"
else
    printf '%s\n' 'interactive tests skipped (python3 is not installed)'
fi
