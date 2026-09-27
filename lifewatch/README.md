# lifewatch

`lifewatch`は、Linux上のプロセスとファイルを監視する小さな常駐ツールです。
プロセス消失やファイル変更をSSH接続中の端末へ表示し、通知内容を専用ログへ保存します。

外部Webサービスやクラウド通信は行いません。

## 機能

- TUIで選択したプロセス個体のPIDと起動時刻による監視
- `pidfd`によるプロセス終了の即時検知
- inotifyによるファイルの編集・削除・移動・再作成検知
- Linux Auditによるシグナル送信元プロセスの特定
- fanotifyとproc connectorによるファイル操作元プロセスの特定
- SSH接続中の`/dev/pts/*`への短い通知
- `/var/log/lifewatch/notifications.log`への追記保存
- ncurses製の設定専用TUI

Linux Audit、fanotifyまたはproc connectorが利用できない環境でも監視は継続しますが、操作元プロセスは
`unknown`になります。

## ビルド

```sh
make
sudo make install
```

必要なものはCコンパイラとncurses開発ファイルです。Ubuntuでは通常
`build-essential`と`libncurses-dev`に含まれます。

## 設定

```sh
sudo lifewatch
```

TUI終了後に設定を必ず検証し、正常ならlifewatchサービスを起動します。すでに稼働中の場合は
新しい設定を再読み込みします。

TUIのキー:

- `↑` / `↓`, `j` / `k`: 監視対象を移動
- `PgUp` / `PgDn`, `g` / `G`: ページ移動、先頭・末尾へ移動
- `p`: 実行中のプロセス一覧から監視対象を選択
- `f`: ファイル監視を追加
- `d`: 選択中の監視を削除
- `Enter` / `e`: 選択中の監視を編集
- `t`: 選択中の監視を単発テスト
- `i`: 監視間隔を変更
- `n`: SSH端末通知の切り替え
- `b`: ターミナルベルの切り替え
- `v`: 通知する最低重要度を変更
- `l`: 通知ログの保存先を変更
- `L`: 日本語／英語表示を切り替え
- `N`: 通知言語を変更
- `s`: 保存
- `?`: キーボードヘルプ
- `q`: 終了

設定ファイルは依存ライブラリを必要としない行形式です。

```ini
interval_ms=500
notify_ssh=yes
terminal_bell=yes
min_level=warning
ui_language=auto
notification_language=auto
log_file=/var/log/lifewatch/notifications.log
process=payment-daemon|pid|1234@987654
file=payment-config|/etc/payment/config.yaml
```

プロセス設定の`PID@START_TICKS`はTUIが自動的に記録します。PIDが再利用されても別プロセスへ
追従せず、選択したプロセス個体だけを監視します。終了後に同名プロセスが存在しても消失状態のままです。

`ui_language`と`notification_language`には`auto`、`ja`、`en`を指定できます。
`auto`はUTF-8の日本語ロケールで日本語、それ以外では英語を使用します。設定画面は
`lifewatch setup --lang ja`のように表示言語を一時指定できます。

## 起動

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now lifewatch
```

設定変更後:

```sh
sudo systemctl reload lifewatch
```

## 確認

```sh
lifewatch validate
lifewatch check
sudo lifewatch events
sudo lifewatch show EVENT_ID
tail -f /var/log/lifewatch/notifications.log
```

`check`の終了コードは、すべて正常なら`0`、監視対象が欠落していれば`1`、設定エラーなら`2`です。

## 権限

Auditルールとfanotifyを利用し、SSHセッションのTTYへ書き込むため、デーモンはrootで動作します。
端末へ表示する文字列から制御文字を除去し、ログファイルはシンボリックリンクを拒否します。

## 制約

- カーネルやOOM Killerによる終了には、送信元プロセスが存在しません。
- Auditイベントを取得できない場合、シグナル送信元は特定できません。
- fanotifyを利用できない場合、ファイル操作元は特定できません。
- ホスト自体が停止した場合、同じホスト上の本ツールからは通知できません。
