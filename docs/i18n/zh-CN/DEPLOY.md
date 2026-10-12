## Shyake 部署指南

[English](../../DEPLOY.md) | 简体中文 | [日本語](../ja/DEPLOY.md)

> Translated by Claude

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
| `--update` | 先切换到最新的正式版，再重新部署 |
| `--no-kv` | 跳过 KV 版本缓存 |
| `--config-only` | 只生成 `wrangler.toml` 然后退出 |
| `--local` | 配置本地开发服务器（见 [DEV.md](DEV.md)） |

#### 升级

```sh
cd shyake/server/cf
./deploy.sh --update
```

它会拉取所有发布 tag，切换到最新的正式版（`vX.Y.Z`），跳过预发布版本和尚未发布的代码，然后应用新增的迁移并重新部署。切换后 Git 会提示处于 "detached HEAD" 状态，这是正常的。已有资源会被复用，你的配置也会保留。它还会根据 `server/VERSION` 设置 `GET /api/version` 报告的版本号。

**升级到 v0.4.0。** v0.4.0 新增回复功能（协议级别 3，见 [SPEC.md §3.2](SPEC.md)），并给 `mail` 表增加一列。`deploy.sh --update` 会执行该迁移，Go 服务端在启动时执行。只有级别 3 的实例接受回复，其他邮件在所有版本之间照常收发。请先升级服务端，再让客户端发送回复。

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

如果你的实例是在 `wrangler.toml` 改为生成式之前部署的，`./deploy.sh --update` 会先把你的配置另存为 `wrangler.toml.bak`，切换到新版本后再恢复回来。

### GitHub token（建议配置）

`shyake update` 会向你的实例查询最新版本，实例再去调用 GitHub API。不带 token 时，GitHub 对每个 IP 每小时只允许 60 次调用。Cloudflare Workers 共用出口 IP，额度很容易被其他 Worker 用光，这时 `shyake update` 就会失败。配上 token 后，你的实例有自己独立的额度。

1. 在 GitHub 上打开 **Settings > Developer settings > Personal access tokens > Fine-grained tokens**，点击 **Generate new token**。
2. 在 **Repository access** 中选择 **Public repositories**，不要添加任何权限。
3. 设置过期时间，生成 token 并复制。
4. 把 token 交给实例：
   - Worker：在 `server/cf` 中运行 `npx wrangler secret put GITHUB_TOKEN`，粘贴 token，立即生效。
   - Go 服务端：在 `/etc/shyake/shyake.env` 中加入 `SHYAKE_GITHUB_TOKEN=<token>`，然后运行 `sudo systemctl restart shyake-server`。

GitHub 拒绝请求时，实例会返回上一次成功获取的结果，并在日志里记录 GitHub 返回的状态：Worker 用 `npx wrangler tail` 查看，Go 服务端用 `journalctl -u shyake-server` 查看。看到 `401` 说明 token 已过期，重新创建一个并重复第 4 步即可。

### 自托管

Go 服务端运行在你自己的机器上。它是单个静态二进制文件 `shyake-server`，所有数据保存在一个 SQLite 文件中。不需要 Node.js，也不需要 Cloudflare 账户。

前提条件：

- 一台保持在线的机器。下面的示例使用带 systemd 的 Linux。
- 若要参与联邦网络：一个指向该机器的公网域名，以及一个带有效 TLS 证书的反向代理。

用安装脚本安装发布版构建，或者从源码构建。从源码构建更稳妥：你运行的是自己能读到的代码，不需要信任构建机器。之后配置反向代理。

#### 安装发布版构建

运行这条命令：

```sh
curl -fsSL https://raw.githubusercontent.com/salmonization/shyake/main/server/go/deploy/install.sh | sh
```

脚本会询问发布渠道。`stable` 只获取正式版，`preview` 也获取预发布版本，例如 `v0.4.0-indev.1`。然后脚本会：

1. 找到带有本机对应构建的最新发布。
2. 把构建的 SHA-256 与 GitHub 给出的摘要比对。
3. 安装二进制文件、systemd unit 和 `/etc/shyake/shyake.env`。
4. 询问实例域名（仅首次安装）。
5. 启动服务。

发布版构建只提供 Linux 的 `x86_64` 和 `aarch64` 版本。要在运行前阅读脚本，先下载它：

```sh
curl -fsSLO https://raw.githubusercontent.com/salmonization/shyake/main/server/go/deploy/install.sh
less install.sh
sh install.sh
```

要修改其他设置，编辑 `/etc/shyake/shyake.env`，然后重启服务。[SPEC.md §11.2](SPEC.md) 说明了所有设置。另外请设置 `SHYAKE_GITHUB_TOKEN`（见 [GitHub token](#github-token建议配置)）。

#### 从源码构建

需要 Go 1.26 或更新版本。

1. **构建最新的正式版。** `main` 分支上可能有尚未发布的改动。要构建预发布版本，请切换到它的 tag。

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake
git checkout "$(git tag -l --sort=-v:refname 'v*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1)"
cd server/go
CGO_ENABLED=0 go build -trimpath -ldflags "-X main.version=$(cat ../VERSION)" \
    -o shyake-server ./cmd/shyake-server
```

`-ldflags` 设置 `shyake-server -version` 和 `GET /api/version` 显示的版本号。不加时版本号为 `dev`。得到的是静态二进制文件，可以在任何 CPU 架构相同的 Linux 机器上运行。

2. **用 systemd 安装。** 创建系统用户，然后安装 `server/go/deploy/` 中的文件：

```sh
sudo useradd --system --home-dir /var/lib/shyake --shell /usr/sbin/nologin shyake
sudo install -m 755 shyake-server /usr/local/bin/
sudo install -D -m 640 -g shyake deploy/shyake.env.example /etc/shyake/shyake.env
sudo install -m 644 deploy/shyake-server.service /etc/systemd/system/
```

3. **配置。** 编辑 `/etc/shyake/shyake.env`，至少设置实例域名：

```sh
SHYAKE_INSTANCE_DOMAIN=your.domain.example
SHYAKE_LISTEN=127.0.0.1:8787
```

实例上的每个地址都包含实例域名（`user@your.domain.example`）。其他实例用它把邮件发给你。该文件列出了其他所有设置及其默认值，[SPEC.md §11.2](SPEC.md) 有完整说明。另外请设置 `SHYAKE_GITHUB_TOKEN`（见 [GitHub token](#github-token建议配置)）。

4. **启动。**

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now shyake-server
curl http://127.0.0.1:8787/health
```

返回 `200 OK` 表示服务端和数据库都正常。服务以 `shyake` 用户运行，只能写入数据库所在的 `/var/lib/shyake`。

#### 配置带 TLS 的反向代理

联邦网络**必须**使用带 TLS 的反向代理。实例之间总是通过 `https://<domain>/...` 连接，因此你的实例必须位于 `https://your.domain.example`，并持有其他实例认可的证书。自签名证书无效。私有实例（其用户只在彼此之间通信）不需要代理，客户端可以通过明文 HTTP 连接。

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

发布版构建：再次运行安装脚本。脚本会保留 `/etc/shyake/shyake.env` 和 systemd unit，安装比当前更旧的版本前会先询问。

从源码构建：重新构建最新的正式版：

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

服务端启动时会自动执行新的数据库迁移。重启期间正在进行的发送会失败，客户端会把它保存为草稿。另请参阅上文的**升级到 v0.4.0**和**升级到 v0.3.0**。

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
