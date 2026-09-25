## Shyake

[English](../../README.md) | [简体中文](./README.zh-CN.md) | 日本語

> Translated by Claude Fable 5

### 概要

Shyake は**耐量子暗号**（ポスト量子暗号）を用いた**エンドツーエンド暗号化メールシステム**であり、検閲や監視に対抗するための分散型通信手段として設計されています。

サーバーは Cloudflare Workers 上で動作するため、誰でも無料で自分のインスタンスをホストできます。Cloudflare グローバルネットワークの代わりに、自分のハードウェア上でサーバーをセルフホストすることも可能です。

### ドキュメント

自分のインスタンスをデプロイするには：

* [**デプロイガイド**](./ja/DEPLOY.md)

開発者向け：

* [**開発者ガイド**](./ja/DEV.md)
* [**技術仕様**](./ja/SPEC.md)

### インストール

[GitHub Releases](https://github.com/salmonization/shyake/releases)
からバイナリをダウンロードして展開し、`$PATH` の通ったディレクトリにコピーします：

```sh
sudo cp ./shyake /usr/local/bin/
```

正常に動作するか確認します：

```sh
shyake version
```

インストール後は `shyake update` でその場でアップグレードできます（後述の update コマンドを参照）。

### GUI

実験的なローカル Web GUI が [`webui/`](../../webui) にあります。[Bun](https://bun.sh) 上で動作し、FFI 経由で `libshyake` を呼び出します：

```sh
cd webui && bun run start
```

詳しくは [webui/README.md](../../webui/README.md) を参照してください。

### 使い方

**初回利用**：

```sh
# ローカル設定を初期化し、鍵ペアを生成する
shyake init

# インスタンスに登録する（-u はユーザー名、-i はインスタンス URL）
shyake register -u salmon -i https://shyake.eee.coffee
```

`init` では、秘密鍵を保護するパスフレーズの設定を求められます（空のままにするとパスフレーズなしになります）。鍵を使用するコマンドの実行時に、このパスフレーズの入力が求められます。

設定ファイルのディレクトリは、デフォルトでは `~/.config/shyake/` です。

初期化時にディレクトリを指定することで、複数のプロファイルを作成できます：

```sh
shyake init -c path/to/your/dir
```

その場合、このプロファイルを使用する際は常に `-c` オプションを付ける必要があります。

`whoami` コマンドでプロファイルを確認できます。

```sh
shyake whoami
```

`shyake man` で全コマンドの一覧を、`shyake man <command>` で各コマンドの詳細な使い方を確認できます。

**check コマンド**：

`check` コマンドで受信箱と送信済みメールを一覧表示できます。

```sh
shyake check inbox
shyake check sent
```

`--csv` や `--json` を使うと、機械処理向けに出力をフォーマットできます。`--no-header` で列ヘッダーを無効化したり、`--count` で件数のみを表示したりすることもできます。

メールのヘッダーを確認するには：

```sh
shyake check fQBjZnvJ56
```

ローカルに保存したメールの一覧表示（後述の save コマンドを参照）や、保存済みメールのヘッダー確認には：

```sh
shyake check saved
shyake check saved fQBjZnvJ56
```

ローカルの暗号化下書きの一覧表示（後述の compose コマンドを参照）や、下書きのヘッダー確認には：

```sh
shyake check drafts
shyake check drafts 3
```

**send コマンド**：

```sh
shyake send -s "This is the subject" -t flat_white < body.txt
```

`-s` を省略した場合、入力ファイルの 1 行目が件名になります。

```sh
shyake send -t flat_white < content.txt
```

件名の長さは 128 バイトを超えてはなりません。

外部インスタンスのユーザーに送るには、宛先に
`username@instance` を使用します。

```sh
shyake send -s "Hello" -t flat_white@shyake.example.com < body.txt
```

ヒアドキュメントも使えますが、シェルの履歴には注意してください。

```sh
shyake send -s "This is the subject" -t flat_white <<EOF
Hello, this is the mail body.
EOF
```

Shyake が送信できるのはテキストのみです。バイナリデータは送信前に
base64 エンコードする必要があります。

```sh
# 貧しい画像を送る
base64 < image.png | shyake send -t flat_white -s "image.png"

# 小さなアーカイブを送る
tar czf - ./source | base64 | shyake send -t flat_white -s "source.tar.gz"
```

**compose コマンド**：

`compose` はメール草稿の作成用コマンドで、個人的な日記などの用途にも使えます。指定したエディタ（デフォルトは `ed`）でシンプルなテンプレートを開き、結果を ML-KEM-768 + ChaCha20-Poly1305 で自分自身の鍵に暗号化して `~/.config/shyake/drafts/` に下書きとして保存します。

```sh
shyake compose
```

```
To: flat_white
Subject: Coffee tomorrow?
---
The mail body goes here.
```

宛先・件名・本文はすべて暗号化された状態でディスクに置かれます。下書きの保存にパスフレーズは不要です（公開鍵のみを使用）。一覧表示や閲覧には秘密鍵のアンロックが必要です。

`To:` は空欄のままでもかまいません。`check drafts` では `(null)` と表示されます：

```sh
shyake compose
shyake check drafts       # エントリの一覧
shyake read drafts 3      # エントリ 3 を読む
shyake compose 3          # エントリ 3 の続きを書く
```

下書きを送信するには `send --draft` を使います。宛先と件名は下書きから取られ（`-t`/`-s` で上書き可能）、送信に成功すると下書きは自動的に削除されます：

```sh
shyake send --draft 3
shyake send --draft 3 -t flat_white
```

エディタのデフォルトは `ed` です。設定ファイルの `EDITOR` キー、または環境変数 `$VISUAL`/`$EDITOR` で別のエディタを指定できます。`vim` または `nvim` を使う場合は、平文がスワップファイルに漏れないよう `-n -i NONE` 付きで起動されます。下書きはただのローカルファイルなので、削除したい場合は `~/.config/shyake/drafts/<id>.json` を直接削除してください。

> かつての `ed` エディタ（および初期の `ex`/`vi`）には `crypt(1)` ベースの暗号化機能が組み込まれていました。マルチユーザーのタイムシェアリングシステムでは `root` が任意のファイルを読めたため、基本的なプライバシー保護や機密データの保護を目的とした機能で、日記や私信、パスワードの管理、未公開のコードや設計草稿などによく使われていました。しかし暗号方式がとうに破られているため、現在ではほぼすべての Unix および Unix 系 OS の ed からこの機能は取り除かれています（NetBSD の `ed` を除く）。Shyake の `compose` は `ed -x` へのオマージュです。

**fetch コマンド**：

メールを取得して復号します。

```sh
shyake fetch fQBjZnvJ56
```

メールをヘッダー付きのプレーンテキストとしてエクスポートするには（暗号化されたままのメールを保存する後述の `save` コマンドと混同しないでください）：

```sh
shyake fetch fQBjZnvJ56 --no-color > exported-mail.txt
```

本文のみを出力するには、`-r` または `--raw` を使用します。

```sh
shyake fetch fQBjZnvJ56 --raw
shyake fetch fQBjZnvJ56 -r > exported-mail.txt
```

受信した base64 エンコード済みバイナリデータをデコードするには：

```sh
# 受信した画像をデコードする
shyake fetch fQBjZnvJ56 -r | base64 -d > image.png

# 受信したアーカイブをデコードして展開する
mkdir -p ./output
shyake fetch fQBjZnvJ56 -r | base64 -d | tar xzf - -C ./output
```

**save と read コマンド**：

`save` はサーバーから暗号化されたメールを取得し、
`~/.config/shyake/saved/<id>.json` に保存します。この段階ではメールは復号されません。

```sh
shyake save fQBjZnvJ56
```

`read` は保存済みメールを復号して表示します。出力は `fetch` と同一で、`-r`/`--raw` もサポートされています。

```sh
shyake read fQBjZnvJ56
```

`read drafts <id>` はローカルの下書きに対して同じことを行います。

```sh
shyake read drafts 3
```

要するに、`read` はローカル、`fetch` はリモートを担当します。

**fingerprint コマンド**：

自分のフィンガープリントを表示するには：

```sh
shyake fingerprint
```

通信相手のフィンガープリントを確認するには：

```sh
shyake fingerprint flat_white
```

通信相手が鍵ペアをローテーションした場合、その相手のフィンガープリントを更新できます。**警告：update コマンドを実行する前に、必ず信頼できる別の帯域外チャネル（対面や別のプラットフォームなど）で新しいフィンガープリントを検証し、なりすましを防いでください。**

```sh
shyake fingerprint flat_white --update
```

**burn コマンド**：

メールを削除します。

```sh
shyake burn fQBjZnvJ56
```

**block と unblock コマンド**：

ユーザーまたはインスタンスをブロック/ブロック解除します。対象にはユーザー名またはインスタンス URL を指定できます。

```sh
shyake block flat_white
shyake block bad.example.com
shyake unblock flat_white
```

`blocklist` で現在のブロック一覧を確認できます：

```sh
shyake blocklist
```

**update コマンド**：

`shyake update` はインストール済みバージョンと利用可能なバージョンを表示します。バージョンの照会は自分のインスタンス経由で行われます（インスタンスが GitHub Releases API を中継します）。`shyake.eee.coffee` は組み込みのフォールバックにすぎず、インスタンスが未設定の場合にのみ使用されます。`stable` または `preview` を指定すると、そのチャネルの最新リリースをインストールします（preview は
stable より新しい場合にのみ提供されます）。

```sh
shyake update
shyake update stable
shyake update preview
```

**rotate コマンド**：

鍵ペアをローテーションし、自分宛て・自分発のメールをすべて消去します。

```sh
shyake rotate
```

**destroy コマンド**：

ローカルの設定と鍵ペアを削除し、インスタンス上のアカウントも抹消します。自分宛て・自分発のメールはすべて消去されます。ユーザー名は永久にロックされ、このインスタンスで再登録することはできません。

```sh
shyake destroy
```

### 高度な使い方

`--no-color` でカラー出力を無効にできます。標準の `NO_COLOR`
環境変数にも対応しています。

```sh
shyake check inbox --no-color
shyake check fQBjZnvJ56 --no-color
shyake fetch fQBjZnvJ56 --no-color
```

`check` と `fetch` コマンドでは、`--plain` を使うとページャー、カラー、切り詰めを無効化できます。

設定ファイル（`~/.config/shyake/config`、または他のプロファイルのディレクトリ内）を編集して、セットアップを最適化できます。たとえば `check` コマンドの列レイアウトを変更できます。

```sh
# Date & Time format (strftime format)
# RECENT: less than 180 days old.
# ISO 8601 format
TIME_FORMAT="%Y-%m-%d %H:%M"
# POSIX format
# TIME_FORMAT="%b %d  %Y"
# TIME_FORMAT_RECENT="%b %d %H:%M"

# Time zone (default: auto)
# Integer offset in hours: 0=UTC, 8=UTC+8, -6=UTC-6
TIME_ZONE=auto

# Display columns for `check` command
CHECK_COLUMNS=id,sender,subject,size,date

# Disable colors (1 = disable)
# NO_COLOR=0

# Editor for 'shyake compose' (default: ed)
# Falls back to $VISUAL, then $EDITOR
# EDITOR="ed"

# Default action when running without arguments
# 0 = man, 1 = check inbox, 2 = check inbox --count
DEFAULT_ACTION=0
```

`enc` と `dec` を使うと、単体のファイルを
ML-KEM-768 + ChaCha20-Poly1305 で暗号化・復号できます。これらのコマンドはデバッグ/テスト用途を想定しています。

```sh
# 自分の公開鍵で暗号化する（出力はデフォルトで <file>.enc）
shyake enc secret.txt

# 宛先を指定して暗号化し、出力パスをカスタマイズする
shyake enc secret.txt -t flat_white -o secret.enc

# KEM 秘密鍵で復号する（-o 未指定時は stdout に出力）
shyake dec secret.enc -o secret.txt
```

`--debug` を使うと、詳細な `curl` ログ（ハンドシェイク、HTTP
ヘッダー、内部変数）が `stderr` に出力されます。

### ライセンス

BSD 2-Clause License
