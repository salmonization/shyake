## Shyake デプロイガイド

[English](../../DEPLOY.md) | [简体中文](../zh-CN/DEPLOY.md) | 日本語

> Translated by Claude Fable 5

サーバーは D1 データベースを備えた Cloudflare Worker として動作します。ただし、自分のハードウェア上でセルフホストすることも可能です。

サーバーのデプロイ方法は 2 通りあります：

* Cloudflare を使用する
* セルフホスティング

**フェデレーション**

2 つのインスタンスは、双方が `FEDERATION_ENABLED = true` になっていると自動的にフェデレーションします。追加の設定は不要です。インスタンス間のメールはサーバー間で直接ルーティングされ、クライアントは常に自分のインスタンスとのみ通信します。

受信・送信フェデレーションを無効にするには：

```toml
FEDERATION_ENABLED = false
```

### Cloudflare を使用する

すべての操作は Wrangler CLI を使って自分のマシン上で完結します。このリポジトリを fork する必要も、Cloudflare に接続する必要も、ダッシュボードを操作する必要もありません。

前提条件：

- Node.js 18+
- Cloudflare アカウント

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/cf
./deploy.sh
```

`deploy.sh` はデプロイ全体を実行します：

1. Worker の依存関係をインストールする
2. 未認証であれば `npx wrangler login` を実行する
3. インスタンスのドメインを尋ねる
4. D1 データベースと KV キャッシュネームスペースを作成する（既存のものがあれば再利用する）
5. 得られたリソース id を `server/cf/wrangler.toml` に書き込む
6. データベースマイグレーションを適用する
7. Worker をデプロイし、`/health` を確認する

インスタンスのドメインはそのインスタンス上のすべてのアドレス（`user@your.domain.example`）に埋め込まれ、他のインスタンスはこれを使ってフェデレーションメールを送り返します。独自ドメインがない場合は、デフォルトの `*.workers.dev` の URL がそのまま使えます。

オプション：

| オプション | 効果 |
|---|---|
| `--domain <d>` | インスタンスのドメインを対話なしで指定する |
| `--update` | 最新のコードを取得してから再デプロイする |
| `--no-kv` | KV バージョンキャッシュを省略する |
| `--config-only` | `wrangler.toml` を生成して終了する |
| `--local` | 代わりにローカルのセルフホスティング用に設定する（後述） |

#### アップグレード

```sh
cd shyake/server/cf
./deploy.sh --update
```

最新のコードを取得し、新しいマイグレーションを適用して再デプロイします。既存のリソースは再利用され、設定もそのまま保たれます。

#### 設定の変更

`server/cf/wrangler.toml` は初回実行時に `wrangler.template.toml` から生成され、git の管理対象では**ありません**。そのためインスタンスの設定は `git pull` しても残り、競合することもありません。編集したら `./deploy.sh` を再実行してください：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example"
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB。786432（768 KiB）を超えないこと
```

スクリプトを再実行してもこれらの設定は上書きされません。まだ設定されていないリソース id を補うだけです。

`wrangler.toml` が生成式になる前にデプロイしたインスタンスの場合、`./deploy.sh --update` が設定を `wrangler.toml.bak` として退避し、取得後に復元します。

### セルフホスティング

セルフホスティングでは、Wrangler に同梱されているローカルの `workerd`
ランタイム上で、まったく同じ Worker コードを自分のマシンで動かします。D1（SQLite）と
KV は Wrangler 自身がローカルでエミュレートするため、**Cloudflare アカウントは不要**です。`wrangler login` も、ダッシュボードでのリソース作成も必要ありません。

前提条件：

- Node.js 18+
- 常時オンラインのマシン（Node.js が動作する OS なら何でも可。以下の例は
  systemd を備えた Linux を想定しています）
- フェデレーションに参加する場合：そのマシンを指す公開ドメイン名と、有効な
  TLS 証明書を持つリバースプロキシ（後述）

手順：

1. **セットアップ**します（fork は不要です）：

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/cf
./deploy.sh --local --domain your.domain.example
```

`--local` を付けると、Cloudflare アカウントを必要とする処理はすべて省略されます。`wrangler login` もリモートリソースの作成もありません。依存関係のインストール、`wrangler.toml` の生成、ローカル SQLite データベースの作成だけを行います。

`--domain` は、あなたのインスタンスに外部から到達できるドメインでなければなりません。この値はインスタンス上のすべてのアドレス（`user@your.domain.example`）に埋め込まれ、他のインスタンスもこれを使ってフェデレーションメールをあなたのインスタンスへルーティングします。省略した場合はスクリプトが尋ねます。

2. 必要であれば、生成された `server/cf/wrangler.toml` で**設定を調整**します。重要なのは `[vars]` セクションだけで、ローカルモードでは `database_id` と KV の `id` は無視されます：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example"
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB。786432（768 KiB）を超えないこと
```

このファイルは git の管理対象ではないため、編集内容は `git pull` しても残ります。以降のアップグレードは `./deploy.sh --update --local` で行います。

3. **サーバーを起動**します：

```sh
npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

`curl http://127.0.0.1:8787/health` で確認します。`200 OK`
が返れば、Worker とデータベースが正常に動作しています。

サーバーは `127.0.0.1` にバインドしたままにし、外部トラフィックはリバースプロキシに処理させます（次の手順）。`0.0.0.0`
へ直接バインドするのは、フェデレーションに参加しない信頼できる LAN 内でのみ妥当です。

4. **TLS 付きリバースプロキシを設定**します

この手順は**フェデレーションに必須**です。インスタンス同士は常に
`https://<domain>/...` で通信するため、あなたのインスタンスは
`https://your.domain.example`
で到達可能であり、他のインスタンスが受け入れる証明書を持っていなければなりません。自己署名証明書は使えません。インスタンスが私的なもの（ユーザー同士でのみメールをやり取りする）であれば、この手順を省略してクライアントに平文
HTTP で接続させることもできます。

[Caddy](https://caddyserver.com/) を使えば証明書の取得と更新は自動です。`Caddyfile`
全体は次のとおりです：

```
your.domain.example {
    reverse_proxy 127.0.0.1:8787
}
```

certbot で管理する証明書を使った nginx でも同様に動作します。`https://your.domain.example`
を `http://127.0.0.1:8787` へプロキシしてください。

5. **常時稼働させる**

`wrangler dev`
はフォアグラウンドプロセスです。ブート時の起動と障害時の再起動はスーパーバイザーに任せます。最小構成の
systemd ユニット（`/etc/systemd/system/shyake.service`）：

```ini
[Unit]
Description=Shyake server (local workerd)
After=network-online.target
Wants=network-online.target

[Service]
User=shyake
WorkingDirectory=/home/shyake/shyake/server/cf
ExecStart=/usr/bin/npx wrangler dev --local --ip 127.0.0.1 --port 8787
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now shyake
```

**データの場所とバックアップ**

すべてのローカル状態（D1 の SQLite データベースと KV キャッシュ）は
`server/cf/.wrangler/state/`
以下に保存されます。インスタンスのバックアップとは、このディレクトリのバックアップです（書き込み中のデータベースをコピーしないよう、先にサーバーを停止するか、SQLite
に安全なツールを使ってください）。このディレクトリを削除するとインスタンスは空のデータベースにリセットされます。`wrangler dev`
に `--persist-to <dir>` を渡せば、状態を別の場所に保存できます。

**注意事項：何を動かしているのかを理解する**

`wrangler dev` は Wrangler
の開発サーバーであり、堅牢化された本番サーバーではありません。Cloudflare
Workers を支えているのと同じ `workerd`
ランタイムを実行するため、個人や小規模コミュニティのインスタンスなら十分に持ちこたえますが、開発向けの挙動には注意が必要です：

- **ファイル監視 / ホットリロード**: ソースツリーを監視し、ファイルが変更されると
  Worker をリロードします。開発中は便利ですが、サーバー上では `server/cf/`
  内のファイル編集や `git pull` が即座にインスタンスの再起動を意味します。更新は慎重に：pull
  して、変更を確認してから、リロードさせる（または自分でサービスを再起動する）ようにしてください。
- **単一プロセスで、自前の監視機能なし**: クラスタリングも組み込みのクラッシュ復旧もありません。それを担うのが上記の
  systemd ユニットです。
- **レート制限や DDoS 防御なし**: Cloudflare
  上ではプラットフォームが提供します。セルフホストでインスタンスを公開する場合、レート制限を加える場所はリバースプロキシです。
- **対話的なキーバインド**: 端末に接続していると `wrangler dev` は stdin
  からホットキーを読み取ります。systemd 下では TTY
  がないため問題になりませんが、代わりに `tmux`
  で動かす場合は誤入力に注意してください（`x` はコンソールをクリアし、`Ctrl+C` は終了します）。

インスタンスがこの構成の限界を超えたら、スケールできるのは前述の
Cloudflare デプロイの方です。データベースは、ローカルの SQLite
ファイルをエクスポートして `wrangler d1 execute --remote`
でインポートすれば移行できます。
