# linux_tui

Linux 向け TUI ツール集です。

## Tools

- `procview/` - C/ncurses 製プロセスビューアー
- `testforge/` - C/ncurses 製 TestForge、負荷注入・試験シナリオ支援ツール
- `gd/` - GDB/MIをバックエンドにした軽量ソースデバッグTUI

## Build

```bash
make
```

個別にビルドする場合:

```bash
make -C procview
make -C testforge
make -C gd
```

## Run

```bash
./procview/procview
./testforge/testforge
./gd/gd ./program arg1 arg2
```

To install `gd` as a persistent system command:

```bash
sudo make -C gd install
gd ./program arg1 arg2
```

## AutoTest Script Builder

```bash
make -C autotest-assist-tui
./autotest-assist-tui/autotest-builder
```
