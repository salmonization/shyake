## Shyake デプロイガイド

[English](../../DEPLOY.md) | [简体中文](../zh-CN/DEPLOY.md) | 日本語

> Translated by Claude Fable 5

Shyake のサーバーには、同じ HTTP API を提供する 2 つの実装があります。クライアントはどちらにも接続でき、両者は互いにフェデレーションできます。

* **Cloudflare を使用する**：`server/cf/` の Worker。Cloudflare Workers 上で D1 データベースとともに動作します。自分のマシンは必要ありません。
* **セルフホスティング**：`server/go/` の Go サーバー。バイナリ 1 つと SQLite ファイル 1 つで、自分のマシン上で動作します。

**フェデレーション**

2 つのインスタンスは、双方でフェデレーションが有効（デフォルト）であれば自動的にフェデレーションします。追加の設定は不要です。インスタンス間のメールはサーバー間で直接ルーティングされ、クライアントは常に自分のインスタンスとのみ通信します。

受信・送信フェデレーションを無効にするには、`wrangler.toml` で `FEDERATION_ENABLED = false`（Worker）、または `SHYAKE_FEDERATION_ENABLED=false`（Go サーバー）を設定します。

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
| `--update` | 最新の正式リリースに切り替えてから再デプロイする |
| `--no-kv` | KV バージョンキャッシュを省略する |
| `--config-only` | `wrangler.toml` を生成して終了する |
| `--local` | ローカルの開発用サーバーを準備する（[DEV.md](DEV.md) を参照） |

#### アップグレード

```sh
cd shyake/server/cf
./deploy.sh --update
```

すべてのリリースタグを取得して最新の正式リリース（`vX.Y.Z`）に切り替え、プレリリースや未リリースのコードは飛ばします。その後、新しいマイグレーションを適用して再デプロイします。切り替え後に Git が "detached HEAD" と表示しますが、これは想定どおりです。既存のリソースは再利用され、設定もそのまま保たれます。また、`GET /api/version` が返すバージョンを `server/VERSION` から設定します。

**v0.3.0 へのアップグレード。** v0.3.0 から、クライアントは block、unblock、rotate のリクエストボディに署名します（プロトコルレベル 2、[SPEC.md §3.3](SPEC.md)）。v0.3.0 のクライアントは古いサーバーでこの 3 つの操作を行えず、古いクライアントも v0.3.0 のサーバーでは行えません。先にサーバーをアップグレードし、その後クライアントを `shyake update` で更新してください。Go サーバーも同様です。

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

`wrangler.toml` が生成式になる前にデプロイしたインスタンスの場合、`./deploy.sh --update` が設定を `wrangler.toml.bak` として退避し、新しいバージョンに切り替えた後に復元します。

**GitHub トークン（推奨）。** `shyake update` はインスタンスに最新のリリースを問い合わせ、インスタンスは GitHub API を呼び出します。トークンがない場合、GitHub が許可する呼び出しは IP アドレスごとに 1 時間 60 回までで、Cloudflare Workers は IP アドレスを共有しているため、他の Worker にその上限を使い切られることがあります。Worker 専用のトークンを設定してください：

```sh
npx wrangler secret put GITHUB_TOKEN
```

権限を一切付けない fine-grained トークンで十分です。それでも GitHub が失敗した場合、インスタンスは最後に取得できた結果を返します。

### セルフホスティング

Go サーバーは自分のマシン上で動作します。単一の静的バイナリ `shyake-server` で、すべてのデータを 1 つの SQLite ファイルに保存します。Node.js も Cloudflare アカウントも必要ありません。

前提条件：

- 常時オンラインのマシン。以下の例は systemd を備えた Linux を想定しています。
- ビルド用の Go 1.26 以降、または Docker。
- フェデレーションに参加する場合：そのマシンを指す公開ドメイン名と、有効な TLS 証明書を持つリバースプロキシ（手順 4）。

手順：

1. **バイナリを入手**します。リリース版をダウンロードするか、ソースからビルドします。

ダウンロードする場合：サーバーを変更したリリースには、[リリースページ](https://github.com/salmonization/shyake/releases)に `shyake-server-linux-amd64.tar.gz` と `shyake-server-linux-arm64.tar.gz` があります。クライアントだけを変更したリリースにはありません。これらを含む最新のリリースを使ってください。

```sh
tar -xzf shyake-server-linux-amd64.tar.gz
cd shyake-server-linux-amd64
```

アーカイブにはバイナリ、`shyake-server.service`、`shyake.env.example` が入っています。

ソースからビルドする場合：

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/go
CGO_ENABLED=0 go build -trimpath -ldflags "-X main.version=$(cat ../VERSION)" \
    -o shyake-server ./cmd/shyake-server
```

`-ldflags` は `shyake-server -version` と `GET /api/version` が返すバージョンを設定します。付けない場合、バージョンは `dev` になります。

どちらの方法でも静的バイナリが得られ、CPU アーキテクチャが同じ任意の Linux マシンにコピーして使えます。

2. **systemd でインストール**します。サービス用のシステムユーザーを作成してから、ファイルをインストールします。ソースではこれらのファイルは `server/go/deploy/` にあります。リリースのアーカイブを使う場合は、以下のコマンドから `deploy/` を取り除いてください：

```sh
sudo useradd --system --home-dir /var/lib/shyake --shell /usr/sbin/nologin shyake
sudo install -m 755 shyake-server /usr/local/bin/
sudo install -D -m 640 -g shyake deploy/shyake.env.example /etc/shyake/shyake.env
sudo install -m 644 deploy/shyake-server.service /etc/systemd/system/
```

3. **設定**します。`/etc/shyake/shyake.env` を編集し、少なくともインスタンスのドメインを設定します：

```sh
SHYAKE_INSTANCE_DOMAIN=your.domain.example
SHYAKE_LISTEN=127.0.0.1:8787
```

インスタンスのドメインはそのインスタンス上のすべてのアドレス（`user@your.domain.example`）に埋め込まれ、他のインスタンスはこれを使ってフェデレーションメールを送り返します。このファイルには他のすべての設定がデフォルト値とともに記載されています。詳細は [SPEC.md §11.2](SPEC.md) を参照してください。

次にサービスを起動します：

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now shyake-server
curl http://127.0.0.1:8787/health
```

`200 OK` が返れば、サーバーとデータベースは正常に動作しています。サービスは `shyake` ユーザーとして動作し、データベースを置く `/var/lib/shyake` にしか書き込めません。

4. **TLS 付きリバースプロキシを設定**します

この手順は**フェデレーションに必須**です。インスタンス同士は常に `https://<domain>/...` で通信するため、あなたのインスタンスは `https://your.domain.example` で到達可能で、他のインスタンスが受け入れる証明書を持っている必要があります。自己署名証明書は使えません。インスタンスが非公開（ユーザー同士でしかメールしない）であれば、この手順を省略して、クライアントを平文 HTTP で接続させることもできます。

[Caddy](https://caddyserver.com/) を使えば、証明書の取得と更新は自動で行われます。`Caddyfile` はこれだけです：

```
your.domain.example {
    reverse_proxy 127.0.0.1:8787
}
```

certbot で証明書を管理する nginx でも同様に動作します。`https://your.domain.example` を `http://127.0.0.1:8787` にプロキシしてください。

サーバーはクライアントアドレスごとにリクエスト数を制限します。プロキシの背後では `X-Forwarded-For` からクライアントアドレスを読み取りますが、それはプロキシのアドレスが `SHYAKE_TRUSTED_PROXIES` に含まれている場合に限られます。デフォルトでは同じマシン上のプロキシ（`127.0.0.1`、`::1`）だけを信頼します。プロキシが別の場所で動いている場合は、そのアドレスを追加してください。そうしないと、すべてのクライアントがプロキシのアドレスとみなされ、1 つのレート制限を共有することになります。

#### Docker で動かす

```sh
docker build --build-arg VERSION=$(cat server/VERSION) \
    -t shyake-server server/go
docker run -d --name shyake --restart unless-stopped \
    -p 127.0.0.1:8787:8787 -v shyake:/data \
    -e SHYAKE_INSTANCE_DOMAIN=your.domain.example \
    -e SHYAKE_TRUSTED_PROXIES=172.16.0.0/12 \
    shyake-server
```

データベースは `shyake` ボリューム上の `/data/shyake.db` です。コンテナからはリバースプロキシが Docker ブリッジのアドレスに見えるため、上の例のように `SHYAKE_TRUSTED_PROXIES` をブリッジのネットワークに設定してください。

#### アップグレード

新しいリリースのアーカイブ、または新しいビルドから新しいバイナリをインストールします。ビルドする場合は、先に最新の正式リリース（`vX.Y.Z`、プレリリースは除く）に切り替えます：

```sh
cd shyake
git fetch --tags
git checkout "$(git tag -l --sort=-v:refname 'v*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1)"
cd server/go
CGO_ENABLED=0 go build -trimpath -ldflags "-X main.version=$(cat ../VERSION)" \
    -o shyake-server ./cmd/shyake-server
sudo install -m 755 shyake-server /usr/local/bin/
sudo systemctl restart shyake-server
curl http://127.0.0.1:8787/api/version
```

サーバーは起動時に新しいデータベースマイグレーションを適用します。再起動中に処理中だった送信は失敗し、クライアントはそれを下書きとして残します。上の **v0.3.0 へのアップグレード** も参照してください。

#### データの場所とバックアップ

すべてのデータは 1 つの SQLite ファイルにあります。systemd では `/var/lib/shyake/shyake.db`、Docker では `/data/shyake.db` です。データベースは WAL モードで動作するため、サーバーの稼働中はその隣にさらに 2 つのファイル（`-wal`、`-shm`）があります。

稼働中のサーバーをバックアップするには、書き込み中でも安全な SQLite のオンラインバックアップを使います：

```sh
sudo sqlite3 /var/lib/shyake/shyake.db ".backup /root/shyake-backup.db"
```

または、サービスを停止してから 3 つのファイルをコピーします。

#### Worker からの移行

Go サーバーは Worker インスタンスのデータ（ユーザー、メール、ブロック）を引き継げます。保存されているアドレスはインスタンスのドメインに依存するため、ドメインは変えないでください。

Cloudflare 上の Worker からは、まずデータベースをエクスポートします：

```sh
cd shyake/server/cf
npx wrangler d1 export shyake-db --remote --output=d1-export.sql
```

ローカルの `wrangler dev` インスタンスからは、まずそれを停止し、そのデータベースファイルを使います。`server/cf/.wrangler/state/v3/d1/miniflare-D1DatabaseObject/` にある、`metadata.sqlite` ではない方の `.sqlite` ファイルです。このファイルは WAL モードで、データの大部分は隣の `-wal` ファイルにあります。データベースをコピーする場合は、`-wal` ファイルも一緒にコピーしてください。

次に、サービスを初めて起動する前に、`shyake` ユーザーとして新しいデータベースにインポートします。このユーザーがエクスポートファイルを読めるようにしておく必要があります：

```sh
sudo install -d -o shyake -g shyake -m 700 /var/lib/shyake
sudo install -o shyake -m 600 d1-export.sql /var/lib/shyake/
# D1 ファイルの場合：<file>.sqlite と <file>.sqlite-wal の両方をインストールする
sudo -u shyake env \
    SHYAKE_INSTANCE_DOMAIN=your.domain.example \
    SHYAKE_DATABASE=/var/lib/shyake/shyake.db \
    shyake-server -import-d1 /var/lib/shyake/d1-export.sql
sudo rm /var/lib/shyake/d1-export.sql
```

インポートは、すでにユーザーがいるデータベースを拒否します。そのままコピーできなかったものは出力で報告されます：

- 大文字小文字だけが異なる 2 つの名前：先に登録したアカウントがその名前を保持します。Go サーバーはこのような組を許しません。
- 同じ署名で 2 回保存されたメール：1 通だけ残します。

また、ブロックの記録を正規化された形式（[SPEC.md §4](SPEC.md)）に書き換えます。

あとはドメインを新しいマシンに向けるだけです。クライアントは何も変更する必要がありません。鍵もアドレスもそのままです。

#### データベース

Go サーバーが現在サポートしているのは SQLite だけです。ストレージ層はインターフェースの背後にあり、すべてのバックエンドが通過しなければならないテストスイートを備えています。PostgreSQL のサポートはこの仕組みの上に実装する予定です。それまでは、`SHYAKE_DATABASE` に `postgres://` を指定するとサーバーは起動を拒否します。
