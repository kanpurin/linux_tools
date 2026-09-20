# gd - lightweight GDB source TUI

`gd` is a small C/ncurses front end for GDB/MI. It presents source when line
debug information is available and automatically falls back to disassembly,
registers, and address-only stack frames when it is not.

## Requirements

- Linux, GCC (or another C11 compiler)
- GDB 10.2 or newer
- ncursesw development files

On RHEL 9: `dnf install gcc make gdb ncurses-devel`

## Build and run

```sh
make -C gd
./gd/gd ./program arg1 arg2
# also accepted:
./gd/gd --args ./program arg1 arg2
```

Compile the target with `-g` for Source Mode and named Args/Locals. Programs
built without `-g` remain debuggable in Assembly Mode; variable and argument
names are not guessed.

## Install

For a persistent system-wide command:

```sh
sudo make -C gd install
gd ./program arg1 arg2
```

The current VM keeps the project under `/opt/gd` and installs the executable as
`/usr/local/bin/gd`. Temporary directories are not used for deployment.

## Modes and keys

`gd` starts in **GDB** control mode. `F2` switches between GDB and the read-only
**VIM** navigation mode. In Source Mode, `Tab` moves through Source, Variables,
and Stack. In Assembly Mode it moves through Disassembly, Registers, and Stack.
`Shift-Tab` moves in reverse. The active input mode, `SRC`/`ASM` mode, and pane
are always shown in the title. `d` switches Source and Disassembly manually.

VIM mode supports `h/j/k/l`, `w/b/e`, `0/^/$`, counts such as `5j`, `gg/G`,
`H/M/L`, `{`/`}`, `PageUp/PageDown`, `Ctrl-u/Ctrl-d`, `Ctrl-f/Ctrl-b`,
`zz/zt/zb`, matching-bracket `%`, `/` and `?` searches with `n/N`, word searches
with `*`/`#`, and declaration jumps with `gd`/`gD`. `gd` searches backward for
a local declaration; `gD` searches from the beginning of the current file.

In the Variables pane, `j/k` selects a variable, `Enter` expands or collapses
one level using GDB variable objects, `p` evaluates the selected expression,
`a` opens address or pointer details, and `w` creates a watchpoint. `B` creates
a conditional breakpoint at the remembered Source cursor line; an input such
as `== 5` is automatically combined with the selected variable.

Variables are grouped as `Args` and `Locals` for GDB's currently selected stack
frame. Arguments come only from `-stack-list-arguments` for that frame level;
locals come only from `-stack-list-locals`. `-stack-list-variables` is not
merged into either group, and duplicate MI records are removed within a group.

For a scalar or structure member, the `a` panel shows its expression, type,
value, and storage address. For a pointer it shows the stored address and the
GDB result of dereferencing it. The pointer address is compared with the
addresses of current arguments, locals, and already-expanded member rows;
matching expressions are listed without expanding any additional structures.
NULL is shown without dereferencing, inaccessible memory is shown as
`<unavailable>`, and `<optimized out>` values are not compared.

While stopped, `E` edits the selected argument, local, expanded structure
member, pointer, or register. The dialog accepts a GDB expression as the new
value, shows the old-to-new change, and requires explicit `y` confirmation.
Assignments are evaluated by GDB with `-data-evaluate-expression`; the TUI does
not parse C values. Variables, expanded children, and registers are refreshed
immediately. Optimized-out values and assignments rejected by GDB remain
unchanged. These edits affect only the debugged process, never the source file.

`R` opens Force Return for the currently selected frame. When debug type
information is available, the return type is shown and `void` functions omit
the value field. The action requires a second confirmation, executes GDB's
`return`, then refreshes the active frame, Source/Disassembly, Variables,
Registers, Stack, and execution position. For a symbol without return-type
debug information, the type remains `<unknown>`. On x86-64 System V only, if
GDB cannot apply `return VALUE` without that type, `gd` performs a valueless
frame return and writes the explicitly supplied raw integer/pointer result to
`rax`; floating-point and aggregate return conventions are not guessed.

In the Stack pane, `j/k` selects any frame and `Enter` makes it GDB's active
frame, refreshes Variables for that frame, and opens its source location.
Stepping into a function in another source file switches Source automatically.
`[P]` identifies project frames and `[X]` identifies external-library frames.

`o` opens a navigation chooser. Function search reuses the same incremental
GDB symbol search as `F`, but opens the function instead of setting a
breakpoint. Functions with line information open in Source; symbols without a
source location open in Disassembly. File search recursively indexes C/C++
source and header files below the inferred project root. Files not reported by
GDB as line-debug sources are labeled `REFERENCE ONLY`; `b` and `B` are refused
there because a source line cannot be mapped safely to an instruction.

Execution and browsing positions are independent. `=>` marks the execution
line and `>` marks the browsing cursor. Source and Disassembly headings say
`[EXEC]` or `[VIEW]`, and `e` returns to the current execution position without
changing debugger state.

Locations visited through Open, stepping, Stack selection, breakpoint-list
jumps, and assembly call-target browsing are recorded in a 64-entry history.
`[` or `Ctrl-o` moves back and `]` moves forward. Terminals encode `Ctrl-i` as
the same byte as `Tab`, so `]` is the unambiguous forward-history key while
`Tab` remains pane navigation. The history position and project/external
classification are shown in the Source heading.

In GDB mode, `r` runs, `n` steps over, `s` steps into, `c` continues, and `f`
finishes the current function. `b` toggles a breakpoint at the cursor; `B`
prompts for a GDB condition; `F` opens the function-symbol search; `C` opens
the Catch Event menu; `w` creates a watchpoint. `p` evaluates an expression.
`L`, `O`, and `h` open the stop-condition list, program output, and
help; lowercase `o` opens source navigation. In the `L` list, `Enter` opens a breakpoint's source even when it belongs
to another file. `q` quits in either mode.

`r` is accepted only before the program starts or after it exits. While the
program is running it is ignored; while stopped, use `c` to continue.

## Assembly Mode

When the selected frame has no usable `fullname` and source line, `gd`
automatically enters Assembly Mode. The Disassembly pane shows the selected
frame's function or an address range around its PC, marks the current
instruction with `=>`, highlights `call` instructions and their symbol targets,
and follows the PC after every stop. `j/k` browses instructions and `b` toggles
a breakpoint at the selected instruction address. `i` uses `-exec-step-instruction`; `I` uses
`-exec-next-instruction`.

Every stop re-evaluates the currently selected frame: `fullname` plus a positive
line selects Source Mode, otherwise Assembly Mode. GDB `step-mode` is enabled so
`s` stops at the entry of a function without line information instead of
silently stepping over it; returning to a frame with line information switches
back to Source automatically.

`F` opens the Function Breakpoint dialog. Function names are queried from GDB
with `-symbol-info-functions --include-nondebug`; typing filters the candidates
incrementally, `j/k` or the arrow keys select one, and `Enter` sets the
breakpoint. Candidates show their module and address when GDB supplies them.
Executable, debug-symbol, and currently loaded shared-library functions are
included; no ELF parser or source-level name guessing is used. Typing a full
name and pressing `Enter` also attempts the GDB breakpoint directly.

Function entries are labeled `function` in the `L` stop-condition list and can
be deleted or enabled/disabled with the existing `d`/`e` keys. `Enter` opens
their source when line information exists, or their function disassembly when
only an address is available. Pending breakpoints are labeled `[pending]`.

`C` opens the Catch Event menu for syscall, signal, fork, vfork, exec, and
shared-library-load events. Syscall and signal names are completed by GDB
itself, so `gd` does not maintain an architecture-specific syscall-number
table. Catchpoints appear in the `L` list as `C#n` and use the same `d` delete
and `e` enable/disable keys. Syscall entry and return are distinguished. On
x86-64, Registers labels `orig_rax` as the syscall number on entry and `rax`
as the return value on return, and labels `rdi`, `rsi`, `rdx`, `r10`, `r8`,
and `r9` as syscall arguments 1-6. An exec
catch discards stale source browsing state before opening the new frame.

Registers are refreshed from `-data-list-register-names` and
`-data-list-register-values` after each stop and frame selection. On x86-64
Linux, `rdi`, `rsi`, `rdx`, `rcx`, `r8`, and `r9` are labeled as System V ABI
`arg1` through `arg6`; no source-level argument names are invented. Changed
values are shown as old-to-new transitions. In the Registers pane, `j/k`
selects a register and `p` shows its raw, decimal, and hexadecimal values.

Address-only Stack frames remain selectable. Selecting one refreshes its PC,
Disassembly, and recoverable register values. Frames without source use `[A]`
and display their address. On a direct assembly `call`, `Enter` opens the call
target without executing it and adds that view to navigation history. Register
memory inspection with `a` remains future work.

The UI and GDB controller are separated in `src/main.c` and `src/gdb.c`.

## Stop-condition display

The always-visible `STOPS` line summarizes breakpoints (with a conditional
count), catchpoints, and watchpoints. Their IDs use the same notation
everywhere: `B#n`, `C#n`, and `W#n`. Source gutter markers and watched-variable tags use
those IDs, so an item can be matched with the detailed `L` list immediately.

Colors carry the same meaning throughout the UI: cyan for breakpoints, yellow
for catchpoints, conditional breakpoints, and search matches, green for watchpoints
and the current execution marker, red for errors, and dim text for disabled
items. When a watchpoint stops the program, its last old-to-new value is shown
in the summary and in the `L` list.
