# linux_tools

Linux 向け TUI ツール集です。

## Tools

- `procview/` - C/ncurses 製プロセスビューアー
- `testforge/` - C/ncurses 製 TestForge、負荷注入・試験シナリオ支援ツール
- `gd/` - GDB/MIをバックエンドにした軽量ソースデバッグTUI
- `ltree/` - ディレクトリ構成を木形式で表示する軽量 `ltree` コマンド
- `strace-src/` - straceイベントと発生元ソースを同期表示する2ペインTUI
- `lifewatch/` - プロセス・ファイル監視、SSH端末通知、操作元プロセス追跡
- `pps/` - `/proc` を直接読む、選択可能な `ps aux | less` 風プロセスピッカー

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
make -C strace-src
make -C lifewatch
make -C pps
```

## Run

```bash
./procview/procview
./testforge/testforge
./gd/gd ./program arg1 arg2
./ltree/ltree [directory]
./strace-src/strace-src ./program arg1 arg2
sudo ./lifewatch/lifewatch
source ./pps/shell/pps.bash
```

To install `gd` as a persistent system command:

```bash
sudo make -C gd install
gd ./program arg1 arg2

sudo make -C ltree install
ltree .

sudo make -C strace-src install
strace-src ./program arg1 arg2
```

## AutoTest Script Builder

```bash
make -C autotest-assist-tui
./autotest-assist-tui/autotest-builder
```
