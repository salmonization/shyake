## Shyake 开发者指南

[English](../../DEV.md) | 简体中文 | [日本語](../ja/DEV.md)

> Translated by Claude Fable 5

本文档帮助你参与 Shyake 的开发，或二次开发。

**目录**：

- [客户端](#客户端)
  * [依赖](#依赖)
  * [构建](#构建)
  * [安装](#安装)
  * [测试](#测试)
- [服务端](#服务端)
  * [Worker](#worker)
  * [Go 服务端](#go-服务端)

## 客户端

### 依赖

`liboqs` 在所有平台上都是静态链接，因此二进制文件对它没有运行时依赖。`libcurl` 和 `libcrypto` 在所有平台上仍为动态链接。

依赖（仅构建时需要）：

| 库 | 用途 |
|---------|---------|
| `liboqs` | ML-KEM-768 与 ML-DSA-65 |
| `libcurl` | HTTP 传输 |
| `openssl`（`libcrypto`） | SHA-256 指纹计算 |

macOS 上使用 Homebrew：

```sh
brew install liboqs curl openssl@3
```

Arch Linux 上：

```sh
sudo pacman -S cmake curl openssl
# 从源码构建 liboqs：参见下方说明
```

Debian/Ubuntu 上：

```sh
sudo apt install cmake libcurl4-openssl-dev libssl-dev
# 从源码构建 liboqs：参见下方说明
```

Termux（Android）上：

```sh
pkg install clang cmake make curl-dev openssl-dev
# 从源码构建 liboqs：参见下方说明
```

**构建 liboqs**

从源码编译 `liboqs` 时（例如在 GNU/Linux 或 Termux 上），必须进行最小化构建。启用全部算法构建 `liboqs` 会使二进制体积急剧膨胀（约 20MB）。

只构建 Shyake 所需算法（ML-KEM-768 和 ML-DSA-65）的 `liboqs`，运行：

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

在 **Termux** 中编译时，必须指定安装前缀（`$PREFIX`）并省略
`sudo`：

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

### 构建

```sh
cd client
make
```

产物：

| 文件 | 说明 |
|------|-------------|
| `bin/shyake` | CLI 二进制文件（所有平台均静态链接 `liboqs`） |
| `lib/libshyake.a` | 用于 FFI 的静态库 |
| `lib/libshyake.so` 或 `lib/libshyake.dylib` | 用于 FFI 的共享库 |

### 安装

将二进制文件复制到 `$PATH` 中的任意目录：

```sh
cp bin/shyake /usr/local/bin/
```

### 测试

针对本地服务端运行端到端测试套件。两个服务端都可以，涉及协议的改动必须对两者都通过：

```sh
# 终端 1：Worker
cd server/cf && npx wrangler dev --local
# 或 Go 服务端
cd server/go && SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 go run ./cmd/shyake-server

# 终端 2
cd client && make
bash tests/e2e_test.sh
```

`SHYAKE_TEST_INSTANCE` 可以让测试套件指向其他 URL。

联邦网络测试会自行启动两个 Go 服务端，并在它们之间运行客户端。它覆盖双向中继、发往已停机实例的中继（客户端保存草稿，实例恢复后再发送），以及对中继邮件的屏蔽：

```sh
cd client && make && cd ..
bash tests/federation_test.sh
```

### 非交互式 passphrase

设置 `SHYAKE_PASSPHRASE` 可以跳过所有需要解锁密钥场景下的交互式提示（`init` 也会用它作为初始 passphrase）。这是为了方便脚本化测试，并非面向普通用户。内联写在命令里的值会进入 shell 历史，导出的环境变量对子进程可见。

```sh
export SHYAKE_PASSPHRASE=$(openssl rand -base64 12)
shyake init
shyake check inbox
```

## 服务端

### Worker

```sh
cd server/cf
./deploy.sh --local      # 依赖、wrangler.toml、本地数据库
npx wrangler dev --local
```

Worker 默认监听 `http://localhost:8787`。

`wrangler.toml` 由 `wrangler.template.toml` 生成，不受 git 跟踪。部署到 Cloudflare 用的是同一个脚本，只是不带 `--local`；见 [DEPLOY.md](DEPLOY.md)。

### Go 服务端

需要 Go 1.26 或更新版本。服务端不依赖 cgo。

```sh
cd server/go
go test ./...                    # 单元测试
go vet ./...
gofmt -l .                       # 应无任何输出
SHYAKE_INSTANCE_DOMAIN=127.0.0.1:8787 SHYAKE_DATABASE=/tmp/dev.db \
    go run ./cmd/shyake-server
```

包的结构，由外向内：

| 包 | 用途 |
|---|---|
| `internal/protocol` | 地址、PoW、签名、被签名的消息。不做 I/O。 |
| `internal/api` | HTTP 处理器、认证、速率限制 |
| `internal/federation` | 出站客户端、远程公钥缓存、中继 |
| `internal/store` | 存储接口及其后端测试套件 |
| `internal/store/sqlite` | SQLite 后端及其迁移 |
| `internal/config` | `SHYAKE_*` 环境变量设置 |

**签名测试向量。** `internal/protocol/testdata/liboqs_vectors.json` 保存了由 liboqs 对客户端自带 cJSON 构建的消息所做的签名。Go 测试检查 circl 能否接受这些签名，以及服务端能否逐字节地重建每条被签名的消息。修改被签名的消息后，请重新生成该文件：

```sh
cd server/go/internal/protocol/testdata
cc -std=c11 -o /tmp/gen gen_vectors.c \
   ../../../../../client/src/lib/vendor/cJSON/cJSON.c \
   -I../../../../../client/src/lib/vendor/cJSON \
   /usr/local/lib/liboqs.a -lcrypto
/tmp/gen > liboqs_vectors.json
```

**新的存储后端**需要实现 `store.Store`，并通过 `storetest.Run`——SQLite 后端运行的是同一套测试。PostgreSQL 就按这种方式规划。SQL 只应出现在后端的包内：接口只描述用户、邮件和屏蔽。

**在一台机器上测试联邦网络。** 实例之间通过 HTTPS 通信，服务端也会拒绝私有地址。本地测试时，`SHYAKE_FEDERATION_INSECURE=true` 允许使用明文 HTTP 和回环地址。切勿在公开实例上设置它。

## 发布

整个仓库只有一条版本线，即发布标签（[SPEC.md §12.1](SPEC.md)）。每个组件记录自己最后一次改动时所在的版本：

- 客户端：`client/Makefile` 中的 `VERSION`。
- 两个服务端：`server/VERSION`。

发布步骤：

1. 把每个改动过的组件的版本号设为新标签，没改动的组件保持原版本号。
2. 用这个标签在 GitHub 上发布 release。

`client/Makefile` 的版本等于标签时，发布流程构建客户端；`server/VERSION` 等于标签时，构建 Go 服务端。两者都不等于标签时，流程直接失败。

影响客户端的服务端改动必须先部署到服务端。服务端开始接受新的请求格式时，要提升协议级别（Go 服务端的 `protocol.Level`、Worker 的 `PROTOCOL_LEVEL`）。客户端通过 `GET /api/version` 读取它。
