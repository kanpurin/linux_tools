# linux_tools

Linux 向け TUI ツール集です。

## Tools

- `procview/` - C/ncurses 製プロセスビューアー
- `testforge/` - C/ncurses 製 TestForge、負荷注入・試験シナリオ支援ツール
- `gd/` - GDB/MIをバックエンドにした軽量ソースデバッグTUI
- `ltree/` - ディレクトリ構成を木形式で表示する軽量 `ltree` コマンド

## Build

```bash
make
```

個別にビルドする場合:

```bash
make -C procview
make -C testforge
make -C gd
make -C ltree
```

## Run

```bash
./procview/procview
./testforge/testforge
./gd/gd ./program arg1 arg2
./ltree/ltree [directory]
```

To install `gd` as a persistent system command:

```bash
sudo make -C gd install
gd ./program arg1 arg2

sudo make -C ltree install
ltree .
```

## AutoTest Script Builder

```bash
make -C autotest-assist-tui
./autotest-assist-tui/autotest-builder
```
