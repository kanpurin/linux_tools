#!/bin/sh
set -eu

LTREE=${1:-./ltree}
TMPDIR_ROOT=${TMPDIR:-/tmp}
WORK="$TMPDIR_ROOT/ltree-test-$$"
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

mkdir -p "$WORK/root/alpha/deep" "$WORK/root/zeta" "$WORK/root/.hidden"
printf 'hello\n' >"$WORK/root/alpha/file.txt"
touch -t 202001020304 "$WORK/root/alpha/file.txt"
printf '#!/bin/sh\n' >"$WORK/root/run.sh"
chmod +x "$WORK/root/run.sh"
ln -s alpha/file.txt "$WORK/root/link"

OUTPUT=$($LTREE --ascii -R "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F '|-- alpha' >/dev/null
printf '%s\n' "$OUTPUT" | grep -F '|   |-- deep' >/dev/null
printf '%s\n' "$OUTPUT" | grep -F '`-- zeta' >/dev/null
printf '%s\n' "$OUTPUT" | grep -F 'link -> alpha/file.txt' >/dev/null
printf '%s\n' "$OUTPUT" | grep -F '3 directories, 3 files' >/dev/null

NBSP=$(printf '\302\240')
OUTPUT=$($LTREE -R "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F '├── alpha' >/dev/null
printf '%s\n' "$OUTPUT" | grep -F "│${NBSP}${NBSP} ├── deep" >/dev/null

OUTPUT=$($LTREE --unicode -R "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F '├── alpha' >/dev/null
printf '%s\n' "$OUTPUT" | grep -F "│${NBSP}${NBSP} ├── deep" >/dev/null

OUTPUT=$($LTREE --ascii -a -L 1 "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F '.hidden' >/dev/null
if printf '%s\n' "$OUTPUT" | grep -F 'deep' >/dev/null; then
    echo 'depth limit failed' >&2
    exit 1
fi

OUTPUT=$($LTREE --ascii -R --dirs-only "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F '3 directories' >/dev/null
if printf '%s\n' "$OUTPUT" | grep -F 'file.txt' >/dev/null; then
    echo 'directories-only mode failed' >&2
    exit 1
fi

OUTPUT=$($LTREE --ascii -R -F "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F 'run.sh*' >/dev/null
printf '%s\n' "$OUTPUT" | grep -F 'link@ -> alpha/file.txt' >/dev/null

OUTPUT=$($LTREE --ascii -R -lh "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -E -- '^-rw-r--r-- +1 +[^ ]+ +[^ ]+ +6B +2020-01-02 03:04 \|   `-- file\.txt' >/dev/null

OUTPUT=$($LTREE --ascii -R -l "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -E -- '^-rw-r--r-- +1 +[^ ]+ +[^ ]+ +6 +2020-01-02 03:04 \|   `-- file\.txt' >/dev/null

OUTPUT=$($LTREE --ascii -p -L 1 "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F 'alpha/' >/dev/null

OUTPUT=$($LTREE --ascii -d "$WORK/root")
if printf '%s\n' "$OUTPUT" | grep -F 'alpha' >/dev/null; then
    echo 'directory-as-file mode failed' >&2
    exit 1
fi

printf 'x' >"$WORK/root/small.dat"
printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' >"$WORK/root/large.dat"
touch -t 202001010000 "$WORK/root/small.dat"
touch -t 202101010000 "$WORK/root/large.dat"

OUTPUT=$($LTREE --ascii -S -L 1 "$WORK/root")
FIRST=$(printf '%s\n' "$OUTPUT" | grep -E 'small\.dat|large\.dat' | head -n 1)
case "$FIRST" in
    *large.dat*) ;;
    *) echo 'size sort failed' >&2; exit 1 ;;
esac

OUTPUT=$($LTREE --ascii -t -L 1 "$WORK/root")
FIRST=$(printf '%s\n' "$OUTPUT" | grep -E 'small\.dat|large\.dat' | head -n 1)
case "$FIRST" in
    *large.dat*) ;;
    *) echo 'time sort failed' >&2; exit 1 ;;
esac

OUTPUT=$($LTREE --ascii -R --du "$WORK/root")
ALPHA_SIZE=$(printf '%s\n' "$OUTPUT" | awk '$NF == "alpha" { print $5; exit }')
case "$ALPHA_SIZE" in
    ''|*[!0-9]*) echo 'directory total is not numeric' >&2; exit 1 ;;
esac
if [ "$ALPHA_SIZE" -le 6 ]; then
    echo 'directory total did not include descendants' >&2
    exit 1
fi

OUTPUT=$($LTREE --ascii "$WORK/root")
printf '%s\n' "$OUTPUT" | grep -F '|-- alpha' >/dev/null
if printf '%s\n' "$OUTPUT" | grep -F 'deep' >/dev/null; then
    echo 'default depth is not one level' >&2
    exit 1
fi

echo 'ltree tests: OK'
