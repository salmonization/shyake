## Shyake 部署指南

[English](../../DEPLOY.md) | 简体中文 | [日本語](../ja/DEPLOY.md)

> Translated by Claude Fable 5

服务端以带有 D1 数据库的 Cloudflare Worker 形式运行。不过，你也可以在自己的硬件上自托管。

部署服务端有两种方式：

* 使用 Cloudflare
* 自托管

**联邦网络**

当两个实例都设置了 `FEDERATION_ENABLED = true` 时，它们会自动进行联邦网络通信，无需额外配置。跨实例邮件以
server-to-server 的方式路由；客户端始终只与自己的实例通信。

要禁用入站和出站的联邦网络通信：

```toml
FEDERATION_ENABLED = false
```

### 使用 Cloudflare

全部操作都在你自己的机器上通过 Wrangler CLI 完成。不需要 fork 本仓库，
不需要把仓库连接到 Cloudflare，也不需要在控制台里点来点去。

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

实例域名会嵌入到你实例上的每一个地址中（`user@your.domain.example`），
其他实例也依靠它把联邦邮件路由回你这里。如果你没有自定义域名，默认的
`*.workers.dev` 地址同样可用。

选项：

| 选项 | 作用 |
|---|---|
| `--domain <d>` | 非交互式地指定实例域名 |
| `--update` | 先拉取最新代码，再重新部署 |
| `--no-kv` | 跳过 KV 版本缓存 |
| `--config-only` | 只生成 `wrangler.toml` 然后退出 |
| `--local` | 改为配置本地自托管（见下文） |

#### 升级

```sh
cd shyake/server/cf
./deploy.sh --update
```

它会拉取最新代码、应用新增的迁移并重新部署。已有资源会被复用，你的配置
也会保留。

#### 修改配置

`server/cf/wrangler.toml` 由 `wrangler.template.toml` 在首次运行时生成，
并且**不受 git 跟踪**，因此你的实例配置能在 `git pull` 后保留，也永远
不会产生冲突。编辑它，然后重新运行 `./deploy.sh`：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example"
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB；不要超过 786432（768 KiB）
```

重新运行脚本不会覆盖这些设置，它只会补上仍未填写的资源 id。

如果你的实例是在 `wrangler.toml` 改为生成式之前部署的，
`./deploy.sh --update` 会先把你的配置另存为 `wrangler.toml.bak`，
拉取代码后再恢复回来。

### 自托管

自托管就是在你自己的机器上、通过 Wrangler 自带的本地 `workerd`
运行时来运行完全相同的 Worker 代码。D1（SQLite）和 KV 都由 Wrangler
在本地模拟，因此**不需要 Cloudflare 账户**。不需要 `wrangler login`，也不需要在控制台创建任何资源。

前提条件：

- Node.js 18+
- 一台保持在线的机器（任何 Node.js 支持的操作系统均可；下面的示例假设是带
  systemd 的 Linux）
- 若要参与联邦网络：一个指向该机器的公网域名，以及一个带有效
  TLS 证书的反向代理（见下文）

步骤：

1. **完成配置**（不需要 fork）：

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server/cf
./deploy.sh --local --domain your.domain.example
```

`--local` 会跳过一切需要 Cloudflare 账户的步骤：不需要 `wrangler login`，
也不会创建任何远端资源。它会安装依赖、生成 `wrangler.toml`
并创建本地 SQLite 数据库。

`--domain` 必须是你的实例在外部可访问到的域名。它会嵌入到你实例上的每个
地址中（`user@your.domain.example`），其他实例也依靠它把联邦邮件路由回
你这里。不加这个参数时脚本会询问。

2. 如有需要，在生成的 `server/cf/wrangler.toml` 里**调整配置**。只有
`[vars]` 部分是重要的；本地模式下会忽略 `database_id` 和 KV 的 `id`：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example"
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB；不要超过 786432（768 KiB）
```

该文件不受 git 跟踪，因此你的修改能在 `git pull` 后保留。之后用
`./deploy.sh --update --local` 升级。

3. **运行服务端**：

```sh
npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

用 `curl http://127.0.0.1:8787/health` 验证。返回 `200 OK`
即表示 Worker 和数据库工作正常。

让服务端只绑定 `127.0.0.1`，由反向代理处理外部流量（见下一步）。直接绑定
`0.0.0.0` 只在不参与联邦网络的受信任局域网中才算合理。

4. **配置带 TLS 的反向代理**

这一步是**参与联邦网络的必要条件**。实例之间总是通过
`https://<domain>/...` 互相通信，所以你的实例必须能在
`https://your.domain.example` 被访问到，并且证书要能被其他实例接受。自签名证书不行。如果你的实例是私有的（用户之间只互发邮件），可以跳过这一步，让客户端用明文
HTTP 连接。

使用 [Caddy](https://caddyserver.com/) 时，证书会自动获取和续期；整个
`Caddyfile` 只需：

```
your.domain.example {
    reverse_proxy 127.0.0.1:8787
}
```

用 nginx 加 certbot 管理的证书同样可行。把
`https://your.domain.example` 代理到 `http://127.0.0.1:8787`。

5. **保持运行**

`wrangler dev` 是前台进程；用进程守护工具让它开机自启并在崩溃后自动重启。一个最小的
systemd 单元（`/etc/systemd/system/shyake.service`）：

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

**数据位置与备份**

所有本地状态（D1 的 SQLite 数据库和 KV 缓存）都存放在
`server/cf/.wrangler/state/` 目录下。备份实例就是备份这个目录（先停止服务端，或使用对
SQLite 安全的工具，避免在写入过程中复制数据库）。删除该目录会把实例重置为空数据库。可以给
`wrangler dev` 传 `--persist-to <dir>` 把状态存到别的位置。

**注意事项：了解你在运行什么**

`wrangler dev` 是 Wrangler
的开发服务器，不是加固过的生产服务器。它运行的正是驱动 Cloudflare
Workers 的同一个 `workerd` 运行时，对个人或小型社区实例来说完全够用，但要了解它面向开发的行为特性：

- **文件监听 / 热重载**: 它会监听源码目录，文件变更时重新加载
  Worker。开发时很方便，但在服务器上意味着在 `server/cf/` 里编辑文件或执行
  `git pull` 会立即重启你的实例。请谨慎更新：先 pull、检查改动，再让它重载（或自己重启服务）。
- **单进程，自身没有守护能力**: 没有集群，也没有内置的崩溃恢复。这正是上面
  systemd 单元的作用。
- **没有限流或 DDoS 防护**: 在 Cloudflare
  上这些由平台提供。自托管时，如果实例对公网开放，应在反向代理层添加限流。
- **交互式快捷键**: 连接到终端时 `wrangler dev` 会从 stdin
  读取热键。在 systemd 下没有 TTY，因此不受影响；但如果改在 `tmux`
  里运行，注意不要误按按键（`x` 会清空控制台，`Ctrl+C` 会退出）。

如果实例规模超出了这套方案的承载能力，上文的 Cloudflare
部署路径才是可扩展的选择。数据库可以通过导出本地 SQLite 文件并用
`wrangler d1 execute --remote` 导入来迁移。
