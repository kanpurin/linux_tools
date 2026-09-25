# gd - 軽量GDBソースTUI

`gd`は、GDB/MIをバックエンドとして使用するC/ncurses製の軽量デバッガーフロントエンドです。
行デバッグ情報が利用できる場合はソースコードを表示し、利用できない場合は自動的に
逆アセンブル、レジスタ、アドレスのみのスタックフレームを表示します。

## 必要環境

- Linux
- GCCまたは他のC11対応コンパイラー
- GDB 10.2以降
- ncursesw開発ファイル

RHEL 9では、次のコマンドで必要なパッケージを導入できます。

```sh
dnf install gcc make gdb ncurses-devel
```

## ビルドと実行

```sh
make -C gd
./gd/gd ./program arg1 arg2

# 次の形式も使用できます
./gd/gd --args ./program arg1 arg2
```

Source ModeでソースとArgs/Localsの変数名を表示するには、対象プログラムを`-g`付きで
ビルドしてください。`-g`なしでビルドされたプログラムもAssembly Modeで解析できます。
デバッグ情報がない場合、変数名や引数名を推測することはありません。

## インストール

システム共通のコマンドとして永続配置する場合は、次のコマンドを実行します。

```sh
sudo make -C gd install
gd ./program arg1 arg2
```

現在の開発用VMでは、ソースを`/opt/gd`、実行ファイルを`/usr/local/bin/gd`に配置します。
デプロイ先に`/tmp`などの一時ディレクトリは使用しません。

## モードと基本操作

`gd`はデフォルトで**GDB操作モード**として起動します。`F2`でGDB操作モードと
読み取り専用の**VIM操作モード**を切り替えます。

Source Modeでは、`Tab`でSource、Variables、Stackの順にペインを移動します。
Assembly Modeでは、Disassembly、Registers、Stackの順に移動します。
`Shift-Tab`は逆方向です。現在の入力モード、`SRC`/`ASM`、選択中のペインは
画面上部に常時表示されます。`d`でSourceとDisassemblyを手動切り替えできます。

GDB操作モードで`?`を押すと、現在画面の上に一時的なHelpポップアップを表示します。
ショートカットはExecution、View / History、Stop conditions、Panes、Modes / Otherの
セクションごとに一覧できます。`Esc`、`?`、`q`で閉じます。
VIM操作モードの`?`は従来どおり後方検索です。常時表示していた画面下部のキー一覧は廃止し、
最下段には現在の状態、操作結果、エラーだけを表示します。

### VIM操作モード

VIM操作モードでは次の読み取り専用操作を使用できます。

- `h/j/k/l`、`w/b/e`、`0/^/$`
- `5j`などの回数指定
- `gg/G`、`H/M/L`、`{`/`}`
- `PageUp/PageDown`、`Ctrl-u/Ctrl-d`、`Ctrl-f/Ctrl-b`
- `zz/zt/zb`
- `%`による対応括弧移動
- `/`、`?`による検索と`n/N`による次・前候補移動
- `*`、`#`によるカーソル上の単語検索
- `gd`、`gD`による宣言位置への移動

`gd`は現在位置より前からローカル宣言を検索し、`gD`は現在ファイルの先頭から検索します。

### GDB操作モード

- `r`: プログラムを実行
- `n`: 次のソース行まで進む
- `s`: 関数内へステップ実行
- `c`: 実行を継続
- `f`: 現在の関数が戻るまで実行し、呼び出し元のSourceまたはAssemblyへ移動
- `b`: カーソル位置にBreakpointを設定・解除
- `B`: 条件付きBreakpointを設定
- `F`: 関数名からFunction Breakpointを設定
- `C`: Catchpointメニューを開く
- `w`: Watchpointを設定
- `p`: 式を評価
- `L`: 停止条件一覧を開く
- `O`: プログラム出力を開く
- `o`: ソースナビゲーションを開く
- `e`: 現在の実行位置へ戻る
- `?`: Helpポップアップを開く
- `h`: Helpを開く（`?`の別名）
- `q`: 終了

`r`はプログラムの実行前、または終了後のみ受け付けます。すでに実行中の場合は無視され、
停止中のプログラムを再開する場合は`c`を使用します。

## Variablesペイン

Variablesペインでは次の操作を使用できます。

- `j/k`: 変数を選択
- `Enter`: 構造体やポインタを1階層展開・折りたたみ
- `p`: 選択中の式を評価
- `a`: アドレスまたはポインタ情報を表示
- `E`: 選択中の変数やメンバの値を変更
- `w`: 選択中の式にWatchpointを設定
- `B`: 選択中の式を使って条件付きBreakpointを設定

変数は、GDBで現在選択されているStack frameごとに`Args`と`Locals`へ分離して表示します。
引数はそのフレームに対する`-stack-list-arguments`だけから取得し、ローカル変数は
`-stack-list-locals`だけから取得します。`-stack-list-variables`を不用意にマージせず、
同じMIレコードが複数回返った場合もグループ内で重複を除去します。

### アドレス・ポインタ確認

scalar変数または構造体メンバで`a`を押すと、式、型、値、格納先アドレスを表示します。
ポインタでは、ポインタが保持するアドレスと、GDBによるdereference結果を表示します。

ポインタ値は、現在の引数、ローカル変数、Variablesペインですでに展開されているメンバの
アドレスと比較され、一致した式を表示します。照合のためだけに未展開の構造体を自動展開
することはありません。

- NULLポインタはdereferenceしません。
- 読み取れないメモリーは`<unavailable>`と表示します。
- `<optimized out>`の値はアドレス照合しません。
- Cのポインタ演算はTUI側で再実装せず、GDBに評価させます。

### 実行中プロセスの値変更

停止中に`E`を押すと、選択中の引数、ローカル変数、展開済み構造体メンバ、ポインタ、
またはレジスタを変更できます。新しい値はGDB式として入力し、変更前後の値を確認してから
`y`で確定します。

代入にはGDBの`-data-evaluate-expression`を使用し、TUI側でCの値を解析しません。
変更後はVariables、展開中の子要素、Registersを即座に更新します。GDBが拒否した代入や
optimized outされた値は変更しません。この操作で変更されるのはデバッグ対象プロセスだけで、
ソースファイルは変更されません。

## 強制return

`R`を押すと、現在選択中のフレームに対するForce Return画面を開きます。
型情報が利用できる場合は戻り値型を表示し、`void`関数では値入力欄を省略します。
再確認後にGDBの`return`を実行し、現在フレーム、Source/Disassembly、Variables、
Registers、Stack、実行位置を更新します。

戻り値型のデバッグ情報がない場合は`<unknown>`と表示します。x86-64 System V環境に限り、
型情報不足によりGDBが`return VALUE`を適用できない場合は、値なしでフレームをreturnした後、
明示的に入力された整数またはポインタ値を`rax`へ設定します。浮動小数点や構造体の
戻り値規約は推測しません。

## Stackペインと複数ファイル

Stackペインでは`j/k`で任意のフレームを選択し、`Enter`でそのフレームをGDB側でも
選択します。選択後は、そのフレーム固有のVariablesを更新してソース位置を開きます。

別ファイルの関数へstepした場合は、Sourceを自動的に切り替えます。`[P]`はプロジェクト内、
`[X]`は外部ライブラリ、`[A]`はソース情報のないアドレスのみのフレームを表します。

`o`はソースナビゲーションを開きます。Function検索は`F`と同じGDBシンボル検索を使用しますが、
Breakpointを設定せず関数を表示します。行情報がある関数はSource、ソース位置がない関数は
Disassemblyで開きます。

File検索は、推定したプロジェクトルート以下のC/C++ソースとヘッダーを再帰的に検索します。
GDBが行デバッグ情報を報告していないファイルは`REFERENCE ONLY`と表示します。そのような
ファイルでは、ソース行を命令へ安全に対応付けられないため`b`と`B`を拒否します。

実行位置と閲覧位置は独立しています。`=>`は実行位置、`>`は閲覧カーソルです。
SourceとDisassemblyの見出しには`[EXEC]`または`[VIEW]`を表示します。`e`を押すと、
デバッガーの状態を変更せず現在の実行位置へ戻ります。

Open、step、Stack選択、Breakpoint一覧からのジャンプ、assemblyのcall先閲覧で移動した位置は、
最大64件の履歴に保存されます。`[`または`Ctrl-o`で戻り、`]`で進みます。端末では
`Ctrl-i`と`Tab`が同じバイトとして扱われるため、進む操作には`]`を使用し、`Tab`は
ペイン移動のまま維持しています。

## Assembly Mode

選択中のフレームに利用可能な`fullname`と行番号がない場合、`gd`は自動的にAssembly Modeへ
切り替わります。GDBがソース位置を返しても、実際のソースファイルを読み込めない場合は、
古いSourceを残さずAssembly Modeへフォールバックします。

Disassemblyペインには、選択中フレームの関数またはPC周辺の命令を表示します。
現在の命令は`=>`で示し、`call`命令とそのシンボルを強調表示します。

- `j/k`: 命令を選択
- `b`: 選択中の命令アドレスにBreakpointを設定・解除
- `i`: `-exec-step-instruction`で1命令進める
- `I`: `-exec-next-instruction`でcallを飛び越す
- `Enter`: 直接callの呼び出し先を実行せずに開く

停止イベントを受けるたび、現在フレームに`fullname`と正の行番号があるかを確認します。
存在すればSource Mode、存在しなければAssembly Modeへ切り替えます。GDBの`step-mode`を
有効にしているため、`s`で行情報のない関数へ入った場合も関数入口で停止できます。
行情報のあるフレームへ戻るとSource Modeへ自動復帰します。

## Function Breakpoint

`F`でFunction Breakpoint画面を開きます。候補はGDBの
`-symbol-info-functions --include-nondebug`から取得し、入力するたびに絞り込みます。
`j/k`または上下キーで候補を選択し、`Enter`でBreakpointを設定します。

GDBが情報を提供できる場合は、関数名に加えてモジュール名とアドレスを表示します。
実行ファイル、デバッグシンボル、ロード済み共有ライブラリの関数を対象とし、独自の
ELF解析やソースレベルの名前推測は行いません。完全な関数名を入力して`Enter`を押した場合も、
GDBへ直接Breakpoint設定を試みます。

Function Breakpointは停止条件一覧に`function`として表示され、`d`で削除、`e`で有効・無効を
切り替えられます。`Enter`を押すと、行情報がある場合はソースへ、アドレスのみの場合は
関数先頭のDisassemblyへ移動します。未ロード共有ライブラリなどのpending Breakpointは
`[pending]`と表示します。

## Catchpoint

`C`でCatch Eventメニューを開き、次のイベントにCatchpointを設定できます。

- Syscall
- Signal
- Fork
- Vfork
- Exec
- Shared library load

syscall名とsignal名の候補は、GDBの`complete catch syscall`および
`complete catch signal`から取得します。アーキテクチャ固有のsyscall番号表を`gd`側で
管理することはありません。

Catchpointは停止条件一覧に`C#n`として表示され、他の停止条件と同様に`d`で削除、
`e`で有効・無効を切り替えられます。停止時にはイベント種別、対象、Entry/Return、
スレッド情報を保持し、Source/Disassembly、Registers、Stackを更新します。

syscallはEntryとReturnを区別します。x86-64では、Entry時に`orig_rax`をsyscall番号として、
Return時に`rax`を戻り値として表示します。`rdi`、`rsi`、`rdx`、`r10`、`r8`、`r9`は
syscall引数1〜6として表示します。

Exec Catchpointで停止した場合は、古いソース閲覧状態を破棄してから新しいフレームの
SourceまたはDisassemblyを開きます。

## Registersペイン

停止後またはフレーム選択後に、`-data-list-register-names`と
`-data-list-register-values`でRegistersを更新します。

x86-64 Linuxでは、通常の関数呼び出し時に`rdi`、`rsi`、`rdx`、`rcx`、`r8`、`r9`を
System V ABIの`arg1`〜`arg6`として表示します。ソースレベルの引数名は推測しません。
変化したレジスタは変更前から変更後への形式で表示します。

Registersペインでは`j/k`でレジスタを選択し、`p`でraw値、10進数、16進数を確認できます。
`E`で選択中のレジスタ値を変更できます。

ソースのないStack frameも選択可能で、選択するとPC、Disassembly、取得可能なRegistersを
更新します。レジスタが指すメモリーを`a`で調べる機能は今後の拡張対象です。

## 停止条件表示

常時表示される`STOPS`行には、Breakpoint、Catchpoint、Watchpointの件数を表示します。
条件付きBreakpointの件数もBreakpoint内に表示します。ID表記は画面全体で統一しています。

- `B#n`: 通常または条件付きBreakpoint
- `C#n`: Catchpoint
- `W#n`: Watchpoint

SourceのガターやVariables上のWatchpointタグにも同じIDを使用するため、`L`の詳細一覧と
すぐに対応付けられます。

色の意味も画面全体で統一しています。

- シアン: Breakpoint
- 黄色: Catchpoint、条件付きBreakpoint、検索一致
- 緑: Watchpoint、現在の実行位置
- 赤: エラー
- 暗色: 無効化中の項目

Watchpointで停止した場合は、直前の値から新しい値への変化を`STOPS`行と詳細一覧に表示します。

## 実装構成

- `src/main.c`: ncurses UI、入力処理、画面遷移
- `src/gdb.c`: GDB/MI制御、レスポンス解析、状態更新
- `src/gdb.h`: GDBコントローラーの公開データ構造とAPI
- `tests/`: fixtureとGDB/MI回帰テスト

TUI側でデバッガー機能を再実装せず、式評価、ポインタ操作、シンボル検索、Breakpoint、
Watchpoint、Catchpoint、実行制御は可能な限りGDBへ任せています。
