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
  * [ローカル開発](#ローカル開発)

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

ローカル開発サーバーに対してエンドツーエンドのテストスイートを実行します：

```sh
# ターミナル 1
cd server/cf && npx wrangler dev --local

# ターミナル 2
cd client && make
bash tests/e2e_test.sh
```

### 非対話的なパスフレーズ

`SHYAKE_PASSPHRASE` を設定すると、秘密鍵の解錠が必要などの場面で対話的プロンプトをスキップできる（`init` も初期パスフレーズとしてこれを使う）。スクリプトによるテストを想定したもので、一般ユーザー向けではない。コマンドラインに直接書いた値はシェル履歴に残り、エクスポートした環境変数は子プロセスから見える。

```sh
export SHYAKE_PASSPHRASE=$(openssl rand -base64 12)
shyake init
shyake check inbox
```

## サーバー

### ローカル開発

```sh
cd server/cf
npx wrangler dev --local
```

Worker はデフォルトで `http://localhost:8787` をリッスンします。
