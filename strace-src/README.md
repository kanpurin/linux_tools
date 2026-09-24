# strace-src

`strace-src` is a two-pane ncurses viewer that connects each `strace` event to
the source line that caused it.

## Requirements

- Linux
- `strace` built with `-k` stack-trace support
- `addr2line` (binutils)
- a C compiler and ncurses development files
- target binaries/libraries built with debug information (`-g`)

## Build and test

```sh
make
make test
```

## Run

Start a new non-interactive command:

```sh
./strace-src ./program arg1 arg2
./strace-src -o run.trace ./program arg1 arg2
```

Attach to a running process:

```sh
./strace-src -p PID
```

Open a saved trace without starting a process:

```sh
./strace-src run.trace
```

Trace files use the versioned `strace-src` binary format. They contain syscall
text, PID/TID and process names, stack frames, and resolved source locations,
but not source file contents. Source is read again from its saved absolute path
when the trace is opened.

The traced command's stdout and stderr are captured separately so they cannot
corrupt the TUI. They are deliberately not shown in v1 and are removed when
`strace-src` exits.

## Keys

| Key | Action |
| --- | --- |
| `Up` / `k` | Previous trace, or scroll Source up when Source is focused |
| `Down` / `j` | Next trace, or scroll Source down when Source is focused |
| `PgUp` / `PgDn` | Move one page in the focused pane |
| `g` | First trace, or top of Source |
| `G` | Latest trace, or bottom of Source |
| `Tab` | Move focus between Trace Log and Source |
| `/` | Filter by text anywhere in the displayed trace line |
| `e` | Open the syscall checkbox filter menu |
| `p` | Open the PID/TID checkbox filter menu |
| `P` | Temporarily show the process/thread graph |
| `r` | Show traces resolved to the currently selected source line |
| `o` | Jump to the trace that created the selected FD |
| `i` | Show origin, history, target, and state for the selected FD |
| `s` | Open the stack and choose the Source frame |
| `?` | Show keyboard help |
| `Esc` | Clear all active filters |
| `q` | Quit |

In the syscall menu, use `Up`/`Down` or `k`/`j` to move, `Space` to toggle,
`a`/`n` to select all/none, `/` to search names, Enter to apply, and Esc to
cancel. Active filters appear in the Trace Log title and are combined with AND.
Filtering only changes the visible event list; every captured event remains in
memory and is included when saving.

The PID/TID menu uses the same movement, toggle, all/none, apply, and cancel
keys. Process filtering is combined with text, syscall, and source-line filters
using AND. The `P` popup reconstructs process and thread relationships from
fork, clone, and exec traces and highlights the producer of the selected trace.

Trace rows that resolve to source are shown in the terminal's accent color.
The selected row uses a stronger cursor style, regardless of its source state.
For failed syscalls in the `= -1 ERRNO` form, only the return value and errno
portion is highlighted in red so the source-resolution color remains visible.
When the selected trace refers to a tracked FD, FD numbers from the same open
or socket/pipe/accept operation are highlighted in yellow. `dup` descendants
share that color, while a number reused after `close` belongs to a new FD
object. FD state is inherited across fork/clone, retained across exec, and
discarded at exec for descriptors marked `O_CLOEXEC` or `FD_CLOEXEC`. Pressing
`o` follows one creator at a time, so a duplicated descriptor first jumps to
the `dup` event and a second press reaches the original open.

The Stack popup starts on the automatically selected Source frame. Use
`Up`/`Down` or `k`/`j` to choose another source-resolved frame and Enter to use
it in the Source pane. This override lasts only while that trace remains
selected. Stack, FD Info, process graph, and help are temporary popups; their
shortcut, Esc, or `q` closes them while tracing continues in the background.

The source pane is read-only. Its contents can be scrolled while it is focused;
switching back to Trace Log restores the Source view to the selected trace's
original source position without changing the selected trace. If a selected stack has no non-runtime frame that
resolves to a readable source line, or a reopened trace refers to a source file
that no longer exists, it displays `Source unavailable`.

The focused pane title is shown in bold. Source remains read-only when focused;
trace navigation and filter shortcuts continue to work from either pane.
