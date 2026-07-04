# AutoTest Script HOWTO

This document is the source of truth for writing AutoTest script bodies for
AutoTest Script Builder TUI.

When asked to create a test, output only the script body that should be pasted
into the AutoTest editor. Do not include Markdown fences unless explicitly
requested. Do not invent directives that are not listed here.

## Core Model

An AutoTest script body is Bash plus AutoTest directives.

The generated test script reports `OK` when:

- the script exit status matches the expected exit status, normally `0`
- every `@check` directive succeeds

The generated test script reports `NG` when any check fails or the final exit
status is different from the expected status.

Prefer this pattern:

```sh
@capture command_to_test
@check AUTOTEST_STATUS exact 0
@check AUTOTEST_STDOUT exact expected-output
@check AUTOTEST_STDERR empty
```

For more complex logic, assign the value you want to check to a Bash variable,
then check that variable:

```sh
state=$(systemctl is-active ssh)
@check state exact active
```

## Directive Reference

| Directive | Purpose | Form |
|---|---|---|
| `@check` | Assert a variable value | `@check VAR exact VALUE` |
| `@capture` | Run a command and capture stdout, stderr, and status | `@capture command` |
| `@tui` | Run a pseudo-terminal command and send TUI input | `@tui command` ... `@end` |
| `@assert` | Fail immediately if a shell condition is false | `@assert condition` |
| `@backup` | Save a file, directory, or symlink before mutation | `@backup /path` |
| `@restore` | Restore a previously backed-up path | `@restore /path` |
| `@reboot-if` | Reboot only when a shell condition is true | `@reboot-if condition` |
| `@evidence` | Write a prompted command and its output to evidence | `@evidence command` |
| `@evidence-capture` | Write evidence and capture stdout, stderr, and status | `@evidence-capture command` |
| `@evidence-vars` | Write resolved variable values to evidence | `@evidence-vars VAR...` or `@evidence-vars "label"=VAR...` |
| `@evidence-comment` | Write a comment line to evidence | `@evidence-comment text` |

Directives are line-oriented. `@capture` is a one-line command directive.
Do not wrap simple commands in `sh -c` or `bash -c`; write the command directly.

Good:

```sh
@capture ./test.sh /etc/conf
@check AUTOTEST_STATUS exact 0
```

Avoid unnecessary wrappers:

```sh
@capture sh -c './test.sh /etc/conf'
```

For loops and conditionals, write normal Bash and put AutoTest directives inside
the block:

```sh
for tc in testcase1 testcase2 testcase3; do
  @capture ./test.sh "$tc"
  @check AUTOTEST_STATUS exact 0
done
```

Use `sh -c '...'` or `bash -c '...'` only when several shell commands must be
captured as one single `@capture` result.

## Match Types

`@check` supports these match types:

| Match | Meaning |
|---|---|
| `exact` | Expected regex must match the whole value |
| `not_exact` | Expected regex must not match the whole value |
| `contains` | Expected regex must match somewhere in the value |
| `not_contains` | Expected regex must not match anywhere in the value |
| `empty` | Value must be empty |
| `not_empty` | Value must not be empty |
| `none` | Always succeeds |

`exact`, `not_exact`, `contains`, and `not_contains` use extended regular
expressions. `exact` is a full-value regex match, not a literal string compare.

Examples:

```sh
@check status exact active
@check count exact [0-9]+
@check message contains "service .* started"
@check error empty
@check output not_contains "permission denied"
```

For multi-line expected text, use heredoc syntax:

```sh
actual=$(cat /tmp/result)

@check actual exact <<'EOF'
line one
line two
EOF
```

## Capturing Command Output

Use `@capture <command>` to capture the result of a normal shell command.

```sh
@capture systemctl is-active ssh

@check AUTOTEST_STDOUT exact active
@check AUTOTEST_STATUS exact 0
@check AUTOTEST_STDERR empty
```

Each `@capture` updates these variables:

- `AUTOTEST_STDOUT`: stdout from the captured command
- `AUTOTEST_STDERR`: stderr from the captured command
- `AUTOTEST_STATUS`: exit status from the captured command
- `AUTOTEST_STDOUT_FILE`: backing file for stdout
- `AUTOTEST_STDERR_FILE`: backing file for stderr

`@capture` itself returns success. A non-zero captured command does not stop the
script. Check `AUTOTEST_STATUS` when the exit status matters.

`@capture` can be used inside `for`, `while`, and `if` blocks:

```sh
for tc in testcase1 testcase2 testcase3; do
  @capture ./test.sh "$tc"
  @check AUTOTEST_STATUS exact 0
  @check AUTOTEST_STDOUT exact 0
done
```

`AUTOTEST_STDOUT`, `AUTOTEST_STDERR`, and `AUTOTEST_STATUS` always contain the
most recent `@capture` result.

Use `@evidence-capture <command>` when the command under test should both appear
in the evidence log and update the same variables as `@capture`:

```sh
@evidence-comment run test.sh
@evidence-capture ./test.sh /etc/conf
@check AUTOTEST_STATUS exact 0
```

`@evidence-capture` runs the command once. It always updates
`AUTOTEST_STDOUT`, `AUTOTEST_STDERR`, `AUTOTEST_STATUS`,
`AUTOTEST_STDOUT_FILE`, and `AUTOTEST_STDERR_FILE`. When `--evidence` is used,
it also writes the prompt, command, stdout, stderr, and `# exit status: N` to
the evidence log.

Use `@evidence-vars <vars...>` when the evidence should show resolved values
without adding an extra prompted command line. Use plain variable names for
machine-like output, or `"label"=VAR` for human-readable evidence:

```sh
script_path="$(pwd)/test.sh"
outfile="$tmpdir/result.txt"
expected="$(pwd)"

@evidence-comment resolved paths
@evidence-vars script_path outfile expected
@evidence-vars "script path"=script_path "output file"=outfile "expected current directory"=expected
```

Evidence output:

```text
# variables
script_path=/root/test.sh
outfile=/tmp/tmp.xxxx/result.txt
expected=/tmp/tmp.xxxx/work
script path: /root/test.sh
output file: /tmp/tmp.xxxx/result.txt
expected current directory: /tmp/tmp.xxxx/work
```

`@evidence-vars` does not run a command and does not update `AUTOTEST_*`.

## TUI Automation

Use `@tui <command>` when the command needs a pseudo terminal, such as `vim`:

```sh
@tui vim /tmp/message
    vim-clear
    send i
    text hello
    text world
    vim-write-quit
@end

actual=$(cat /tmp/message)
@check actual exact <<'EOF'
hello
world
EOF
```

`@tui` updates these variables:

- `AUTOTEST_TUI_STDOUT`: cleaned TUI output for assertions
- `AUTOTEST_TUI_TEXT`: same cleaned output as `AUTOTEST_TUI_STDOUT`
- `AUTOTEST_TUI_TRANSCRIPT`: raw pseudo-terminal transcript
- `AUTOTEST_TUI_STDERR`: stderr from the `script` wrapper
- `AUTOTEST_TUI_STATUS`: exit status from the `script` command
- `AUTOTEST_TUI_STDOUT_FILE`: cleaned output backing file
- `AUTOTEST_TUI_TEXT_FILE`: same cleaned output backing file
- `AUTOTEST_TUI_TRANSCRIPT_FILE`: raw transcript backing file
- `AUTOTEST_TUI_INPUT_FILE`: printable input backing file
- `AUTOTEST_TUI_STDERR_FILE`: stderr backing file

For assertions, prefer `AUTOTEST_TUI_STDOUT` or `AUTOTEST_TUI_TEXT`.
Use `AUTOTEST_TUI_TRANSCRIPT` only when you intentionally need raw terminal
control sequences.

### TUI Commands

Inside an `@tui` block, use only these commands:

| Command | Meaning |
|---|---|
| `send <keys>` | Send text as written |
| `text <text>` | Send text, then newline |
| `send-shell <expr>` | Send shell-expanded text |
| `text-shell <expr>` | Send shell-expanded text, then newline |
| `enter` | Send Enter |
| `esc` | Send Escape |
| `tab` | Send Tab |
| `space` | Send Space |
| `sleep <seconds>` | Wait |
| `ctrl <key>` | Send Ctrl-key, for example `ctrl c` |
| `vim-clear` | For Vim, send `ggdG` |
| `vim-write-quit` | For Vim, send Escape, then `:wq` and Enter |

Use quotes with `send` and `text` when you want grouping. Surrounding quotes are
not sent. To send a literal quote, escape it.

```sh
text "hello world"
text "\"quoted\""
```

## Reboot Tests

To create a reboot test, put a reboot command directly in the script body.
Commands before the reboot run before reboot. Commands after the reboot are
automatically resumed after boot.

```sh
echo before > /root/reboot-test.log
reboot
echo after >> /root/reboot-test.log

actual=$(cat /root/reboot-test.log)
@check actual exact <<'EOF'
before
after
EOF
```

Supported reboot forms include commands that look like:

- `reboot`
- `systemctl reboot`
- `shutdown -r`
- `init 6`
- `@reboot-if <condition>`

Use `@reboot-if` when reboot is conditional:

```sh
need_reboot=yes
@reboot-if [ "$need_reboot" = yes ]
echo resumed

@capture cat /root/result
@check AUTOTEST_STDOUT contains resumed
```

Reboot tests need root privileges because the generated script installs a
systemd resume unit.

## Backup and Restore

Use `@backup` before modifying a path and `@restore` when you want to restore
it later. The path may be a file, directory, or symlink.

```sh
@backup /etc/example.conf
printf 'new value\n' > /etc/example.conf

@capture cat /etc/example.conf
@check AUTOTEST_STDOUT exact "new value"

@restore /etc/example.conf
```

If the path did not exist at backup time, restore removes the path. If it did
exist, restore replaces it with the saved original.

## Assertions

Use `@assert <condition>` for required preconditions or immediate failures:

```sh
@assert command -v systemctl >/dev/null
@assert [ "$(id -u)" -eq 0 ]
```

Use `@check` when you want the value to appear in the final check details.

## Evidence Output

Evidence output is enabled by running the generated test with:

```sh
./test-name.sh --evidence evidence.log
```

Only explicit evidence directives write to the evidence file:

```sh
@evidence-comment before service check
@evidence systemctl status ssh --no-pager
```

`@evidence` commands always run. When `--evidence` is not specified, their
stdout and stderr are discarded. When `--evidence` is specified, they write the
prompt, command, stdout, and stderr to the evidence file. Their exit status does
not affect the test result.

### Evidence Style

When a test prepares files, directories, or configuration, prefer running the
preparation commands with `@evidence` so the evidence log shows the test
precondition.

Use `@evidence-comment` as a short section label. Do not duplicate the command
itself in the comment; the command line is already written by `@evidence` or
`@evidence-capture`.

When you need to show resolved variable values such as paths, prefer
`@evidence-vars` instead of `@evidence echo "$var"`.

Use `@evidence-vars` only for values that help explain the next evidence item or
the next important check. Do not dump unrelated setup variables at the top of the
test. Place the variable evidence close to the command or check that uses it.
Do not use it as evidence for produced files or command results; show produced
artifacts with `@evidence` instead:

```sh
tmpdir=$(mktemp -d)
outfile="$tmpdir/result.txt"
expected="$(pwd)"

@evidence-comment run test.sh
@evidence-vars "output file"=outfile "expected current directory"=expected
@evidence-capture ./test.sh "$outfile"
@check AUTOTEST_STATUS exact 0

@evidence-comment written file
@evidence cat "$outfile"
actual=$(cat "$outfile")
content_ok=no
[ "$actual" = "$expected" ] && content_ok=yes

@evidence-comment compare current directory path with outfile content
@evidence-vars "current directory path"=expected "outfile content"=actual
@check content_ok exact yes
```

Do not record routine setup or cleanup commands such as `rm -f "$outfile"` or
`rm -rf "$tmpdir"` unless they are meaningful test preconditions. Prefer a clear
comment such as `@evidence-comment run test.sh` over vague comments like
`@evidence-comment prepare`.

| Directive | Runs command | Writes evidence | Updates `AUTOTEST_*` |
|---|---:|---:|---:|
| `@evidence` | yes | yes, when `--evidence` is used | no |
| `@capture` | yes | no | yes |
| `@evidence-capture` | yes | yes, when `--evidence` is used | yes |
| `@evidence-vars` | no | yes, when `--evidence` is used | no |

Default style for tests that modify a path:

1. Back up the path with `@backup`.
2. Prepare the file or configuration with `@evidence`.
3. Record resolved paths or expected values with `@evidence-vars`.
4. Record prepared state with `@evidence`.
5. Run the command under test with `@evidence-capture` when its result should be
   checked and logged.
6. Check `AUTOTEST_STATUS`, `AUTOTEST_STDOUT`, or `AUTOTEST_STDERR`.
7. Restore the path with `@restore`.

Example:

```sh
@backup /etc/conf

@evidence-comment prepare /etc/conf
@evidence rm -f /etc/conf
@evidence echo 1 > /etc/conf
@evidence ls -l /etc/conf
@evidence cat /etc/conf

@evidence-comment run test.sh
@evidence-capture ./test.sh /etc/conf
@check AUTOTEST_STATUS exact 0

@restore /etc/conf
```

## Generated Script Options

Generated test scripts support:

```sh
./test-name.sh --detail
./test-name.sh --detail detail.log
./test-name.sh --detail=detail.log
./test-name.sh --evidence evidence.log
./test-name.sh --help
```

`--detail` prints detailed result information to stdout. `--detail PATH` writes
the detail information to a file. Unknown options print usage and exit with
status `2`.

## Common Patterns

### Check a Service Is Active

```sh
@capture systemctl is-active ssh
@check AUTOTEST_STATUS exact 0
@check AUTOTEST_STDOUT exact active
@check AUTOTEST_STDERR empty
```

### Check File Content

```sh
actual=$(cat /root/message)
@check actual exact <<'EOF'
/a/b/c/d : protected
/i/j : protected
EOF
```

### Check Lines Appear in Order

Use a regex with `contains` when extra text may appear between lines:

```sh
actual=$(cat /root/message)
@check actual contains '/a/b/c/d : protected(.|\n)*/i/j : protected'
```

### Loop Over Test Cases

```sh
for tc in testcase1 testcase2 testcase3; do
  @capture ./test.sh "$tc"
  @check AUTOTEST_STATUS exact 0
  @check AUTOTEST_STDOUT exact 0
done
```

### Edit a File with Vim

```sh
@tui vim /tmp/testfile
    vim-clear
    send i
    text new text
    vim-write-quit
@end

actual=$(cat /tmp/testfile)
@check actual exact "new text"
```

## Things to Avoid

- Do not invent directives.
- Do not wrap simple commands in `sh -c` or `bash -c`.
- Do not use `sh -c` or `bash -c` for loops or conditionals just because they
  span multiple lines. Write normal Bash and place AutoTest directives inside.
- Use `sh -c '...'` or `bash -c '...'` only when multiple commands must be
  captured as one single `@capture` result.
- Do not rely on raw TUI transcripts unless the test intentionally checks raw
  terminal output.
- Do not rely on `runlevel`, `who -r`, `who`, `w`, or `users` immediately after
  reboot; these commands depend on utmp/session state and may be stale or
  unknown.
- Do not assume network state is ready immediately after reboot unless the test
  waits for it.
- Do not use parallel execution for tests that mutate shared system state.

## Output Format for AI Assistants

When generating a test for a user, output only the AutoTest script body:

```sh
@capture command
@check AUTOTEST_STATUS exact 0
```

Do not include explanations, file names, or Markdown fences unless the user
explicitly asks for them.
