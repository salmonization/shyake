## Shyake 開発者ガイド

[English](../../DEV.md) | [简体中文](../zh-CN/DEV.md) | 日本語

> Translated by Claude Fable 5

このドキュメントは Shyake の開発を支援するものです。

**目次**：

- [クライアント](#クライアント)
  * [依存関係](#依存関係)
  * [ビルド](#ビルド)
  * [インストール](#インストール)
  * [テスト](#テスト)
- [サーバー](#サーバー)
  * [Worker](#worker)
  * [Go サーバー](#go-サーバー)

## クライアント

### 依存関係

`liboqs` はすべてのプラットフォームで静的リンクされるため、バイナリはそれに対する実行時依存を持ちません。`libcurl` と
`libcrypto` はすべてのプラットフォームで動的リンクのままです。

依存関係（ビルド時のみ）：

| ライブラリ | 用途 |
|---------|---------|
| `liboqs` | ML-KEM-768 と ML-DSA-65 |
| `libcurl` | HTTP トランスポート |
| `openssl`（`libcrypto`） | SHA-256 フィンガープリント |

macOS（Homebrew）の場合：

```sh
brew install liboqs curl openssl@3
```

Arch Linux の場合：

```sh
sudo pacman -S cmake curl openssl
# liboqs をソースからビルドする：下記の手順を参照
```

Debian/Ubuntu の場合：

```sh
sudo apt install cmake libcurl4-openssl-dev libssl-dev
# liboqs をソースからビルドする：下記の手順を参照
```

Termux（Android）の場合：

```sh
pkg install clang cmake make curl-dev openssl-dev
# liboqs をソースからビルドする：下記の手順を参照
```

**liboqs のビルド**

`liboqs` をソースからコンパイルする場合（GNU/Linux や Termux
など）、最小構成でビルドする必要があります。すべてのアルゴリズムを有効にして `liboqs` をビルドすると、バイナリサイズが大幅に肥大化します（約 20MB）。

Shyake に必要なアルゴリズム（ML-KEM-768 と ML-DSA-65）のみで
`liboqs` をビルドするには、次を実行します：

```sh
git clone --depth 1 \
          --single-branch -b main \
          https://github.com/open-quantum-safe/liboqs.git
cd liboqs
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DOQS_BUILD_ONLY_LIB=ON \
      -DOQS_USE_OPENSSL=ON \
      -DOQS_MINIMAL_BUILD="KEM_ml_kem_768;SIG_ml_dsa_65" \
      ..
make -j$(nproc)
sudo make install
```

**Termux** でコンパイルする場合は、インストールプレフィックス（`$PREFIX`）を指定し、`sudo` を省略する必要があります：

```sh
cmake -DCMAKE_BUILD_TYPE=Release \
      -DOQS_BUILD_ONLY_LIB=ON \
      -DOQS_USE_OPENSSL=ON \
      -DOQS_MINIMAL_BUILD="KEM_ml_kem_768;SIG_ml_dsa_65" \
      -DCMAKE_INSTALL_PREFIX=$PREFIX \
      ..
make -j4
make install
```

### ビルド

```sh
cd client
make
```

成果物：

| ファイル | 説明 |
|------|-------------|
| `bin/shyake` | CLI バイナリ（全プラットフォームで `liboqs` を静的リンク） |
| `lib/libshyake.a` | FFI 用静的ライブラリ |
| `lib/libshyake.so` または `lib/libshyake.dylib` | FFI 用共有ライブラリ |

### インストール

バイナリを `$PATH` の通った任意のディレクトリにコピーします：

```sh
cp bin/shyake /usr/local/bin/
```

### テスト

ローカルのサーバーに対してエンドツーエンドのテストスイートを実行します。どちらのサーバーでも構いませんが、プロトコルに関わる変更は両方で通る必要があります：

```sh
# ターミナル 1：Worker
cd server/cf && npx wrangler dev --local
# または Go サーバー
cd server/go && SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 go run ./cmd/shyake-server

# ターミナル 2
cd client && make
bash tests/e2e_test.sh
```

`SHYAKE_TEST_INSTANCE` でテストスイートの接続先 URL を変えられます。

フェデレーションのテストは Go サーバーを 2 つ自分で起動し、その間でクライアントを動かします。双方向のリレー、リモートインスタンスの復帰を待ってから届くリレー、リレーされたメールに対するブロックを確認します：

```sh
cd client && make && cd ..
bash tests/federation_test.sh
```

### 非対話的なパスフレーズ

`SHYAKE_PASSPHRASE` を設定すると、秘密鍵の解錠が必要な場面で対話的プロンプトをスキップできます（`init` も初期パスフレーズとしてこれを使います）。スクリプトによるテストを想定したもので、一般ユーザー向けではありません。コマンドラインに直接書いた値はシェル履歴に残り、エクスポートした環境変数は子プロセスから見えます。

```sh
export SHYAKE_PASSPHRASE=$(openssl rand -base64 12)
shyake init
shyake check inbox
```

## サーバー

### Worker

```sh
cd server/cf
./deploy.sh --local      # 依存関係、wrangler.toml、ローカルデータベース
npx wrangler dev --local
```

Worker はデフォルトで `http://localhost:8787` をリッスンします。

`wrangler.toml` は `wrangler.template.toml` から生成され、git の管理対象ではありません。Cloudflare へのデプロイは同じスクリプトを `--local` なしで実行します。[DEPLOY.md](DEPLOY.md) を参照してください。

### Go サーバー

Go 1.26 以降が必要です。サーバーは cgo に依存しません。

```sh
cd server/go
go test ./...                    # ユニットテスト
go vet ./...
gofmt -l .                       # 何も出力されないこと
SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 SHYAKE_DATABASE=/tmp/dev.db \
    go run ./cmd/shyake-server
```

パッケージの構成（外側から内側へ）：

| パッケージ | 役割 |
|---|---|
| `internal/protocol` | アドレス、PoW、署名、署名対象メッセージ。I/O は行いません。 |
| `internal/api` | HTTP ハンドラー、認証、レート制限 |
| `internal/federation` | 送信クライアント、リモート公開鍵キャッシュ、リレーキュー |
| `internal/store` | ストレージインターフェースとバックエンドのテストスイート |
| `internal/store/sqlite` | SQLite バックエンドとマイグレーション |
| `internal/config` | `SHYAKE_*` 環境変数の設定 |

**署名のテストベクター。** `internal/protocol/testdata/liboqs_vectors.json` には、クライアント自身の cJSON で組み立てたメッセージに liboqs が付けた署名が入っています。Go のテストは、circl がそれらの署名を受け入れること、そしてサーバーが署名対象メッセージをバイト単位で正確に再構築できることを確認します。署名対象メッセージを変更したら、このファイルを再生成してください：

```sh
cd server/go/internal/protocol/testdata
cc -std=c11 -o /tmp/gen gen_vectors.c \
   ../../../../../client/src/lib/vendor/cJSON/cJSON.c \
   -I../../../../../client/src/lib/vendor/cJSON \
   /usr/local/lib/liboqs.a -lcrypto
/tmp/gen > liboqs_vectors.json
```

**新しいストレージバックエンド**は `store.Store` を実装し、`storetest.Run`（SQLite バックエンドと同じテストスイート）に通る必要があります。PostgreSQL はこの方法で対応する予定です。SQL はバックエンドのパッケージ内に閉じ込めてください。インターフェースが扱うのはユーザー、メール、ブロック、リレーだけです。

**1 台のマシンでのフェデレーション。** インスタンス同士は HTTPS で通信し、サーバーはプライベートアドレスへの接続を拒否します。ローカルでのテストでは、`SHYAKE_FEDERATION_INSECURE=true` で平文 HTTP とループバックアドレスを許可できます。公開インスタンスでは絶対に設定しないでください。
