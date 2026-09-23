## Shyake 技术规范

[English](../../SPEC.md) | 简体中文 | [日本語](../ja/SPEC.md)

> Translated by Claude Fable 5

Copyright (c) 2026 Salmonization. BSD 2-Clause License.

<table>
<tr><td>版本</td><td>0.3</td></tr>
<tr><td>最后更新</td><td>2026-09-23</td></tr>
</table>

---

### 1. 概述

Shyake 是一个后量子、端到端加密的异步邮件系统，配备一个 POSIX 风格的 CLI 客户端，旨在提供一种去中心化，抗审查，防嗅探的文本通信方式。

关键特性：

- **端到端加密**：服务器从不持有明文。所有消息内容在传输前均在客户端加密。
- **后量子密码学**：密钥封装使用 ML-KEM-768；身份认证使用 ML-DSA-65（CRYSTALS-Dilithium），两者均来自 [liboqs](https://github.com/open-quantum-safe/liboqs)。
- **去中心化**：任何运营者都能以几乎为零的成本托管自己的实例。实例之间可选地通过 server-to-server 的中继模型进行联邦网络通信。
- **无状态服务器**：服务器只存储密文和公钥。
- **静态加密的密钥**：客户端私钥可选地通过 passphrase（scrypt + ChaCha20-Poly1305）在磁盘上受到保护。

---

### 2. 架构

#### 2.1 组件

```
shyake/
├── client/                 # C 客户端
│   ├── src/lib/            # 核心逻辑（网络、密码学、邮件、
│   │                       #   账户、passphrase）
│   ├── src/cli/            # CLI 解析、显示、提示、配置、
│   │                       #   草稿、自更新
│   ├── include/shyake.h    # 公开 API（不透明指针）
│   ├── tests/              # 库测试程序
│   └── Makefile
├── server/
│   ├── cf/                 # Cloudflare Worker
│   │   ├── src/index.ts    # Hono 路由
│   │   ├── src/utils.ts    # 辅助函数（PoW、用户名校验）
│   │   ├── migrations/     # D1 模式迁移
│   │   └── wrangler.template.toml  # Worker 配置
│   └── go/                 # Go 服务端（自托管）
│       ├── cmd/shyake-server/      # 入口
│       └── internal/       # protocol、store、api、federation
└── docs/
```

#### 2.2 客户端

- **标准**：C11，POSIX.1-2008（`_POSIX_C_SOURCE=200809L`）
- **构建系统**：GNU Make；跨平台（macOS、GNU/Linux、Termux）
- **产物**：
  - `bin/shyake`: CLI 二进制文件，静态链接 `libshyake.a`
  - `lib/libshyake.a`: 静态库
  - `lib/libshyake.so` / `libshyake.dylib`: 用于 FFI 的共享库
- **依赖**：
  - `liboqs`（始终静态链接）: ML-KEM 与 ML-DSA
  - `libcurl`: HTTP 传输
  - `libcrypto`（OpenSSL）: SHA-256 指纹、SHA-1 (PoW)、ChaCha20-Poly1305 AEAD、scrypt KDF (`EVP_PBE_scrypt`)
  - `cJSON`（内置）: JSON 解析

#### 2.3 服务端

服务端有两个实现，提供同一套 HTTP API（§5）。客户端可以连接其中任意一个，两种实例之间也能互相联邦。

**Worker**（`server/cf/`），部署在 Cloudflare 上：

- **运行时**：Cloudflare Workers
- **框架**：[Hono](https://hono.dev/)
- **数据库**：Cloudflare D1（SQLite）
- **签名验证**：ML-DSA-65 编译为 WebAssembly（`mldsa65-wasm`），通过 Wrangler 的 `CompiledWasm` 规则加载。

**Go 服务端**（`server/go/`），用于自托管：

- **运行时**：单个静态二进制文件 `shyake-server`
- **数据库**：单个 SQLite 文件。PostgreSQL 已在规划中，将接在同一个存储接口之后。
- **签名验证**：纯 Go 实现的 ML-DSA-65，来自 [circl](https://github.com/cloudflare/circl)。

---

### 3. 密码学设计

#### 3.1 密钥对

每个用户通过 `liboqs` 在本地生成两组独立的密钥对：

| 用途 | 算法 | 文件 |
|---|---|---|
| 密钥封装 | ML-KEM-768 | `kem_pk.bin`、`kem_sk.bin` |
| 认证/签名 | ML-DSA-65 | `sig_pk.bin`、`sig_sk.bin` |

公钥以原始字节存储，并在注册时上传到服务器。设置 passphrase 时，私钥以 §3.7 描述的静态加密格式存储；未设置 passphrase 时，以原始字节存储。

#### 3.2 消息加密

1. 生成一个随机的 256 位对称密钥。
2. 使用该密钥通过 **ChaCha20-Poly1305** 加密 `subject` 和 `body`，各自使用独立的随机 96 位 nonce。每段密文以 `base64(nonce || ciphertext || tag)` 形式传输。
3. 向**收件人的 ML-KEM 公钥**进行封装：KEM 封装产生一段 KEM 密文和一个 32 字节共享密钥；对称密钥与共享密钥异或后附加在其后：`enc_key_recipient = base64(kem_ct || (sym_key XOR ss))`。
4. 对**发件人自己的 ML-KEM 公钥**重复该封装过程 → `enc_key_sender`（使发件人能够读取自己的已发送邮件箱）。

解密是上述过程的逆过程：客户端用自己的 KEM 私钥解封装出共享密钥，将其与加密密钥字段异或以恢复对称密钥，然后解密内容。

独立文件加密命令（`enc` / `dec`）使用相同的 ML-KEM-768 + ChaCha20-Poly1305 构造，采用带长度前缀的二进制容器（`.enc` 文件）。

#### 3.3 认证协议

所有需要认证的操作都用 ML-DSA-65 签名。使用两种承载形式：

**基于请求头**（除注册和邮件提交外的所有认证端点）：

```
X-Shyake-Username:  <username>
X-Shyake-Timestamp: <unix seconds>
X-Shyake-Signature: <base64(ML-DSA-65 signature)>
X-Shyake-Pow:       <Hashcash token>
```

被签名的消息是由 HTTP 方法、端点（含查询字符串）、用户名和时间戳构成的确定性字符串。例如：

```
GET:/api/mail?type=inbox:salmon:1749513600
```

带请求体的请求（`POST /api/rotate`、`POST /api/block`、`DELETE /api/block`）在末尾再加一个冒号和请求体原始字节的 SHA-256（小写十六进制）：

```
POST:/api/block:salmon:1749513600:<请求体的 sha256 十六进制>
```

这样请求体就和签名绑定在一起。否则，能看到传输中请求内容的一方（例如终结 TLS 的代理）可以保留签名、替换请求体：给 rotate 换上自己的公钥，或给 block 换一个目标。协议级别为 2 的服务器（§5.1）在这三个接口上只接受这种格式，对不带摘要的旧格式返回 `401`。

**基于请求体**（`POST /api/register` 和 `POST /api/mail`）：签名和 PoW 令牌作为请求体的 JSON 字段传输。被签名的消息是以下载荷子集的紧凑 JSON 序列化（字段顺序与客户端产生的一致）：

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

完整请求体还携带 `enc_key_sender`、`enc_key_recipient`、`signature` 和 `pow`，它们不属于被签名的子集。

服务器用发件人的 `sig_pubkey` 验证签名。公钥从本实例的数据库读取；对于联邦网络邮件，则从发件人所在实例获取。

#### 3.4 防重放

每条被签名的消息中都包含时间戳。服务器拒绝时间戳与服务器时间偏差超过 **300 秒**（5 分钟）的请求。

Go 服务端还会记住每个已接受的签名，重复使用同一签名的请求会被以 HTTP 403 拒绝。`POST /api/mail` 是例外：重复的签名不算错误，服务器返回 `201` 和已存储邮件的 id，不会重复存储。因此客户端重试是安全的。

#### 3.5 工作量证明（PoW）

每个认证请求（包括读取操作）都需要一个 Hashcash-v1 风格的 PoW 令牌，SHA-1 难度为 **20 位**：

```
1:<bits>:<yymmdd>:<resource>::<rand>:<counter-hex>
```

`resource` 为操作用户：注册和请求头认证时是用户名，发送邮件时是 `sender` 字段。令牌在客户端铸造，并在签名验证或任何数据库操作之前在服务端完成验证。出现以下任一情况时，服务器拒绝该令牌：

- resource 不是操作用户；
- 日期与服务器日期（UTC）相差超过一天；
- SHA-1 哈希的前 20 位不全为零。

联邦网络发件人的 resource 可能含有冒号（`alice@host:8787`），因此服务器按位置取前三个字段和后三个字段，两者之间的部分即为 resource。

Go 服务端还会拒绝已经接受过的令牌，因此每个令牌只能用于一次请求。

#### 3.6 密钥指纹

指纹是原始（解码后）ML-KEM 公钥字节的 **SHA-256** 小写十六进制编码。客户端将受信任的密钥缓存在 `~/.config/shyake/known_hosts` 中，每行一条以空格分隔的记录：

```
<username> <fingerprint-hex> <kem_pubkey-base64>
```

客户端在每次发送前将收件人的实时密钥与 `known_hosts` 比对，并额外在载荷中嵌入 `recipient_kem_fingerprint`，使服务器可以将其与存储的密钥比对，并以 `KEY_MISMATCH`（HTTP 409）拒绝过期的发送。

#### 3.7 私钥的静态保护

当用户设置了非空 passphrase 时，私钥文件（`kem_sk.bin`、`sig_sk.bin`）以 `SHYK` 容器格式写入：

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 B | 魔数 `"SHYK"` |
| 4 | 1 B | 版本 `0x01` |
| 5 | 1 B | KDF 标识 `0x01`（scrypt） |
| 6 | 32 B | 盐（随机） |
| 38 | 4 B | scrypt `N`（LE u32，默认 65536） |
| 42 | 4 B | scrypt `r`（LE u32，默认 8） |
| 46 | 4 B | scrypt `p`（LE u32，默认 1） |
| 50 | 12 B | ChaCha20-Poly1305 nonce（随机） |
| 62 | — | 密文（与明文密钥长度相同） |
| 末尾 | 16 B | Poly1305 标签 |

62 字节的文件头作为 AAD 绑定，因此对 KDF 参数的任何篡改都会导致认证失败。KDF 从 passphrase 派生出 256 位的 ChaCha20-Poly1305 密钥。

没有 `SHYK` 魔数的文件被视为旧式原始密钥，按原样加载。空 passphrase 会写入原始（未加密）密钥。

passphrase 通过交互式提示输入（终端回显关闭），或通过`SHYAKE_PASSPHRASE` 环境变量非交互式提供。`rotate` 会提示输入当前 passphrase 和新 passphrase；新密钥对仅在服务器确认轮换后才以新 passphrase 保存。

#### 3.8 本地加密草稿

`shyake compose` 将草稿存储在配置目录下的 `drafts/<id>.json` 中。草稿永远不会接触服务器。每份草稿使用与邮件相同的混合方案（§3.2）：随机的 32 字节对称密钥用 ChaCha20-Poly1305 加密各个字段，该密钥再通过 ML-KEM-768 封装到用户自己的 KEM 公钥。

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

收件人、主题和正文在磁盘上全部加密；只有时间戳、大小和 id 是明文。空的 `enc_recipient` / `enc_subject` 字符串表示该字段为空。

由于保存只需要公钥，`compose` 不需要 passphrase；列出、阅读、编辑和发送草稿则需要解锁 KEM 私钥。草稿 id 是本地分配的小整数（现有最大 id + 1，用 `O_EXCL` 创建）。

compose 编辑器操作的明文临时文件由 `mkstemp` 创建（权限 0600），位于配置目录内，绝不使用 `/tmp`。结束后该文件会被零覆写并删除。当编辑器为 `vim`/`nvim` 时，以 `-n -i NONE` 启动，确保明文不会泄漏到 swap 或 viminfo 文件中。

---

### 4. 数据库模式

两个服务端都使用 SQLite：Worker 使用 Cloudflare D1（`server/cf/migrations/`），Go 服务端使用本地文件（`server/go/internal/store/sqlite/migrations/`）。下面的表为两者共有。Go 服务端另外增加了：

- `mail` 表的 `sig_hash` 列：`signature` 的 SHA-256，唯一。用于识别重复提交的邮件（§3.4）。

#### `users`

| 列 | 类型 | 说明 |
|---|---|---|
| `username` | TEXT PK | 正则 `^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$` |
| `kem_pubkey` | TEXT | Base64 编码的 ML-KEM-768 公钥 |
| `sig_pubkey` | TEXT | Base64 编码的 ML-DSA-65 公钥 |
| `created_at` | INTEGER | UNIX 时间戳 |

用户名在**忽略大小写**的意义上唯一：当一个名字与已有用户名只有大小写差异时，注册会被以 HTTP 409 拒绝，因此 `Alice` 不能与 `alice` 并存。其他地方的查找都是精确匹配，两者不会被混淆。

执行 `destroy` 时，`kem_pubkey` 和 `sig_pubkey` 被置为空字符串，所有与该用户相关的邮件和屏蔽记录都会被删除。用户行本身**被保留**以永久锁定该用户名。

#### `mail`

| 列 | 类型 | 说明 |
|---|---|---|
| `mail_id` | TEXT PK | 10 字符 base58 字符串，由服务器分配 |
| `sender` | TEXT | 本地用户名，联邦网络邮件为 `user@domain` |
| `recipient` | TEXT | 本地用户名，联邦网络邮件为 `user@domain` |
| `enc_key_sender` | TEXT | 为发件人 KEM 封装的密钥 |
| `enc_key_recipient` | TEXT | 为收件人 KEM 封装的密钥 |
| `enc_subject` | TEXT | ChaCha20-Poly1305 密文，base64 |
| `enc_body` | TEXT | ChaCha20-Poly1305 密文，base64 |
| `size` | INTEGER | 明文正文字节数（仅供界面显示） |
| `signature` | TEXT | 发件人的 ML-DSA-65 签名，base64 |
| `timestamp` | INTEGER | 服务器分配的 UNIX 时间戳 |

本地地址以裸用户名存储：插入前会剥离 `@<INSTANCE_DOMAIN>` 后缀。`recipient` 和 `sender` 上的索引服务于邮件箱查询。`rotate` 会删除该用户作为发件人或收件人的所有邮件行。

#### `blocks`

| 列 | 类型 | 说明 |
|---|---|---|
| `blocker` | TEXT | 执行屏蔽的用户名 |
| `blocked` | TEXT | 用户名、`user@domain` 或裸域名 |
| `created_at` | INTEGER | UNIX 时间戳 |
| PK | | `(blocker, blocked)` 复合主键 |

地址在存储和匹配前会先**归一化**：去掉 `@<INSTANCE_DOMAIN>` 后缀，远端地址的域名部分与裸域名都会转为小写。因此本地用户始终是 `bob`，远端用户始终是 `mallory@evil.example`。

提交邮件时，如果收件人屏蔽了发件人的归一化地址或发件人的域名，服务器将以 HTTP 403 拒绝发送。双方都会先归一化，这正是该检查在转发邮件上仍然有效的原因——转发时收件人是以全限定形式到达的。

---

### 5. HTTP API

两个服务端提供相同的端点，状态码和响应体也相同。基础 URL 为实例域名。

#### 5.1 公开端点

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/health` | 存活检查；查询数据库 |
| `GET` | `/api/pubkey/:username` | 返回 `kem_pubkey`、`sig_pubkey` |
| `GET` | `/api/version` | 服务端版本和协议级别 |
| `GET` | `/api/client/version` | 最新客户端发布标签 |

`/api/pubkey/:username` 支持 `user@domain` 语法；如果域名与本地实例不同，服务器会将请求代理到远程实例（需要启用联邦网络）。

`/api/version` 说明服务端的身份：

```json
{"version": "v0.3.0", "implementation": "go", "protocol": 2}
```

`version` 是 `server/VERSION` 中的版本（§12.1），没有注入版本的构建为 `dev`。`implementation` 为 `cf` 或 `go`。`protocol` 是协议级别：级别 2 会对请求体签名（§3.3）；没有这个端点的服务器视为级别 1。客户端依据协议级别而不是版本号来判断实例接受哪些请求格式。

`/api/client/version` 代理 GitHub Releases API，返回每个渠道中带有客户端构建的最新标签，以及该版本每个发布产物的 SHA-256 摘要：

```json
{
  "release": "vX.Y.Z",
  "release_digests": {"shyake-linux-x86_64.tar.gz": "<hex>"},
  "pre_release": "vX.Y.Z-...",
  "pre_release_digests": {"shyake-linux-x86_64.tar.gz": "<hex>"}
}
```

任一渠道都可能缺失。只含服务端构建（`shyake-server-*`）的版本会被跳过。结果缓存一小时：Worker 缓存在 KV 中，Go 服务端缓存在内存中。参见 §12。

#### 5.2 认证端点

所有端点都验证 PoW 令牌、时间戳窗口和 ML-DSA-65 签名（§3.3）。`POST /api/register` 和 `POST /api/mail` 在 JSON 请求体中携带认证字段；其余端点使用 `X-Shyake-*` 请求头。

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/api/register` | 注册新用户 |
| `POST` | `/api/mail` | 发送邮件 |
| `GET` | `/api/mail?type=inbox\|sent` | 列出邮件箱元数据 |
| `GET` | `/api/mail/:id` | 获取单封邮件（完整密文） |
| `DELETE` | `/api/mail/:id` | 焚毁（删除）邮件 |
| `POST` | `/api/block` | 屏蔽用户或域名 |
| `DELETE` | `/api/block` | 取消屏蔽用户或域名 |
| `GET` | `/api/block` | 返回调用者的屏蔽列表 |
| `POST` | `/api/rotate` | 轮换公钥 |
| `DELETE` | `/api/destroy` | 销毁账户 |

`POST /api/mail` 值得注意的状态码：`409`（`KEY_MISMATCH`，提供的收件人指纹已不匹配）、`410`（`USER_DESTROYED`）、`413`（载荷过大）、`403`（被屏蔽、PoW 无效或时间戳过期）。

Go 服务端还可能返回：

- `429`：任意端点，客户端地址超过速率限制时。
- `403`：请求重复使用了签名或 PoW 令牌时（§3.4、§3.5）。
- `403`：`POST /api/mail` 的收发双方都不属于本实例时。服务器不在另外两个实例之间中继。
- `503`：`POST /api/mail` 查询发件人公钥时发件人实例无响应。
- `502`：`POST /api/mail` 查询收件人公钥时收件人实例无响应。Worker 在这种情况下返回 `404`。

中继失败时，两个服务端都返回 `502` 或远程实例自己的拒绝结果（§6.2）。

#### 5.3 大小限制

服务器对 `POST /api/mail` 的原始 HTTP 请求体强制施加硬性上限。默认为 **196608 字节（192 KiB）**，运维者可以修改（§11）。绝对上限为 786432 字节（768 KiB），由 Cloudflare D1 的单行限制决定。Go 服务端沿用同一上限，以保证它的邮件在联邦网络中仍能被 Worker 实例接受。

---

### 6. 联邦网络

#### 6.1 地址格式

- **本地用户**：`username`（不含 `@`）
- **远程用户**：`username@instance.domain`

客户端始终只与用户自己的实例通信。向远程收件人发送时，客户端会将自己的发件人字符串限定为 `username@<own-domain>`。

#### 6.2 出站邮件中继

当 `recipient` 属于远程实例时：

1. 客户端将签名并加密的载荷提交到**发件人自己的实例**（`POST /api/mail`）。
2. 发件人的实例完成验证后，将原始载荷转发到 `https://<recipientDomain>/api/mail`，并等待响应，最多 15 秒。
3. 远程实例接受邮件（`2xx`）后，发件人的实例才为已发送邮件箱存储自己的副本，并返回 `201`。

中继失败时，发件人的实例不存储任何内容：

- 远程实例拒收（`408`、`429` 以外的 `4xx`）：发件人的实例原样返回该状态码和远程实例的 `error` 文本，例如 `403` 和 `Recipient has blocked this sender`。
- 其他失败（无法连接、超时、`408`、`429`、`5xx`）：发件人的实例返回 `502` 和 `Recipient instance unreachable`。

服务端不排队，也不重试中继。排队也撑不了多久：收件人实例会在发件人签名时间戳 300 秒后拒绝这份载荷（§3.4），而只有客户端能重新签名。发送失败时，客户端把邮件保存为本地草稿（§3.8），用户稍后重发即可，重发时会签一份新的载荷。

收件人的实例独立验证发件人的签名，方法是从发件人的实例获取其公钥（`GET /api/pubkey/<sender>`）。

发件人和收件人的数据库都存储该邮件，因此即使远程实例不可用，发件人的已发送邮件箱也照常可用。

#### 6.3 联邦网络开关

通过 `FEDERATION_ENABLED`（Worker）或 `SHYAKE_FEDERATION_ENABLED`（Go 服务端）配置。设为 `false` 时，实例拒绝解析远程用户，从而同时拒绝传入的中继邮件和传出的跨实例发送。

---

### 7. 信任模型（TOFU + OOB）

Shyake 使用**首次使用信任**（TOFU）进行公钥管理：

- **首次联系**：客户端查询 `GET /api/pubkey/<recipient>`，计算 KEM 指纹，并静默地将其追加到 `~/.config/shyake/known_hosts`。
- **后续联系**：每次发送前，将获取到的密钥与 `known_hosts` 中的条目比对；不匹配时在本地以 `KEY_MISMATCH` 中止，任何数据都不会被传输。
- **服务端二次校验**：载荷中嵌入 `recipient_kem_fingerprint`；如果它与存储的密钥不再匹配，服务器独立地以 HTTP 409 拒绝。
- **检测到密钥轮换**：客户端打印致命错误并停止：

```
FATAL: Remote public key of recipient has changed!
RUN 'shyake fingerprint <username>' to inspect and update trust.
```

`fingerprint` 命令提供**带外（OOB）验证**：它从服务器获取当前公钥，计算指纹，并与 `known_hosts` 比对。输出显示 GPG 风格的十六进制分组以及 OpenSSH 风格的 randomart 图案。在用户通过可信渠道核实新指纹后，`--update` 标志会重写 `known_hosts`。

---

### 8. 客户端库 ABI

`libshyake` 是协议的实现本体，随附的 CLI 只是构建在其之上的一个参考客户端。库中只包含核心的、普适性的逻辑：密码学、线格式编码，以及本规范定义的收发操作。客户端特有的部分（参数解析、显示、交互提示、自更新）位于 `src/cli/`，不得迁入库中。第三方开发者可以仅基于 `libshyake` 构建完全兼容协议的客户端（无论是 TUI、GUI，还是通过 FFI 使用任何语言）。

核心库通过 `include/shyake.h` 暴露稳定的 C API。内部状态隐藏在不透明指针之后，以防止 ABI 破坏：

```c
typedef struct shyake_ctx shyake_ctx;

shyake_ctx* shyake_init_ctx(const shyake_config *config);
void        shyake_free_ctx(shyake_ctx *ctx);

/* passphrase for secret key files (§3.7) */
void shyake_set_passphrase(shyake_ctx *ctx, const char *pp);
void shyake_set_new_passphrase(shyake_ctx *ctx, const char *pp);

/* 本上下文最近一次失败的详细信息，无则为 "" */
const char* shyake_last_error(shyake_ctx *ctx);
```

内部结构体定义位于 `src/lib/lib_internal.h`，不对调用者暴露。库从不向 stdout/stderr 写入任何内容：失败时它会记录一条人类可读的详细信息（通过 `shyake_last_error(ctx)` 获取，在同一上下文的下一次调用前有效），并返回语义化错误码。错误码以类型化枚举（`shyake_err`）返回，其中 `SHYAKE_OK = 0` 以保持向后兼容：

| 错误码 | 含义 |
|---|---|
| `SHYAKE_OK` | 成功 |
| `SHYAKE_ERR` | 通用/内部错误 |
| `SHYAKE_ERR_NETWORK` | libcurl 传输失败 |
| `SHYAKE_ERR_HTTP` | 非预期的 HTTP 状态码 |
| `SHYAKE_ERR_KEY_MISMATCH` | HTTP 409：收件人密钥已轮换 |
| `SHYAKE_ERR_GONE` | HTTP 410：收件人已销毁账户 |
| `SHYAKE_ERR_NOT_FOUND` | HTTP 404 |
| `SHYAKE_ERR_FORBIDDEN` | HTTP 403 |
| `SHYAKE_ERR_CRYPTO` | 密码学操作失败 |
| `SHYAKE_ERR_NO_INSTANCE` | 未配置实例 URL |

API 分组：上下文生命周期、密钥生成、PoW 铸造、注册、邮件（`shyake_send`、`shyake_check`、`shyake_fetch`、`shyake_check_one`、`shyake_burn`）、本地保存的邮件（`shyake_save_mail`、`shyake_read_saved`、`shyake_check_saved_one`、`shyake_list_saved`）、账户（`shyake_block`、`shyake_list_blocks`、`shyake_rotate`、`shyake_destroy`）、指纹（`shyake_fingerprint`）、自加密原语（`shyake_selfenc_begin`、`shyake_selfdec_new`、`shyake_selfdec_key`、`shyake_selfdec_free`、`shyake_seal_b64`、`shyake_unseal_b64`），以及独立文件加密（`shyake_enc_file`、`shyake_dec_file`）。

草稿与自更新属于 CLI 层功能（`src/cli/`），不在库 API 之内。草稿的磁盘格式（§3.8）完全基于公开的自加密原语构建；其他客户端可以复用该格式，也可以用自己的方式存储草稿。

共享库（`libshyake.so` / `libshyake.dylib`）面向第三方 FFI 使用者。CLI 二进制文件链接静态归档（`libshyake.a`）以实现单文件分发。

---

### 9. 本地配置

配置目录：`~/.config/shyake/`（默认），或通过 `-c` / `--config` 指定的自定义路径。

| 文件 | 内容 |
|---|---|
| `config` | Shell 风格的 key=value 设置 |
| `kem_pk.bin` / `sig_pk.bin` | 公钥（原始字节） |
| `kem_sk.bin` / `sig_sk.bin` | 私钥（原始字节或 `SHYK`，§3.7） |
| `known_hosts` | 每行 `username fingerprint kem_pubkey` |
| `saved/<id>.json` | 由 `shyake save` 保存的加密邮件 |
| `drafts/<id>.json` | 由 `shyake compose` 写入的加密草稿（§3.8） |

`saved/<id>.json` 是 `GET /api/mail/:id` 返回的原样密文 JSON；它仅在执行 `shyake read` 时才被解密。

主要的 `config` 字段：

| 键 | 默认值 | 说明 |
|---|---|---|
| `INSTANCE` | — | 实例基础 URL |
| `USERNAME` | — | 已注册的用户名（由 `register` 设置） |
| `TIME_FORMAT` | `%Y-%m-%d %H:%M` | `strftime` 格式 |
| `TIME_FORMAT_RECENT` | — | 用于 180 天内邮件的格式 |
| `TIME_ZONE` | `auto` | 整数小时偏移或 `auto` |
| `CHECK_COLUMNS` | `id,sender,subject,size,date` | `check` 列布局 |
| `NO_COLOR` | `0` | 设为 `1` 以禁用 ANSI 颜色 |
| `DEFAULT_ACTION` | `0` | 0=man，1=check inbox，2=inbox --count |
| `EDITOR` | — | `compose` 使用的编辑器（依次回退到 `$VISUAL`、`$EDITOR`、`ed`） |

识别的环境变量：

| 变量 | 作用 |
|---|---|
| `SHYAKE_PASSPHRASE` | 非交互式地提供密钥 passphrase |
| `NO_COLOR` | 禁用 ANSI 颜色（任意非空值） |

---

### 10. CLI 参考

#### 全局选项

| 标志 | 说明 |
|---|---|
| `-c, --config <dir>` | 使用替代配置目录 |
| `--plain` | 禁用分页器、颜色和截断 |
| `--no-color` | 禁用 ANSI 彩色输出 |
| `--debug` | 输出详细 curl 日志到 stderr |

#### 命令

| 命令 | 说明 |
|---|---|
| `init [-c <dir>]` | 生成配置目录和密钥对 |
| `register -u <user> -i <url>` | 在实例上注册 |
| `whoami` | 打印当前配置文件（无网络请求） |
| `send -t <to> [-s <subj>] [file]` | 发送邮件（仅文本）；失败时存为草稿 |
| `send --draft <id> [-t <to>] [-s <subj>]` | 发送已存草稿（成功后删除） |
| `compose [<id>]` | 撰写或编辑加密草稿（§3.8） |
| `check inbox\|sent [opts]` | 列出邮件箱元数据 |
| `check <id>` | 查看单封邮件的邮件头 |
| `check saved [<id>]` | 列出/查看本地保存的邮件 |
| `check drafts [<id>]` | 列出草稿/查看单份草稿的头部 |
| `fetch [-r] <id>` | 解密并打印邮件 |
| `save <id>` | 在本地存储加密邮件 |
| `read [-r] <id>` | 解密并打印已保存的邮件 |
| `read [-r] drafts <id>` | 解密并打印草稿 |
| `burn <id>` | 删除邮件（发件人或收件人均可） |
| `block <target>` | 屏蔽用户或域名 |
| `unblock <target>` | 取消屏蔽用户或域名 |
| `blocklist` | 列出已屏蔽的用户和域名 |
| `rotate` | 轮换密钥对（清除自己的全部邮件） |
| `fingerprint [<user>] [--update]` | 比对密钥指纹 |
| `destroy` | 销毁账户和本地配置 |
| `enc <file> [-t <user>] [-o <out>]` | 加密独立文件 |
| `dec <file> [-o <out>]` | 解密独立文件 |
| `update [stable\|preview]` | 显示版本/自更新 |
| `man [<command>]` | 显示文档 |
| `version` | 打印版本字符串 |

`check inbox|sent` 接受 `--count`、`--json`、`--csv` 和 `--no-header`。`send` 的收件人可以是当前实例用户（`username`）或外部实例用户（`username@instance`）；二进制数据必须由调用方进行 base64 编码。`enc`/`dec` 用于调试和测试。

---

### 11. 服务端配置

#### 11.1 Worker（`wrangler.toml`）

| 变量 | 默认值 | 说明 |
|---|---|---|
| `INSTANCE_DOMAIN` | — | 本实例的规范域名 |
| `REGISTRATION_ENABLED` | `true` | 接受新用户注册 |
| `RESERVED_USERNAMES` | `admin,system,...` | 保留用户名（CSV） |
| `FEDERATION_ENABLED` | `true` | 接受并中继联邦网络邮件 |
| `MAX_MAIL_SIZE` | `196608` | `POST /api/mail` 最大载荷字节数 |

必需的绑定：

| 绑定 | 类型 | 用途 |
|---|---|---|
| `DB` | D1 数据库 | 用户、邮件、屏蔽记录（§4） |
| `VERSION_CACHE` | KV 命名空间 | 版本查询缓存（§12） |

`CompiledWasm` 构建规则加载 `mldsa65-wasm` 模块。

`wrangler.toml` 由 `server/cf/deploy.sh` 从 `wrangler.template.toml` 生成，不受 git 跟踪：它保存运维者自己的域名和资源 id。

#### 11.2 Go 服务端（环境变量）

| 变量 | 默认值 | 说明 |
|---|---|---|
| `SHYAKE_INSTANCE_DOMAIN` | — | 本实例的规范域名（必填） |
| `SHYAKE_LISTEN` | `127.0.0.1:8787` | 监听地址 |
| `SHYAKE_DATABASE` | `shyake.db` | SQLite 文件路径 |
| `SHYAKE_REGISTRATION_ENABLED` | `true` | 接受新用户注册 |
| `SHYAKE_RESERVED_USERNAMES` | `admin,system,...` | 保留用户名（CSV） |
| `SHYAKE_FEDERATION_ENABLED` | `true` | 接受并中继联邦网络邮件 |
| `SHYAKE_MAX_MAIL_SIZE` | `196608` | 最大载荷字节数，不超过 `786432` |
| `SHYAKE_TRUSTED_PROXIES` | `127.0.0.1/32,::1/128` | 信任其 `X-Forwarded-For` 的代理 |
| `SHYAKE_RATE_LIMIT` | `5` | 每个客户端地址每秒请求数 |
| `SHYAKE_RATE_BURST` | `30` | 每个客户端地址的突发额度 |
| `SHYAKE_LOG_FORMAT` | `text` | `text` 或 `json` |
| `SHYAKE_FEDERATION_INSECURE` | `false` | 仅用于测试：允许通过明文 HTTP 与私有地址进行联邦 |

Go 服务端在启动时自动执行数据库迁移。

---

### 12. 发布渠道与自更新

发布通过 GitHub 分两个渠道进行：**stable**（正式发布）和 **preview**（预发布）。服务端端点 `GET /api/client/version` 代理 GitHub Releases API，选取每个渠道的最新标签，并将结果缓存一小时（§5.1）。

`shyake update` 从**用户自己的实例**（profile 配置中的 `INSTANCE`）获取该端点，因此每个实例都用自己的缓存中继 GitHub API；`shyake.eee.coffee` 只是内置的回退目标，仅在未配置实例时使用。标签使用 semver 排序比较（`vX.Y.Z`；同一基础版本下正式发布高于预发布）。仅当 preview 渠道比 stable 更新时才会提供。

`shyake update stable|preview` 执行自更新：

1. 从 GitHub Releases 下载与操作系统/架构匹配的发布产物（`shyake-<os>-<arch>.tar.gz`）。
2. 将压缩包的 SHA-256 与 `/api/client/version` 响应中该产物的摘要比较（§5.1）。不匹配，或响应中没有该产物的摘要时，中止更新。
3. 解压压缩包并原地替换正在运行的二进制文件（通过 `/proc/self/exe`、`_NSGetExecutablePath` 或 `which shyake` 解析路径）。

#### 12.1 版本号

整个仓库只有一条版本线，即发布标签。每个组件记录自己最后一次改动时所在的版本：

- 客户端：`client/Makefile` 中的 `VERSION`。
- 服务端：`server/VERSION`。Worker 和 Go 服务端共用这个版本号，因为两者必须对每个请求给出同样的回答。

一次发布只构建版本号等于该标签的组件。只改动客户端的发布不会重新构建服务端，服务端保持原来的版本号；改动了服务端的发布会把 `server/VERSION` 设为新标签。

发布产物：

| 产物 | 内容 |
|---|---|
| `shyake-<os>-<arch>.tar.gz` | 客户端 |
| `shyake-server-linux-<arch>.tar.gz` | Go 服务端（`amd64`、`arm64`）、systemd unit 和配置样例 |

只含服务端产物的发布不会推送给客户端（§5.1）。
