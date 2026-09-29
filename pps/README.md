# pps

`pps` is a Linux-only process selector: a selectable `ps aux | less` that removes the need to look up and copy a PID before acting on a process.

It is implemented in C11 using `/proc`, `termios`, and ANSI terminal sequences. It has no runtime dependency on `fzf`, `ncurses`, `lsof`, `pstree`, `htop`, or procps commands.

## Install

Copy the complete `pps` directory to the target Linux machine, then run:

```bash
cd pps
make
```

That one command:

1. builds the C core;
2. installs the runtime under `~/.local/lib/pps`;
3. installs the fallback launcher as `~/.local/bin/pps`.

`make` never modifies shell startup files. To add the Bash integration to `~/.bashrc`, explicitly run:

```bash
make install-shell
source ~/.bashrc
```

`make install-shell` is idempotent and adds the integration line only once. Alternatively, manage the following line yourself:

```bash
source "$HOME/.local/lib/pps/shell/pps.bash"
```

The locations can be overridden when needed:

```bash
make PREFIX=/opt/pps-prefix BASHRC=/path/to/bashrc
```

Use `make build` when only a local build is wanted and no files outside the source tree should be changed.

To remove the installed runtime and launcher without touching `.bashrc`:

```bash
make uninstall
```

`pps` intentionally requires the Bash integration. Running `bin/pps` directly explains that the integration is missing. Other shells are unsupported.

## Use

```bash
pps                         # select from all processes
pps server                  # search comm, cmdline, and exe
pps 1234                    # process details
pps 1234 --fd
pps 1234 --threads          # -t is also accepted
pps 1234 --tree
pps 1234 --limits
pps 1234 --term
pps 1234 --kill
pps 1234 --stop
pps 1234 --cont
pps 1234 --signal SIGUSR1
```

A name with one match is handled immediately. Multiple matches open Process Select. Enter returns an editable `pps PID ...` command to Bash without executing it; `P` instead stores the PID in the non-exported shell variable `PID`.

## Process Select keys

| Key | Action |
|---|---|
| `j`, `Down` / `k`, `Up` | move one line |
| `Space`, `f`, `PgDn` / `b`, `PgUp` | move one page |
| `d` / `u` | move half a page |
| `g` / `G` | first / last process |
| `Left` / `Right` | horizontal scroll |
| `/`, `n`, `N` | search, next, previous |
| `o` | sort by PID, CPU, memory, start, or CPU time |
| `r` | rescan `/proc` |
| `s` | send a signal immediately |
| `Enter` | return an editable `pps PID ...` command |
| `P` | set Bash variable `PID` and return |
| `h`, `?` | help |
| `q`, `Esc` | cancel |

TUI output goes directly to `/dev/tty`; stdout is reserved for the small internal protocol used by `pps.bash`. Terminal state is restored on normal exit, cancellation, Ctrl-C, and termination signals.

## Test

```bash
make test
```

The automated suite covers non-interactive process views, not-found behavior, and launcher behavior. Interactive key handling should be checked in a real terminal after sourcing `shell/pps.bash`.
