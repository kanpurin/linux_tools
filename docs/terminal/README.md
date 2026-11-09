# Tera Term + tmux 設定記録

2026-11-09時点。WindowsのTera Term 5.6.1からUbuntu 22.04上のtmux 3.2aを使用する環境で確認。
目的はプレフィックスなしのCtrl＋矢印によるペイン移動と、操作・表示・コピーの改善。

## 収録ファイル

| ファイル | 内容 |
|---|---|
| [tmux.conf](tmux.conf) | VMの `/root/.tmux.conf` の実ファイルのコピー |
| [KEYBOARD.CNF](KEYBOARD.CNF) | Windows側で修正済みのキーマップ全体（UTF-8で収録） |
| [bashrc-flow-control.sh](bashrc-flow-control.sh) | `.bashrc` に追加したフロー制御対策。追記用の抜粋 |
| [teraterm-settings.ini.example](teraterm-settings.ini.example) | INIに反映する設定項目だけの参考例。全体の置き換え用ではない |
| [copy-tmux-buffer.ps1](../../copy-tmux-buffer.ps1) | 長文コピーの任意の補助スクリプト |

実環境の `.bashrc` や `TERATERM.INI` 全体、秘密鍵、パスワード、接続履歴は収録していない。

## 変更前のバックアップ

既存ファイルを無条件に上書きしない。別環境では各ファイルの差分を確認して必要な設定を取り込む。

Linux:

```bash
stamp=$(date +%Y%m%d-%H%M%S)
cp -p ~/.tmux.conf ~/.tmux.conf.bak-$stamp
cp -p ~/.bashrc ~/.bashrc.bak-$stamp
```

Windows PowerShell（今回のインストーラー版Tera Term 5の保存先）:

```powershell
$stamp = Get-Date -Format yyyyMMdd-HHmmss
$settingsDir = Join-Path $env:APPDATA teraterm5
Copy-Item -LiteralPath "$settingsDir\KEYBOARD.CNF" -Destination "$settingsDir\KEYBOARD.CNF.bak-$stamp"
Copy-Item -LiteralPath "$settingsDir\TERATERM.INI" -Destination "$settingsDir\TERATERM.INI.bak-$stamp"
```

保存先はTera Termの［設定］→［設定フォルダー］（Setup directory）で確認できる。ポータブル版や起動引数で指定したファイルでは異なる。

## Ctrl＋矢印でペイン移動

tmux側には4方向の `bind -n C-Left/Right/Up/Down select-pane ...` がすでに登録されていた。
一方、Tera TermのキーマップではCtrl＋左右に送信設定がなく、Ctrl＋上下はローカル画面のスクロールに割り当てられていた。

`KEYBOARD.CNF` の既存の `[Shortcut keys]` 内で以下の2項目を変更する。

```ini
LineUp=off
LineDown=off
```

既存の `[User keys]` 内に以下を追加する。すでにUserキーがある場合は連番を調整し、キーコードの重複を避ける。

```ini
User1=1355,0,$1B[1;5D
User2=1357,0,$1B[1;5C
User3=1352,0,$1B[1;5A
User4=1360,0,$1B[1;5B
```

順にCtrl＋左・右・上・下。Ctrl＋上下でのTera Termローカルスクロールは解除される。
［設定］→［キーマップの読み込み］（Load key map）で修正したファイルを読み込む。
再起動後は標準の `KEYBOARD.CNF` が読み込まれる（別ファイルを起動引数で指定している場合を除く）。

今回の実行ファイルは `C:\Program Files\teraterm5\ttermpro.exe`。
`KEYCODE.EXE` も同じフォルダーの `keycode.exe` として存在した。見つからない場合は、実際のインストール先を確認する。

## tmuxの主な操作・表示

`Ctrl+B → キー` はCtrl+Bを押して離し、次のキーを押す操作。

| 操作 | キー |
|---|---|
| ペイン移動 | Ctrl＋矢印 |
| Vim風ペイン移動 | Ctrl+B → h / j / k / l |
| 左右分割 | Ctrl+B → \| |
| 上下分割 | Ctrl+B → - |
| 最大化・元に戻す（標準機能） | Ctrl+B → z |
| 設定の再読み込み | Ctrl+B → r |
| コピーモード | Ctrl+B → [ |
| コピーモードで選択開始／コピーして終了 | v / y |
| コピーモード終了 | q |
| tmux内のコピーを貼り付け | Ctrl+B → ] |

マウス有効、履歴50,000行、番号は1から開始、ウィンドウ削除時に番号を詰める。
端末種別は `screen-256color`、Esc判定は10ms、コピーモードはvi操作。
ステータスバーは下部、2秒更新。現在のウィンドウは黒文字・緑背景・太字。
ペイン境界は通常グレー、アクティブは水色。
`pane-border-lines simple` でTera Termの境界線表示崩れを解消した。
独自の右クリックメニュー、ポップアップシェル、ペイン見出しは現在の設定には含まれない。

## Tera Termで手動設定した項目

### 文字幅

Ambiguous Characters widthを **1 Cell** に変更し、右クリックメニューの枠線・文字位置の崩れが解消した。
この値は維持し、日本語表示やVimに問題が出た場合のみ再検討する。

**注意：調査時の保存済みINIは `UnicodeAmbiguousWidth=2` のままだった。**
実行中の値と保存済み値が一致していない可能性がある。
画面で1 Cellを確認してから［設定］→［設定の保存］で保存する。
この資料を作成する際に、実行中の文字幅や保存済みの2を勝手に変更してはいない。

### Windowsクリップボード連携

［設定］→［その他の設定］→［制御シーケンス］で、
「リモートからのクリップボードアクセス」を **書き込みのみ（write only）** にする。
これによりSSH先からWindowsクリップボードへコピーできる。読み取り許可は不要。
保存済みINIにも `ClipboardAccessFromRemote=write` を反映済み。

今回のtmuxは `set-clipboard external` が有効で、接続端末は `xterm`、
`tmux info` の `Ms`（OSC 52送信機能）も存在したため、tmux設定の追記は不要だった。

1. Ctrl+B → `[` でコピーモードに入る。
2. 過去の出力へ移動し、`v` で選択開始。
3. 矢印やPageUp/PageDownで選択範囲を広げ、`y` でコピー。
4. WindowsアプリにCtrl+Vで貼り付ける。

ユーザーによる実操作で、補助スクリプトなしのコピー成功を確認済み。
自動テストでは短文のtmux→Windows転送が完全一致した一方、
約41KB・日本語1,000行のOSC 52直接送信は約2,622文字となり完全一致しなかった。
この結果だけから、普段の選択コピーの固定上限や原因は断定していない。
長文では末尾まで貼り付けられるか確認する。

### 長文が欠ける場合の任意の補助手段

tmuxで `y` により選択内容をコピーした後、Windows PowerShellで実行する。

```powershell
& "C:\path\to\linux_tools\copy-tmux-buffer.ps1"
```

Windows標準の `ssh.exe` で `virtualbox-linux` に接続し、`tmux save-buffer -` の内容を
Windowsクリップボードに設定する。現在の内容は置き換わる。
SSHのホスト別名 `virtualbox-linux` と鍵認証などの非対話認証が必要。
別名が異なる環境ではスクリプトの接続先を変更する。
追加パッケージは不要。この経路では日本語1,000行の一致を検証済み。
通常の `y` で範囲全体がコピーできれば、このスクリプトは不要。

## Ctrl＋Sでシェルが止まったようになる問題

両ペインで `ixon`、`stop = ^S`、`start = ^Q` が有効だった。
Ctrl＋Sで出力が停止すると、入力の表示や実行結果が見えなくなるが、tmuxのペイン移動は動く。
Ctrl＋Qで再開する。停止中に入力したコマンドが残る可能性があるため、復帰後はCtrl＋Cで入力行を取り消す。
Ctrl＋Fは通常bashのカーソル右移動で、この停止機能ではない。

再発防止として `.bashrc` の対話シェル判定より後に、次を追記済み。

```bash
if [ -t 0 ]; then
    stty -ixon
fi
```

既存のbashペインにも `stty -ixon` を適用し、停止した出力を再開した。
新しい対話bashでも適用される。tmux設定ではなくシェルの端末設定。

## 検証と復元

- Ctrl＋左右の移動：ユーザー実操作で成功。
- 4方向のキー認識：実際のtmux設定を読む一時サーバーとPTYで成功。
- `.bashrc`：`bash -n` 成功、既存両ペインの `-ixon` を確認。
- キーマップ：変更はスクロール解除2項目と追加4項目のみ、重複キーコードなし。
- クリップボード：上記の短文・長文・SSH経由の結果を確認。

復元時は対応するバックアップを元のパスへ戻す。
tmuxは `tmux source-file ~/.tmux.conf`、Tera Termのキー設定はLoad key mapで再読み込みする。
文字幅などINI項目は、実行中の設定を確認してから保存・再起動する。
フロー制御対策を戻す場合は追加ブロックを除去し、必要な端末で `stty ixon` を実行する。

今回のバックアップ名:

- `KEYBOARD.CNF.bak-20261109-213812`
- `TERATERM.INI.bak-clipboard-20261109-222007`
- `/root/.bashrc.bak-flow-control-20261109T124326Z`

`.tmux.conf` 自体は今回変更していない。

## 参考資料

- [Tera Termキーマップ仕様](https://teratermproject.github.io/manual/5/en/setup/keyboard.html)
- [Tera Term設定フォルダー](https://teratermproject.github.io/manual/5/en/setup/folder.html)
- [Tera Term制御シーケンス設定](https://teratermproject.github.io/manual/5/en/menu/setup-additional-ctrlseq.html)
- [tmux公式クリップボード解説](https://github.com/tmux/tmux/wiki/Clipboard)
