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

前提条件：

- Node.js 18+
- Cloudflare アカウント

手順：

1. GitHub でこのリポジトリを **fork してクローン**します。

2. ターミナルで Cloudflare **Wrangler CLI** を**認証**します：

```sh
npx wrangler login
```

Wrangler が未インストールの場合、初回実行時に `npx` がインストールを促します。別途のインストール手順は不要です。

3. **D1 データベースを作成**します：

```sh
npx wrangler d1 create shyake-db
```

出力から `database_id` をコピーします。

4. **KV ネームスペースを作成**します（バージョン中継キャッシュ）：

```sh
npx wrangler kv namespace create VERSION_CACHE
```

出力から `id` をコピーします。各インスタンスは自身のクライアント向けに GitHub Releases API を中継して `shyake update`
を支えます。この KV ネームスペースはその照会結果を 1 時間キャッシュします。このバインディングは省略可能です。なくてもエンドポイントは動作しますが、リクエストごとに GitHub へアクセスします。

5. fork 内の **`server/wrangler.toml` を編集**します：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example" # ここを編集
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB；786432（768 KiB）を超えないこと

[[d1_databases]]
binding        = "DB"
database_name  = "shyake-db"
database_id    = "<your database_id>" # ここに database_id を貼り付け
migrations_dir = "migrations"

[[kv_namespaces]]
binding = "VERSION_CACHE"
id      = "<your kv namespace id>" # ここに KV ネームスペースの id を貼り付け
```

`[[d1_databases]]` ブロックは必ず存在し、正しい `database_id`
を含んでいる必要があります。これがないと Worker はデータベースバインディングを持たず、すべてのリクエストが失敗します。

カスタムドメインを持っていない場合は、デフォルトの
`*.workers.dev` URL を `INSTANCE_DOMAIN` として使用できます。

6. **データベースマイグレーションを適用**します（すべてのテーブルが作成されます）：

```sh
cd server
npx wrangler d1 migrations apply shyake-db --remote
```

Cloudflare の CI パイプラインはデータベースマイグレーションを自動では適用しません。`wrangler d1 migrations apply` を一度手動で実行する必要があります。これを省略するとデータベースが空のままになり、Worker はすべての API 呼び出しでエラーになります。

7. **デプロイ**

以下のいずれかを選択します：

**方法 A: ダッシュボード**：Cloudflare ダッシュボードで
`Compute → Workers & Pages → Create application → Continue with GitHub`
に進み（初回は `Add GitHub account` が必要な場合があります）、自分の fork を選択して次のように設定します：

| 項目 | 値 |
|-------|-------|
| Framework preset | None |
| Build command | None |
| Deploy command | `npx wrangler deploy` |
| Root directory | `/server` |

以降、fork への push で自動的に再デプロイされます。

**方法 B: CLI のみ**：

```sh
cd server
npm install
npx wrangler deploy
```

8. **確認**

デプロイの完了を待ってから
`https://<worker>.workers.dev/health`（またはカスタムドメイン）を開きます。`200 OK` が返れば、Worker とデータベースが正常に動作しています。

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

1. このリポジトリを**クローン**します（fork は不要です）：

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server
npm install
```

2. **`server/wrangler.toml` を編集**します。重要なのは `[vars]`
セクションだけです。ローカルモードでは `database_id` と KV の `id`
は無視されるため、プレースホルダーのままで構いません：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example" # ここを編集
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB；786432（768 KiB）を超えないこと
```

`INSTANCE_DOMAIN` は、あなたのインスタンスに外部から到達できるドメインでなければなりません。この値はインスタンス上のすべてのアドレス（`user@your.domain.example`）に埋め込まれ、他のインスタンスもこれを使ってフェデレーションメールをあなたのインスタンスへルーティングします。

3. ローカルで**データベースマイグレーションを適用**します（すべてのテーブルが作成されます）：

```sh
npx wrangler d1 migrations apply shyake-db --local
```

`--local` フラグに注意してください。Cloudflare
がホストするデータベースではなく、ディスク上の SQLite ファイルに書き込みます。

4. **サーバーを起動**します：

```sh
npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

`curl http://127.0.0.1:8787/health` で確認します。`200 OK`
が返れば、Worker とデータベースが正常に動作しています。

サーバーは `127.0.0.1` にバインドしたままにし、外部トラフィックはリバースプロキシに処理させます（次の手順）。`0.0.0.0`
へ直接バインドするのは、フェデレーションに参加しない信頼できる LAN 内でのみ妥当です。

5. **TLS 付きリバースプロキシを設定**します

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

6. **常時稼働させる**

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
WorkingDirectory=/home/shyake/shyake/server
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
`server/.wrangler/state/`
以下に保存されます。インスタンスのバックアップとは、このディレクトリのバックアップです（書き込み中のデータベースをコピーしないよう、先にサーバーを停止するか、SQLite
に安全なツールを使ってください）。このディレクトリを削除するとインスタンスは空のデータベースにリセットされます。`wrangler dev`
に `--persist-to <dir>` を渡せば、状態を別の場所に保存できます。

**注意事項：何を動かしているのかを理解する**

`wrangler dev` は Wrangler
の開発サーバーであり、堅牢化された本番サーバーではありません。Cloudflare
Workers を支えているのと同じ `workerd`
ランタイムを実行するため、個人や小規模コミュニティのインスタンスなら十分に持ちこたえますが、開発向けの挙動には注意が必要です：

- **ファイル監視 / ホットリロード**: ソースツリーを監視し、ファイルが変更されると
  Worker をリロードします。開発中は便利ですが、サーバー上では `server/`
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
