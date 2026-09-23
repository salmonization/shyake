## Shyake 技術仕様

[English](../../SPEC.md) | [简体中文](../zh-CN/SPEC.md) | 日本語

> Translated by Claude Fable 5

Copyright (c) 2026 Salmonization. BSD 2-Clause License.

<table>
<tr><td>バージョン</td><td>0.3</td></tr>
<tr><td>最終更新</td><td>2026-09-23</td></tr>
</table>

---

### 1. 概要

Shyake は、POSIX スタイルのコマンドラインインターフェースクライアントを備えた、耐量子・エンドツーエンド暗号化の非同期メールシステムであり、検閲や監視に対抗するための分散型コミュニケーション手段として設計されている。

主な特性：

- **エンドツーエンド暗号化**：サーバーは平文を一切保持しない。すべてのメッセージ内容は送信前にクライアント側で暗号化される。
- **耐量子暗号**：鍵カプセル化には ML-KEM-768 を、認証には ML-DSA-65（CRYSTALS-Dilithium）を使用する。いずれも [liboqs](https://github.com/open-quantum-safe/liboqs) 由来である。
- **分散型**：どの運営者もほぼゼロコストで自分のインスタンスをホストできる。インスタンスは、サーバー間リレーモデルを用いて任意でフェデレーションする。
- **ステートレスなサーバー**：サーバーは暗号文と公開鍵のみを保存する。
- **保存時に暗号化される鍵**：クライアントの秘密鍵は、任意でパスフレーズ（scrypt + ChaCha20-Poly1305）によりディスク上で保護される。

---

### 2. アーキテクチャ

#### 2.1 コンポーネント

```
shyake/
├── client/                 # C クライアント
│   ├── src/lib/            # コアロジック（ネットワーク、暗号、
│   │                       #   メール、アカウント、パスフレーズ）
│   ├── src/cli/            # CLI 解析、表示、プロンプト、設定、
│   │                       #   下書き、自己更新
│   ├── include/shyake.h    # 公開 API（不透明ポインタ）
│   ├── tests/              # ライブラリテストプログラム
│   └── Makefile
├── server/
│   ├── cf/                 # Cloudflare Worker
│   │   ├── src/index.ts    # Hono ルート
│   │   ├── src/utils.ts    # ヘルパー（PoW、ユーザー名検証）
│   │   ├── migrations/     # D1 スキーママイグレーション
│   │   └── wrangler.template.toml  # Worker 設定
│   └── go/                 # Go サーバー（セルフホスティング用）
│       ├── cmd/shyake-server/      # エントリポイント
│       └── internal/       # protocol、store、api、federation
└── docs/
```

#### 2.2 クライアント

- **標準**：C11、POSIX.1-2008（`_POSIX_C_SOURCE=200809L`）
- **ビルドシステム**：GNU Make、クロスプラットフォーム（macOS、GNU/Linux、Termux）
- **成果物**：
  - `bin/shyake`: CLI バイナリ、`libshyake.a` を静的リンク
  - `lib/libshyake.a`: 静的ライブラリ
  - `lib/libshyake.so` / `libshyake.dylib`: FFI 用共有ライブラリ
- **依存関係**：
  - `liboqs`（常に静的リンク）: ML-KEM と ML-DSA
  - `libcurl`: HTTP トランスポート
  - `libcrypto`（OpenSSL）: SHA-256 フィンガープリント、SHA-1（PoW）、ChaCha20-Poly1305 AEAD、scrypt KDF（`EVP_PBE_scrypt`）
  - `cJSON`（同梱）: JSON 解析

#### 2.3 サーバー

サーバーには 2 つの実装があり、同じ HTTP API（§5）を提供する。クライアントはどちらにも接続でき、両種のインスタンスは相互にフェデレーションできる。

**Worker**（`server/cf/`）、Cloudflare 向け：

- **ランタイム**：Cloudflare Workers
- **フレームワーク**：[Hono](https://hono.dev/)
- **データベース**：Cloudflare D1（SQLite）
- **署名検証**：WebAssembly にコンパイルされた ML-DSA-65（`mldsa65-wasm`）を、Wrangler の `CompiledWasm` ルール経由でロードする。

**Go サーバー**（`server/go/`）、セルフホスティング向け：

- **ランタイム**：単一の静的バイナリ `shyake-server`
- **データベース**：単一の SQLite ファイル。PostgreSQL は同じストレージインターフェースの背後に追加する計画である。
- **署名検証**：[circl](https://github.com/cloudflare/circl) の純粋な Go 実装による ML-DSA-65

---

### 3. 暗号設計

#### 3.1 鍵ペア

各ユーザーは `liboqs` を用いてローカルで 2 つの独立した鍵ペアを生成する：

| 用途 | アルゴリズム | ファイル |
|---|---|---|
| 鍵カプセル化 | ML-KEM-768 | `kem_pk.bin`、`kem_sk.bin` |
| 認証／署名 | ML-DSA-65 | `sig_pk.bin`、`sig_sk.bin` |

公開鍵は生バイトとして保存され、登録時にサーバーへアップロードされる。秘密鍵は、パスフレーズが設定されている場合は §3.7 に記述する保存時暗号化フォーマットで、設定されていない場合は生バイトとして保存される。

#### 3.2 メッセージ暗号化

1. ランダムな 256 ビット対称鍵を生成する。
2. その鍵を用いて `subject` と `body` を **ChaCha20-Poly1305** で暗号化する。それぞれ独立したランダムな 96 ビット nonce を使用する。各暗号文は `base64(nonce || ciphertext || tag)` として送信される。
3. **受信者の ML-KEM 公開鍵**にカプセル化する：KEM カプセル化により KEM 暗号文と 32 バイトの共有秘密が得られる。対称鍵を共有秘密と XOR して連結する：`enc_key_recipient = base64(kem_ct || (sym_key XOR ss))`。
4. **送信者自身の ML-KEM 公開鍵**に対して同じカプセル化を繰り返す → `enc_key_sender`（送信者が自分の送信ボックスを読めるようにするため）。

復号はこの逆の手順である：クライアントは自身の KEM 秘密鍵で共有秘密をデカプセル化し、それを暗号化鍵フィールドと XOR して対称鍵を復元し、その後コンテンツを復号する。

単体ファイル暗号化コマンド（`enc` / `dec`）は、同じ ML-KEM-768 + ChaCha20-Poly1305 構成を、長さプレフィックス付きのバイナリコンテナ（`.enc` ファイル）で使用する。

#### 3.3 認証プロトコル

認証を要するすべての操作は ML-DSA-65 で署名される。2 つの運搬形式が使われる：

**ヘッダーベース**（登録とメール送信を除くすべての認証エンドポイント）：

```
X-Shyake-Username:  <username>
X-Shyake-Timestamp: <unix seconds>
X-Shyake-Signature: <base64(ML-DSA-65 signature)>
X-Shyake-Pow:       <Hashcash token>
```

署名対象のメッセージは、HTTP メソッド、エンドポイント（クエリ文字列を含む）、ユーザー名、タイムスタンプから構成される決定的な文字列である。例：

```
GET:/api/mail?type=inbox:salmon:1749513600
```

ボディを持つリクエスト（`POST /api/rotate`、`POST /api/block`、`DELETE /api/block`）では、末尾にさらにコロンと、ボディの生バイト列の SHA-256（小文字の 16 進）を付ける：

```
POST:/api/block:salmon:1749513600:<ボディの sha256 16 進>
```

これによりボディが署名に結び付く。そうでなければ、転送中のリクエストを読める者（たとえば TLS を終端するプロキシ）が署名をそのまま残してボディを差し替えられる。rotate なら自分の公開鍵に、block なら別の対象に置き換えられてしまう。プロトコルレベル 2（§5.1）のサーバーは、この 3 つのエンドポイントではこの形式のみを受け付け、ダイジェストのない旧形式には `401` を返す。

**ボディベース**（`POST /api/register` と `POST /api/mail`）：署名と PoW トークンはリクエストボディの JSON フィールドとして運ばれる。署名対象のメッセージは、以下のペイロード部分集合のコンパクト JSON シリアライゼーションである（フィールド順はクライアントが生成した通り）：

`POST /api/register`：

```json
{
  "username": "...",
  "kem_pubkey": "...",
  "sig_pubkey": "...",
  "timestamp": "1749513600"
}
```

`POST /api/mail`：

```json
{
  "sender": "...",
  "recipient": "...",
  "recipient_kem_fingerprint": "...",
  "enc_subject": "...",
  "enc_body": "...",
  "timestamp": "1749513600",
  "size": 512
}
```

完全なリクエストボディはさらに `enc_key_sender`、`enc_key_recipient`、`signature`、`pow` を運ぶが、これらは署名対象の部分集合には含まれない。

サーバーは送信者の `sig_pubkey` で署名を検証する。公開鍵は自インスタンスのデータベースから読み出すか、フェデレーションメールの場合は送信者のインスタンスから取得する。

#### 3.4 リプレイ対策

署名対象のすべてのメッセージにタイムスタンプが含まれる。サーバーは、タイムスタンプがサーバー時刻から **300 秒（5 分）**を超えて乖離しているリクエストを拒否する。

Go サーバーはさらに、受け入れた署名をすべて記憶し、同じ署名を再利用したリクエストを HTTP 403 で拒否する。ただし `POST /api/mail` では、署名の重複はエラーではない。サーバーは保存済みメールの id とともに `201` を返し、二重には保存しない。したがってクライアントの再送は安全である。

#### 3.5 プルーフ・オブ・ワーク（PoW）

読み取りを含むすべての認証リクエストには、SHA-1 難易度 **20 ビット**の Hashcash-v1 スタイルの PoW トークンが必要である：

```
1:<bits>:<yymmdd>:<resource>::<rand>:<counter-hex>
```

`resource` は操作するユーザーである。登録とヘッダー認証ではユーザー名、メール送信では `sender` フィールドとなる。トークンはクライアント側で生成（マイニング）され、署名検証やデータベース処理より前にサーバー側で検証される。サーバーは次のいずれかに当てはまるトークンを拒否する：

- resource が操作するユーザーでない
- 日付がサーバーの日付（UTC）から 1 日を超えて離れている
- SHA-1 ハッシュの先頭 20 ビットがすべて 0 ではない

フェデレーションの送信者の resource にはコロンが含まれることがある（`alice@host:8787`）。そのためサーバーは先頭 3 フィールドと末尾 3 フィールドを位置で取り出し、その間をすべて resource とみなす。

Go サーバーはさらに、一度受け入れたトークンを拒否する。したがって 1 つのトークンで支払えるのは 1 回のリクエストだけである。

#### 3.6 鍵フィンガープリント

フィンガープリントは、生の（デコード済み）ML-KEM 公開鍵バイトの **SHA-256** を小文字 16 進エンコードしたものである。クライアントは信頼済みの鍵を `~/.config/shyake/known_hosts` にキャッシュする。1 行につき 1 エントリで、スペース区切りである：

```
<username> <fingerprint-hex> <kem_pubkey-base64>
```

クライアントは送信のたびに受信者の最新の鍵を `known_hosts` と比較し、さらにペイロードに `recipient_kem_fingerprint` を埋め込む。これによりサーバーは保存済みの鍵と比較し、古い鍵に基づく送信を `KEY_MISMATCH`（HTTP 409）で拒否できる。

#### 3.7 秘密鍵の保存時保護

ユーザーが空でないパスフレーズを設定すると、秘密鍵ファイル（`kem_sk.bin`、`sig_sk.bin`）は `SHYK` コンテナフォーマットで書き込まれる：

| オフセット | サイズ | フィールド |
|---|---|---|
| 0 | 4 B | マジック `"SHYK"` |
| 4 | 1 B | バージョン `0x01` |
| 5 | 1 B | KDF id `0x01`（scrypt） |
| 6 | 32 B | ソルト（ランダム） |
| 38 | 4 B | scrypt `N`（LE u32、デフォルト 65536） |
| 42 | 4 B | scrypt `r`（LE u32、デフォルト 8） |
| 46 | 4 B | scrypt `p`（LE u32、デフォルト 1） |
| 50 | 12 B | ChaCha20-Poly1305 nonce（ランダム） |
| 62 | — | 暗号文（平文の鍵と同じ長さ） |
| 末尾 | 16 B | Poly1305 タグ |

62 バイトのヘッダーは AAD として結び付けられるため、KDF パラメータへのいかなる改ざんも認証に失敗する。KDF はパスフレーズから 256 ビットの ChaCha20-Poly1305 鍵を導出する。

`SHYK` マジックを持たないファイルはレガシーな生鍵として扱われ、そのままロードされる。空のパスフレーズの場合は生の（暗号化されていない）鍵が書き込まれる。

パスフレーズは対話的に入力（ターミナルエコー無効）するか、非対話用途では `SHYAKE_PASSPHRASE` 環境変数で渡す。`rotate` は現在のパスフレーズと新しいパスフレーズの入力を求め、新しい鍵ペアはサーバーがローテーションを確認した後にのみ、新しいパスフレーズで保存される。

#### 3.8 ローカル暗号化下書き

`shyake compose` は下書きを設定ディレクトリ内の `drafts/<id>.json` に保存する。下書きがサーバーに触れることはない。各下書きはメールと同じハイブリッド方式（§3.2）を使用する：ランダムな 32 バイトの対称鍵が各フィールドを ChaCha20-Poly1305 で暗号化し、その鍵はユーザー自身の KEM 公開鍵に ML-KEM-768 でカプセル化される。

```json
{
  "version": 1,
  "draft_id": "3",
  "created": 1752400000,
  "modified": 1752400000,
  "size": 123,
  "enc_key": "<b64: kem_ct || (sym_key XOR ss)>",
  "enc_recipient": "<b64: nonce||ct||mac>",
  "enc_subject": "<b64: nonce||ct||mac>",
  "enc_body": "<b64: nonce||ct||mac>"
}
```

宛先・件名・本文はすべて暗号化された状態で保存される。平文なのはタイムスタンプ、サイズ、id だけである。空の `enc_recipient` / `enc_subject` 文字列は空フィールドを表す。

保存には公開鍵しか必要ないため、`compose` はパスフレーズ不要である。一覧表示・閲覧・編集・送信には KEM 秘密鍵のアンロックが必要である。下書き id はローカルで割り当てられる小さな整数である（既存の最大 id + 1、`O_EXCL` で作成）。

compose のエディタが扱う平文一時ファイルは `mkstemp`（モード 0600）で設定ディレクトリ内に作成される。`/tmp` は決して使わない。終了後、ファイルはゼロで上書きされてから削除される。エディタが `vim`/`nvim` の場合は `-n -i NONE` 付きで起動され、平文が swap や viminfo に漏れることはない。

---

### 4. データベーススキーマ

どちらのサーバーも SQLite を使う。Worker は Cloudflare D1（`server/cf/migrations/`）、Go サーバーはローカルファイル（`server/go/internal/store/sqlite/migrations/`）である。以下のテーブルは両者に共通する。Go サーバーはさらに次を追加する：

- `mail` テーブルの `sig_hash` 列：`signature` の SHA-256 で、一意である。再送されたメールを識別するために使う（§3.4）。

#### `users`

| カラム | 型 | 備考 |
|---|---|---|
| `username` | TEXT PK | 正規表現 `^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$` |
| `kem_pubkey` | TEXT | Base64 エンコードされた ML-KEM-768 公開鍵 |
| `sig_pubkey` | TEXT | Base64 エンコードされた ML-DSA-65 公開鍵 |
| `created_at` | INTEGER | UNIX タイムスタンプ |

ユーザー名は**大文字小文字を区別せずに**一意である。大文字小文字だけが既存の名前と異なる場合、登録は HTTP 409 で拒否されるため、`Alice` を `alice` と並べて取得することはできない。それ以外の場所での照合は完全一致であり、両者が混同されることはない。

`destroy` 時、`kem_pubkey` と `sig_pubkey` は空文字列に設定され、そのユーザーに関わるすべてのメール行とブロック行が削除される。ユーザー行自体は、ユーザー名を永久にロックするために **保持される**。

#### `mail`

| カラム | 型 | 備考 |
|---|---|---|
| `mail_id` | TEXT PK | 10 文字の base58 文字列、サーバーが割り当て |
| `sender` | TEXT | ローカル名、フェデレーションでは `user@domain` |
| `recipient` | TEXT | ローカル名、フェデレーションでは `user@domain` |
| `enc_key_sender` | TEXT | 送信者向けに KEM カプセル化された鍵 |
| `enc_key_recipient` | TEXT | 受信者向けに KEM カプセル化された鍵 |
| `enc_subject` | TEXT | ChaCha20-Poly1305 暗号文、base64 |
| `enc_body` | TEXT | ChaCha20-Poly1305 暗号文、base64 |
| `size` | INTEGER | 平文本文のバイト数（UI 表示専用） |
| `signature` | TEXT | 送信者の ML-DSA-65 署名、base64 |
| `timestamp` | INTEGER | サーバーが割り当てる UNIX タイムスタンプ |

ローカルアドレスは裸の名前で保存される：`@<INSTANCE_DOMAIN>` サフィックスは挿入前に取り除かれる。`recipient` と `sender` のインデックスがメールボックスクエリを支える。`rotate` はそのユーザーが送信者または受信者であるすべてのメール行を削除する。

#### `blocks`

| カラム | 型 | 備考 |
|---|---|---|
| `blocker` | TEXT | ブロックするユーザーのユーザー名 |
| `blocked` | TEXT | ユーザー名、`user@domain`、または裸のドメイン |
| `created_at` | INTEGER | UNIX タイムスタンプ |
| PK | | `(blocker, blocked)` 複合主キー |

アドレスは保存や照合の前に**正規化**される。`@<INSTANCE_DOMAIN>` サフィックスは取り除かれ、リモートアドレスのドメイン部分と裸のドメインは小文字化される。したがってローカルユーザーは常に `bob`、リモートユーザーは常に `mallory@evil.example` となる。

メール送信時、受信者が送信者の正規化されたアドレスまたは送信者のドメインをブロックしている場合、サーバーは HTTP 403 で送信を拒否する。双方が先に正規化されるため、受信者が完全修飾形式で届く中継メールでもこの検査が機能する。

---

### 5. HTTP API

両サーバーは同じエンドポイントを、同じステータスコードとレスポンスボディで提供する。ベース URL はインスタンスのドメインである。

#### 5.1 公開エンドポイント

| メソッド | パス | 説明 |
|---|---|---|
| `GET` | `/health` | 死活チェック、データベースに問い合わせ |
| `GET` | `/api/pubkey/:username` | `kem_pubkey`、`sig_pubkey` を返す |
| `GET` | `/api/version` | サーバーのリリースとプロトコルレベル |
| `GET` | `/api/client/version` | 最新のクライアントリリースタグ |

`/api/pubkey/:username` は `user@domain` 構文をサポートする。ドメインがローカルインスタンスと異なる場合、サーバーはリクエストをリモートインスタンスへプロキシする（フェデレーションの有効化が必要）。

`/api/version` はサーバー自身を示す：

```json
{"version": "v0.3.0", "implementation": "go", "protocol": 2}
```

`version` は `server/VERSION` にあるリリース（§12.1）で、バージョンを埋め込まずにビルドした場合は `dev` になる。`implementation` は `cf` または `go` である。`protocol` はプロトコルレベルで、レベル 2 はリクエストボディに署名する（§3.3）。このエンドポイントを持たないサーバーはレベル 1 とみなす。クライアントは、インスタンスが受け付けるリクエスト形式をバージョンではなくプロトコルレベルで判断する。

`/api/client/version` は GitHub Releases API をプロキシし、クライアントのビルドを含む各チャネルの最新タグと、そのリリースの各アセットの SHA-256 ダイジェストを返す：

```json
{
  "release": "vX.Y.Z",
  "release_digests": {"shyake-linux-x86_64.tar.gz": "<hex>"},
  "pre_release": "vX.Y.Z-...",
  "pre_release_digests": {"shyake-linux-x86_64.tar.gz": "<hex>"}
}
```

どちらのチャネルも欠けることがある。サーバーのビルド（`shyake-server-*`）しか含まないリリースは飛ばす。結果は 1 時間キャッシュされる。Worker は KV に、Go サーバーはメモリに保持する。§12 を参照。

#### 5.2 認証エンドポイント

すべてのエンドポイントは PoW トークン、タイムスタンプウィンドウ、ML-DSA-65 署名（§3.3）を検証する。`POST /api/register` と `POST /api/mail` は認証フィールドを JSON ボディで運び、その他はすべて `X-Shyake-*` ヘッダーを使用する。

| メソッド | パス | 説明 |
|---|---|---|
| `POST` | `/api/register` | 新規ユーザーを登録 |
| `POST` | `/api/mail` | メールを送信 |
| `GET` | `/api/mail?type=inbox\|sent` | メールボックスのメタデータを一覧表示 |
| `GET` | `/api/mail/:id` | 単一のメールを取得（完全な暗号文） |
| `DELETE` | `/api/mail/:id` | メールを焼却（削除） |
| `POST` | `/api/block` | ユーザーまたはドメインをブロック |
| `DELETE` | `/api/block` | ユーザーまたはドメインのブロックを解除 |
| `GET` | `/api/block` | 呼び出し元のブロック一覧を返す |
| `POST` | `/api/rotate` | 公開鍵をローテーション |
| `DELETE` | `/api/destroy` | アカウントを抹消 |

`POST /api/mail` の注目すべきステータスコード：`409`（`KEY_MISMATCH`、送信された受信者フィンガープリントがもはや一致しない）、`410`（`USER_DESTROYED`）、`413`（ペイロードが大きすぎる）、`403`（ブロック済み、不正な PoW、または古いタイムスタンプ）。

Go サーバーはさらに次を返すことがある：

- `429`：任意のエンドポイントで、クライアントアドレスがレート制限を超えたとき。
- `403`：リクエストが署名または PoW トークンを再利用したとき（§3.4、§3.5）。
- `403`：`POST /api/mail` で、送信者と受信者のどちらもこのインスタンスに属さないとき。サーバーは他の 2 つのインスタンス間ではリレーしない。
- `503`：`POST /api/mail` で、送信者のインスタンスが公開鍵の問い合わせに応答しないとき。
- `502`：`POST /api/mail` で、受信者のインスタンスが公開鍵の問い合わせに応答しないとき。Worker はこの場合 `404` を返す。

リレーが失敗したとき、どちらのサーバーも `502`、またはリモートインスタンス自身の拒否をそのまま返す（§6.2）。

#### 5.3 サイズ制限

サーバーは `POST /api/mail` の生の HTTP リクエストボディにハードキャップを課す。デフォルトは **196608 バイト**（192 KiB）で、運用者が変更できる（§11）。絶対上限は 786432 バイト（768 KiB）で、Cloudflare D1 の単一行制限によるものである。Go サーバーも同じ上限を守る。これにより、そのメールはフェデレーションで Worker インスタンスにも受け入れられる。

---

### 6. フェデレーション

#### 6.1 アドレッシング

- **ローカルユーザー**：`username`（`@` なし）
- **リモートユーザー**：`username@instance.domain`

クライアントは常にユーザー自身のインスタンスとのみ通信する。リモートの受信者へ送信する際、クライアントは自身の送信者文字列を `username@<own-domain>` として修飾する。

#### 6.2 送信メールリレー

`recipient` がリモートインスタンスに属する場合：

1. クライアントは署名・暗号化済みペイロードを**送信者自身のインスタンス**に POST する（`POST /api/mail`）。
2. 送信者のインスタンスは検証を終えた後、元の生ペイロードを `https://<recipientDomain>/api/mail` へ転送し、最大 15 秒まで応答を待つ。
3. リモートインスタンスがメールを受け入れたら（`2xx`）、送信者のインスタンスは送信ボックス用に自身のコピーを保存し、`201` を返す。

リレーが失敗した場合、送信者のインスタンスは何も保存しない：

- リモートインスタンスが拒否したとき（`408`、`429` 以外の `4xx`）：送信者のインスタンスは同じステータスとリモートの `error` テキストをそのまま返す。たとえば `403` と `Recipient has blocked this sender` である。どちらのサーバーもブロック時には必ずこのテキストを使い、クライアントはこれでブロックを識別する（`SHYAKE_ERR_BLOCKED`、§8）。
- それ以外の失敗（接続できない、タイムアウト、`408`、`429`、`5xx`）：送信者のインスタンスは `502` と `Recipient instance unreachable` を返す。

サーバーはリレーをキューに入れず、再試行もしない。キューがあっても長くはもたない。受信者のインスタンスは送信者が署名したタイムスタンプの 300 秒後にペイロードを拒否し（§3.4）、署名し直せるのはクライアントだけだからである。送信に失敗すると、クライアントはメールをローカルの下書きとして残し（§3.8）、ユーザーは後で送り直せる。送り直すときは新しいペイロードに署名する。

受信者のインスタンスは、送信者のインスタンスから送信者の公開鍵を取得して（`GET /api/pubkey/<sender>`）、送信者の署名を独立に検証する。

送信者と受信者の両方のデータベースがメールを保存する。そのため、リモートインスタンスが使えないときでも送信者の送信ボックスは使える。

#### 6.3 フェデレーションの切り替え

`FEDERATION_ENABLED`（Worker）または `SHYAKE_FEDERATION_ENABLED`（Go サーバー）で設定できる。`false` の場合、インスタンスはリモートユーザーの解決を拒否し、受信のリレーメールと送信のインスタンス間送信の両方が拒否される。

---

### 7. 信頼モデル（TOFU + OOB）

Shyake は公開鍵管理に **Trust On First Use（TOFU）** を採用している：

- **初回接触**：クライアントは `GET /api/pubkey/<recipient>` を問い合わせ、KEM フィンガープリントを計算し、`~/.config/shyake/known_hosts` に黙って追記する。
- **以降の接触**：送信のたびに、取得した鍵を `known_hosts` のエントリと比較する。不一致の場合、何も送信される前にローカルで `KEY_MISMATCH` により中止する。
- **サーバー側のダブルチェック**：ペイロードに `recipient_kem_fingerprint` が埋め込まれており、保存済みの鍵と一致しなくなった場合、サーバーは独立に HTTP 409 で拒否する。
- **鍵ローテーションの検出**：クライアントは送信を止め、次のように表示する：

```
Error: Send failed. The public key of <username> has changed.
Run 'shyake fingerprint <username>' to check the new key.
```

`fingerprint` コマンドは**帯域外（OOB）検証**を提供する：サーバーから現在の公開鍵を取得し、フィンガープリントを計算して `known_hosts` と比較する。出力には GPG スタイルの 16 進グループと OpenSSH スタイルの randomart イメージが表示される。`--update` フラグは、ユーザーが信頼できるチャネルで新しいフィンガープリントを検証した後に `known_hosts` を書き換える。

---

### 8. クライアントライブラリ ABI

`libshyake` はプロトコルの実装そのものであり、同梱の CLI はその上に構築された参照クライアントの一つにすぎない。ライブラリには核心的かつ普遍的なロジック（暗号処理、ワイヤフォーマットのエンコード、本仕様で定義される送受信操作）のみを含める。クライアント固有の部分（引数解析、表示、対話プロンプト、自己更新）は `src/cli/` に置き、ライブラリへ移してはならない。サードパーティの開発者は `libshyake` のみを基盤として、TUI・GUI を問わず、FFI 経由で任意の言語により、完全にプロトコル互換のクライアントを構築できる。

コアライブラリは `include/shyake.h` を通じて安定した C API を公開する。ABI の破壊を防ぐため、内部状態は不透明ポインタの背後に隠されている：

```c
typedef struct shyake_ctx shyake_ctx;

shyake_ctx* shyake_init_ctx(const shyake_config *config);
void        shyake_free_ctx(shyake_ctx *ctx);

/* passphrase for secret key files (§3.7) */
void shyake_set_passphrase(shyake_ctx *ctx, const char *pp);
void shyake_set_new_passphrase(shyake_ctx *ctx, const char *pp);

/* このコンテキストの直近の失敗の詳細、なければ "" */
const char* shyake_last_error(shyake_ctx *ctx);
```

内部構造体の定義は `src/lib/lib_internal.h` にあり、呼び出し側には公開されない。ライブラリは stdout/stderr へ一切出力しない。失敗時には失敗の理由を記録し（`shyake_last_error(ctx)` で取得できる）、セマンティックなエラーコードを返す。理由は 1 つ以上の完全な文であり、何が失敗したかではなく、なぜ失敗したかだけを述べる。例は `You are blocked by bob.` である。操作名はクライアントが前に付ける。CLI は `Error: <Action> failed. <reason>` の形で表示し、例は `Error: Send failed. You are blocked by bob.` である。呼び出しのたびに理由はクリアされ、同一コンテキストでの次の呼び出しまで有効である。エラーコードは型付き列挙型（`shyake_err`）として返され、後方互換のため `SHYAKE_OK = 0` である：

| コード | 意味 |
|---|---|
| `SHYAKE_OK` | 成功 |
| `SHYAKE_ERR` | 汎用／内部エラー |
| `SHYAKE_ERR_NETWORK` | libcurl トランスポート障害 |
| `SHYAKE_ERR_HTTP` | 予期しない HTTP ステータス |
| `SHYAKE_ERR_KEY_MISMATCH` | HTTP 409：受信者の鍵がローテーション済み |
| `SHYAKE_ERR_GONE` | HTTP 410：受信者がアカウントを抹消済み |
| `SHYAKE_ERR_NOT_FOUND` | HTTP 404 |
| `SHYAKE_ERR_FORBIDDEN` | HTTP 403 |
| `SHYAKE_ERR_CRYPTO` | 暗号操作の失敗 |
| `SHYAKE_ERR_NO_INSTANCE` | インスタンス URL が未設定 |
| `SHYAKE_ERR_BLOCKED` | HTTP 403：受信者が送信者をブロックしている |

API グループ：コンテキストのライフサイクル、鍵生成、PoW 生成、登録、メール（`shyake_send`、`shyake_check`、`shyake_fetch`、`shyake_check_one`、`shyake_burn`）、ローカル保存メール（`shyake_save_mail`、`shyake_read_saved`、`shyake_check_saved_one`、`shyake_list_saved`）、アカウント（`shyake_block`、`shyake_list_blocks`、`shyake_rotate`、`shyake_destroy`）、フィンガープリント（`shyake_fingerprint`）、自己暗号化プリミティブ（`shyake_selfenc_begin`、`shyake_selfdec_new`、`shyake_selfdec_key`、`shyake_selfdec_free`、`shyake_seal_b64`、`shyake_unseal_b64`）、単体ファイル暗号化（`shyake_enc_file`、`shyake_dec_file`）。

下書きと自己更新は CLI 層の機能（`src/cli/`）であり、ライブラリ API には含まれない。下書きのディスク上フォーマット（§3.8）は公開された自己暗号化プリミティブのみで構築されている。他のクライアントはこのフォーマットを再利用してもよいし、独自の方式で下書きを保存してもよい。

共有ライブラリ（`libshyake.so` / `libshyake.dylib`）はサードパーティの FFI 利用者向けである。CLI バイナリは単一ファイル配布のため、静的アーカイブ（`libshyake.a`）にリンクされる。

---

### 9. ローカル設定

設定ディレクトリ：`~/.config/shyake/`（デフォルト）、または `-c` / `--config` で指定するカスタムパス。

| ファイル | 内容 |
|---|---|
| `config` | シェル形式の key=value 設定 |
| `kem_pk.bin` / `sig_pk.bin` | 公開鍵（生バイト） |
| `kem_sk.bin` / `sig_sk.bin` | 秘密鍵（生バイトまたは `SHYK`、§3.7） |
| `known_hosts` | 1 行につき `username fingerprint kem_pubkey` |
| `saved/<id>.json` | `shyake save` で保存された暗号化メール |
| `drafts/<id>.json` | `shyake compose` が書き込む暗号化下書き（§3.8） |

`saved/<id>.json` は `GET /api/mail/:id` が返す暗号文 JSON そのままであり、`shyake read` の実行時にのみ復号される。

主な `config` フィールド：

| キー | デフォルト | 説明 |
|---|---|---|
| `INSTANCE` | — | インスタンスのベース URL |
| `USERNAME` | — | 登録済みユーザー名（`register` が設定） |
| `TIME_FORMAT` | `%Y-%m-%d %H:%M` | `strftime` フォーマット |
| `TIME_FORMAT_RECENT` | — | 180 日未満のメール用フォーマット |
| `TIME_ZONE` | `auto` | 整数の時間オフセットまたは `auto` |
| `CHECK_COLUMNS` | `id,sender,subject,size,date` | `check` のレイアウト |
| `NO_COLOR` | `0` | `1` で ANSI カラーを無効化 |
| `DEFAULT_ACTION` | `0` | 0=man、1=check inbox、2=inbox --count |
| `EDITOR` | — | `compose` 用エディタ（`$VISUAL`、`$EDITOR`、`ed` の順にフォールバック） |

認識される環境変数：

| 変数 | 効果 |
|---|---|
| `SHYAKE_PASSPHRASE` | 鍵のパスフレーズを非対話的に供給 |
| `NO_COLOR` | ANSI カラーを無効化（空でない任意の値） |

---

### 10. CLI リファレンス

#### グローバルオプション

| フラグ | 説明 |
|---|---|
| `-c, --config <dir>` | 代替の設定ディレクトリを使用 |
| `--plain` | ページャー、カラー、切り詰めを無効化 |
| `--no-color` | ANSI カラー出力を無効化 |
| `--debug` | 詳細な curl ログを stderr に出力 |

#### コマンド

| コマンド | 説明 |
|---|---|
| `init [-c <dir>]` | 設定ディレクトリと鍵ペアを生成 |
| `register -u <user> -i <url>` | インスタンスに登録 |
| `whoami` | 現在のプロファイルを表示（ネットワーク不使用） |
| `send -t <to> [-s <subj>] [file]` | メールを送信（テキストのみ）。失敗時は下書きとして保存 |
| `send --draft <id> [-t <to>] [-s <subj>]` | 保存済み下書きを送信（成功時に削除） |
| `compose [<id>]` | 暗号化下書きの作成・編集（§3.8） |
| `check inbox\|sent [opts]` | メールボックスのメタデータを一覧表示 |
| `check <id>` | 単一メールのヘッダーを確認 |
| `check saved [<id>]` | ローカル保存メールの一覧／確認 |
| `check drafts [<id>]` | 下書きの一覧／ヘッダー確認 |
| `fetch [-r] <id>` | メールを復号して表示 |
| `save <id>` | 暗号化メールをローカルに保存 |
| `read [-r] <id>` | 保存済みメールを復号して表示 |
| `read [-r] drafts <id>` | 下書きを復号して表示 |
| `burn <id>` | メールを削除（送信者・受信者どちらでも可） |
| `block <target>` | ユーザーまたはドメインをブロック |
| `unblock <target>` | ユーザーまたはドメインのブロックを解除 |
| `blocklist` | ブロック中のユーザーとドメインを一覧表示 |
| `rotate` | 鍵ペアをローテーション（自分の全メールを消去） |
| `fingerprint [<user>] [--update]` | 鍵フィンガープリントを比較 |
| `destroy` | アカウントとローカル設定を抹消 |
| `enc <file> [-t <user>] [-o <out>]` | 単体ファイルを暗号化 |
| `dec <file> [-o <out>]` | 単体ファイルを復号 |
| `update [stable\|preview]` | バージョン表示／自己更新 |
| `man [<command>]` | ドキュメントを表示 |
| `version` | バージョン文字列を表示 |

`check inbox|sent` は `--count`、`--json`、`--csv`、`--no-header` を受け付ける。`send` の宛先はローカル（`username`）またはリモート（`username@instance`）を指定でき、バイナリデータは呼び出し側で base64 エンコードする必要がある。`enc`/`dec` はデバッグとテスト用途を想定している。

---

### 11. サーバー設定

#### 11.1 Worker（`wrangler.toml`）

| 変数 | デフォルト | 説明 |
|---|---|---|
| `INSTANCE_DOMAIN` | — | このインスタンスの正規ドメイン |
| `REGISTRATION_ENABLED` | `true` | 新規ユーザー登録を受け付ける |
| `RESERVED_USERNAMES` | `admin,system,...` | 予約済みの名前（CSV） |
| `FEDERATION_ENABLED` | `true` | フェデレーションメールの受信とリレー |
| `MAX_MAIL_SIZE` | `196608` | `POST /api/mail` の最大ペイロードバイト数 |

必須のバインディング：

| バインディング | 型 | 用途 |
|---|---|---|
| `DB` | D1 データベース | ユーザー、メール、ブロック（§4） |
| `VERSION_CACHE` | KV ネームスペース | リリース検索キャッシュ（§12） |

`CompiledWasm` ビルドルールが `mldsa65-wasm` モジュールをロードする。

`wrangler.toml` は `server/cf/deploy.sh` が `wrangler.template.toml` から生成するもので、git の管理対象ではない。運用者自身のドメインとリソース id を保持する。

#### 11.2 Go サーバー（環境変数）

| 変数 | デフォルト | 説明 |
|---|---|---|
| `SHYAKE_INSTANCE_DOMAIN` | — | このインスタンスの正規ドメイン（必須） |
| `SHYAKE_LISTEN` | `127.0.0.1:8787` | 待ち受けアドレス |
| `SHYAKE_DATABASE` | `shyake.db` | SQLite ファイルのパス |
| `SHYAKE_REGISTRATION_ENABLED` | `true` | 新規ユーザー登録を受け付ける |
| `SHYAKE_RESERVED_USERNAMES` | `admin,system,...` | 予約済みの名前（CSV） |
| `SHYAKE_FEDERATION_ENABLED` | `true` | フェデレーションメールの受信とリレー |
| `SHYAKE_MAX_MAIL_SIZE` | `196608` | 最大ペイロードバイト数（最大 `786432`） |
| `SHYAKE_TRUSTED_PROXIES` | `127.0.0.1/32,::1/128` | `X-Forwarded-For` を信頼するプロキシ |
| `SHYAKE_RATE_LIMIT` | `5` | クライアントアドレスごとの毎秒リクエスト数 |
| `SHYAKE_RATE_BURST` | `30` | クライアントアドレスごとのバースト許容量 |
| `SHYAKE_LOG_FORMAT` | `text` | `text` または `json` |
| `SHYAKE_FEDERATION_INSECURE` | `false` | テスト専用：平文 HTTP とプライベートアドレスでのフェデレーションを許可する |

Go サーバーは起動時にデータベースのマイグレーションを適用する。

---

### 12. リリースチャネルと自己更新

リリースは GitHub で 2 つのチャネルで公開される：**stable**（通常リリース）と **preview**（プレリリース）。サーバーエンドポイント `GET /api/client/version` は GitHub Releases API をプロキシし、各チャネルの最新タグを選択して、結果を 1 時間キャッシュする（§5.1）。

`shyake update` はこのエンドポイントを**ユーザー自身のインスタンス**（profile 設定の `INSTANCE`）から取得する。したがって各インスタンスが自身のキャッシュで GitHub API を中継する。`shyake.eee.coffee` は組み込みのフォールバックにすぎず、インスタンスが未設定の場合にのみ使用される。タグは semver 順序（`vX.Y.Z`、同じベースバージョンではリリースがプレリリースより上位）で比較される。preview チャネルは stable より新しい場合にのみ提示される。

`shyake update stable|preview` は自己更新を実行する：

1. OS／アーキテクチャに一致するリリースアセット（`shyake-<os>-<arch>.tar.gz`）を GitHub Releases からダウンロードする。
2. アーカイブの SHA-256 を、`/api/client/version` の応答に含まれるそのアセットのダイジェストと照合する（§5.1）。不一致の場合、または応答にそのアセットのダイジェストがない場合は中止する。
3. アーカイブを展開し、実行中のバイナリをその場で置き換える（パスは `/proc/self/exe`、`_NSGetExecutablePath`、または `which shyake` で解決）。

#### 12.1 バージョン

リポジトリのバージョン系列は 1 本だけで、それがリリースタグである。各コンポーネントは、最後に変更されたリリースのバージョンを記録する：

- クライアント：`client/Makefile` の `VERSION`。
- サーバー：`server/VERSION`。Worker と Go サーバーはこれを共有する。両者はすべてのリクエストに同じ応答を返さなければならないからである。

リリースは、バージョンがそのタグと一致するコンポーネントだけをビルドする。クライアントだけを変更したリリースではサーバーを再ビルドせず、サーバーは古いバージョンのままとなる。サーバーを変更したリリースでは `server/VERSION` を新しいタグにする。

リリースアセット：

| アセット | 内容 |
|---|---|
| `shyake-<os>-<arch>.tar.gz` | クライアント |
| `shyake-server-linux-<arch>.tar.gz` | Go サーバー（`amd64`、`arm64`）、その systemd unit、設定例 |

サーバーのアセットしか含まないリリースは、クライアントに提示されない（§5.1）。
