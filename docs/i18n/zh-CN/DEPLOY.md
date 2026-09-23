## Shyake 部署指南

[English](../../DEPLOY.md) | 简体中文 | [日本語](../ja/DEPLOY.md)

> Translated by Claude Fable 5

Shyake 的服务端有两个实现，提供同一套 HTTP API。客户端可以连接其中任意一个，两种实例之间也能互相联邦。

* **使用 Cloudflare**：`server/cf/` 中的 Worker，运行在 Cloudflare Workers 上，使用 D1 数据库。不需要自己的机器。
* **自托管**：`server/go/` 中的 Go 服务端，单个二进制文件加单个 SQLite 文件，运行在你自己的机器上。

**联邦网络**

两个实例都启用联邦网络（默认启用）时，它们会自动互相通信，无需额外配置。跨实例邮件以 server-to-server 的方式直接路由；客户端始终只与自己的实例通信。

要禁用入站和出站的联邦网络通信，在 `wrangler.toml` 中设置 `FEDERATION_ENABLED = false`（Worker），或设置 `SHYAKE_FEDERATION_ENABLED=false`（Go 服务端）。

### 使用 Cloudflare

全部操作都在你自己的机器上通过 Wrangler CLI 完成。不需要 fork 本仓库，不需要把仓库连接到 Cloudflare，也不需要在控制台里点来点去。

前提条件：

- Node.js 18+
- 一个 Cloudflare 账户

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/cf
./deploy.sh
```

`deploy.sh` 会完成整个部署流程：

1. 安装 Worker 的依赖
2. 如果尚未认证，运行 `npx wrangler login`
3. 询问你的实例域名
4. 创建 D1 数据库和 KV 缓存命名空间；若已存在则直接复用
5. 把得到的资源 id 写入 `server/cf/wrangler.toml`
6. 应用数据库迁移
7. 部署 Worker 并检查 `/health`

实例域名会嵌入到你实例上的每一个地址中（`user@your.domain.example`），其他实例也依靠它把联邦邮件路由回你这里。如果你没有自定义域名，默认的 `*.workers.dev` 地址同样可用。

选项：

| 选项 | 作用 |
|---|---|
| `--domain <d>` | 非交互式地指定实例域名 |
| `--update` | 先拉取最新代码，再重新部署 |
| `--no-kv` | 跳过 KV 版本缓存 |
| `--config-only` | 只生成 `wrangler.toml` 然后退出 |
| `--local` | 配置本地开发服务器（见 [DEV.md](DEV.md)） |

#### 升级

```sh
cd shyake/server/cf
./deploy.sh --update
```

它会拉取最新代码、应用新增的迁移并重新部署。已有资源会被复用，你的配置也会保留。它还会根据 `server/VERSION` 设置 `GET /api/version` 报告的版本号。

**升级到 v0.3.0。** 从 v0.3.0 起，客户端会对 block、unblock 和 rotate 请求的请求体签名（协议级别 2，见 [SPEC.md §3.3](SPEC.md)）。v0.3.0 客户端无法在旧服务端上执行这三个操作，旧客户端也无法在 v0.3.0 服务端上执行。请先升级服务端，客户端再用 `shyake update` 更新。Go 服务端同样适用。

#### 修改配置

`server/cf/wrangler.toml` 由 `wrangler.template.toml` 在首次运行时生成，并且**不受 git 跟踪**，因此你的实例配置能在 `git pull` 后保留，也永远不会产生冲突。编辑它，然后重新运行 `./deploy.sh`：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example"
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB；不要超过 786432（768 KiB）
```

重新运行脚本不会覆盖这些设置，它只会补上仍未填写的资源 id。

如果你的实例是在 `wrangler.toml` 改为生成式之前部署的，`./deploy.sh --update` 会先把你的配置另存为 `wrangler.toml.bak`，拉取代码后再恢复回来。

### 自托管

Go 服务端运行在你自己的机器上。它是单个静态二进制文件 `shyake-server`，所有数据保存在一个 SQLite 文件中。不需要 Node.js，也不需要 Cloudflare 账户。

前提条件：

- 一台保持在线的机器。下面的示例假设是带 systemd 的 Linux。
- 用于构建的 Go 1.26 或更新版本，或者 Docker。
- 若要参与联邦网络：一个指向该机器的公网域名，以及一个带有效 TLS 证书的反向代理（第 4 步）。

步骤：

1. **获取二进制文件**。可以下载发布版，也可以从源码构建。

下载：改动了服务端的发布会在 [Releases 页面](https://github.com/salmonization/shyake/releases)附带 `shyake-server-linux-amd64.tar.gz` 和 `shyake-server-linux-arm64.tar.gz`；只改动客户端的发布没有这两个文件。请使用带有它们的最新发布。

```sh
tar -xzf shyake-server-linux-amd64.tar.gz
cd shyake-server-linux-amd64
```

压缩包里有二进制文件、`shyake-server.service` 和 `shyake.env.example`。

从源码构建：

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/go
CGO_ENABLED=0 go build -trimpath -ldflags "-X main.version=$(cat ../VERSION)" \
    -o shyake-server ./cmd/shyake-server
```

`-ldflags` 设置 `shyake-server -version` 和 `GET /api/version` 报告的版本号；不加时版本号为 `dev`。

两种方式得到的都是静态二进制文件，可以复制到任何 CPU 架构相同的 Linux 机器上运行。

2. **用 systemd 安装**。先为服务创建一个系统用户，再安装文件。源码中这些文件位于 `server/go/deploy/`；如果用的是发布版压缩包，把下面命令里的 `deploy/` 去掉即可：

```sh
sudo useradd --system --home-dir /var/lib/shyake --shell /usr/sbin/nologin shyake
sudo install -m 755 shyake-server /usr/local/bin/
sudo install -D -m 640 -g shyake deploy/shyake.env.example /etc/shyake/shyake.env
sudo install -m 644 deploy/shyake-server.service /etc/systemd/system/
```

3. **配置**。编辑 `/etc/shyake/shyake.env`，至少设置实例域名：

```sh
SHYAKE_INSTANCE_DOMAIN=your.domain.example
SHYAKE_LISTEN=127.0.0.1:8787
```

实例域名会嵌入到你实例上的每一个地址中（`user@your.domain.example`），其他实例也依靠它把联邦邮件路由回你这里。该文件列出了其余所有设置及其默认值，完整说明见 [SPEC.md §11.2](SPEC.md)。

然后启动服务：

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now shyake-server
curl http://127.0.0.1:8787/health
```

返回 `200 OK` 即表示服务端和数据库工作正常。服务以 `shyake` 用户运行，只能写入存放数据库的 `/var/lib/shyake`。

4. **配置带 TLS 的反向代理**

这一步是**联邦网络所必需的**。实例之间总是通过 `https://<domain>/...` 互相联系，因此你的实例必须能通过 `https://your.domain.example` 访问，并且持有其他实例认可的证书。自签名证书无效。如果你的实例是私有的（其用户只在彼此之间通信），可以跳过这一步，让客户端通过明文 HTTP 连接。

使用 [Caddy](https://caddyserver.com/) 时，证书会自动获取并续期，整个 `Caddyfile` 只需：

```
your.domain.example {
    reverse_proxy 127.0.0.1:8787
}
```

使用由 certbot 管理证书的 nginx 同样可行，把 `https://your.domain.example` 代理到 `http://127.0.0.1:8787` 即可。

服务端按客户端地址限制请求速率。在代理之后时，它从 `X-Forwarded-For` 读取客户端地址，但仅当代理的地址在 `SHYAKE_TRUSTED_PROXIES` 中时才会这样做。默认只信任同一台机器上的代理（`127.0.0.1`、`::1`）。如果你的代理在别处运行，请加入它的地址；否则所有客户端都会被视为代理的地址，共用同一个速率限制。

#### 用 Docker 运行

```sh
docker build --build-arg VERSION=$(cat server/VERSION) \
    -t shyake-server server/go
docker run -d --name shyake --restart unless-stopped \
    -p 127.0.0.1:8787:8787 -v shyake:/data \
    -e SHYAKE_INSTANCE_DOMAIN=your.domain.example \
    -e SHYAKE_TRUSTED_PROXIES=172.16.0.0/12 \
    shyake-server
```

数据库位于 `shyake` 卷上的 `/data/shyake.db`。容器看到的反向代理地址是 Docker 网桥地址，因此要像上面那样把 `SHYAKE_TRUSTED_PROXIES` 设为网桥网段。

#### 升级

从更新的发布版压缩包或重新构建中安装新的二进制文件：

```sh
cd shyake
git pull
cd server/go
CGO_ENABLED=0 go build -trimpath -ldflags "-X main.version=$(cat ../VERSION)" \
    -o shyake-server ./cmd/shyake-server
sudo install -m 755 shyake-server /usr/local/bin/
sudo systemctl restart shyake-server
curl http://127.0.0.1:8787/api/version
```

服务端启动时会自动执行新的数据库迁移。重启期间正在进行的发送会失败，客户端会把它保存为草稿。另请参阅上文的**升级到 v0.3.0**。

#### 数据位置与备份

所有数据都在一个 SQLite 文件中：systemd 下为 `/var/lib/shyake/shyake.db`，Docker 中为 `/data/shyake.db`。数据库运行在 WAL 模式下，因此服务运行期间旁边还有两个文件（`-wal`、`-shm`）。

要备份运行中的服务端，请使用 SQLite 的在线备份，它在写入期间也是安全的：

```sh
sudo sqlite3 /var/lib/shyake/shyake.db ".backup /root/shyake-backup.db"
```

或者先停止服务，再复制这三个文件。

#### 从 Worker 迁移

Go 服务端可以接管 Worker 实例的数据：用户、邮件和屏蔽记录。请保持实例域名不变，因为存储的地址依赖于它。

如果 Worker 部署在 Cloudflare 上，先导出数据库：

```sh
cd shyake/server/cf
npx wrangler d1 export shyake-db --remote --output=d1-export.sql
```

如果是本地的 `wrangler dev` 实例，先停止它，然后直接使用它的数据库文件：`server/cf/.wrangler/state/v3/d1/miniflare-D1DatabaseObject/` 下不叫 `metadata.sqlite` 的那个 `.sqlite` 文件。该文件运行在 WAL 模式下，大部分数据都在旁边的 `-wal` 文件里。如果要复制数据库，请把 `-wal` 文件一起复制。

然后在服务首次启动之前，以 `shyake` 用户导入到一个新的数据库中。该用户必须能读取导出文件：

```sh
sudo install -d -o shyake -g shyake -m 700 /var/lib/shyake
sudo install -o shyake -m 600 d1-export.sql /var/lib/shyake/
# 若使用 D1 文件：请同时安装 <file>.sqlite 和 <file>.sqlite-wal
sudo -u shyake env \
    SHYAKE_INSTANCE_DOMAIN=your.domain.example \
    SHYAKE_DATABASE=/var/lib/shyake/shyake.db \
    shyake-server -import-d1 /var/lib/shyake/d1-export.sql
sudo rm /var/lib/shyake/d1-export.sql
```

导入命令会拒绝已有用户的数据库。无法原样复制的内容会在输出中报告：

- 只有大小写不同的两个用户名：较早注册的账户保留该名字。Go 服务端不允许这样的用户名并存。
- 以同一签名存储了两次的邮件：只保留一份。

它还会把屏蔽记录改写为归一化形式（[SPEC.md §4](SPEC.md)）。

最后把域名指向新机器即可。客户端无需任何修改：它们的密钥和地址都保持不变。

#### 数据库

Go 服务端目前只支持 SQLite。存储层位于一个接口之后，并配有一套所有后端都必须通过的测试。PostgreSQL 的支持将在此基础上实现。在此之前，服务端会拒绝 `SHYAKE_DATABASE` 中的 `postgres://` 值。
