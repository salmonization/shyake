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

前提条件：

- Node.js 18+
- 一个 Cloudflare 账户

步骤：

1. 在 GitHub 上 **fork 并克隆**本仓库。

2. 在终端中**认证** Cloudflare **Wrangler CLI**：

```sh
npx wrangler login
```

如果 Wrangler 尚未安装，`npx` 会在首次运行时提示安装。无需单独的安装步骤。

3. **创建 D1 数据库**：

```sh
npx wrangler d1 create shyake-db
```

从输出中复制 `database_id`。

4. **创建 KV 命名空间**（版本中继缓存）：

```sh
npx wrangler kv namespace create VERSION_CACHE
```

从输出中复制 `id`。每个实例都会为自己的客户端中继 GitHub
Releases API 以支持 `shyake update`；此 KV 命名空间将查询结果缓存一小时。该绑定是可选的。没有它端点仍然可用，只是每次请求都会访问 GitHub。

5. **编辑你 fork 中的 `server/wrangler.toml`**：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example" # 修改此处
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB；不要超过 786432（768 KiB）

[[d1_databases]]
binding        = "DB"
database_name  = "shyake-db"
database_id    = "<your database_id>" # 在此粘贴你的 database_id
migrations_dir = "migrations"

[[kv_namespaces]]
binding = "VERSION_CACHE"
id      = "<your kv namespace id>" # 在此粘贴你的 KV 命名空间 id
```

`[[d1_databases]]` 块必须存在且包含正确的 `database_id`。缺少它的话 Worker 没有数据库绑定，所有请求都会失败。

如果没有自定义域名，可以使用默认的 `*.workers.dev` URL 作为
`INSTANCE_DOMAIN`。

6. **应用数据库迁移**（创建所有表）：

```sh
cd server
npx wrangler d1 migrations apply shyake-db --remote
```

Cloudflare 的 CI 流水线不会自动应用数据库迁移。你必须手动运行一次 `wrangler d1 migrations apply`。跳过这一步会导致数据库为空，
Worker 的每次 API 调用都会报错。

7. **部署**

选择以下方式之一：

**方式 A：控制台（Dashboard）**：在 Cloudflare 控制台中进入
`Compute → Workers & Pages → Create application → Continue with GitHub`
（首次使用可能需要先 `Add GitHub account`），选择你的 fork，并设置：

| 字段 | 值 |
|-------|-------|
| Framework preset | None |
| Build command | None |
| Deploy command | `npx wrangler deploy` |
| Root directory | `/server` |

之后推送到你的 fork 时会自动重新部署。

**方式 B：仅使用 CLI**：

```sh
cd server
npm install
npx wrangler deploy
```

8. **验证**

等待部署完成，然后打开
`https://<worker>.workers.dev/health`（或你的自定义域名）。返回 `200 OK` 即表示 Worker 和数据库工作正常。

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

1. **克隆**本仓库（不需要 fork）：

```sh
git clone https://github.com/salmonization/shyake.git
cd shyake/server
npm install
```

2. **编辑 `server/wrangler.toml`**：只有 `[vars]` 部分是重要的。本地模式下会忽略
`database_id` 和 KV 的 `id`，占位符保持原样即可：

```toml
[vars]
INSTANCE_DOMAIN      = "your.domain.example" # 修改此处
REGISTRATION_ENABLED = true
RESERVED_USERNAMES   = "admin,system,support,noreply,shyake,root,postmaster"
FEDERATION_ENABLED   = true
MAX_MAIL_SIZE        = 196608 # 192 KiB；不要超过 786432（768 KiB）
```

`INSTANCE_DOMAIN` 必须是你的实例在外部可访问到的域名。它会嵌入到你实例上的每个地址中（`user@your.domain.example`），其他实例也依靠它把联邦邮件路由回你这里。

3. 在本地**应用数据库迁移**（创建所有表）：

```sh
npx wrangler d1 migrations apply shyake-db --local
```

注意 `--local` 标志。它会写入磁盘上的 SQLite
文件，而不是 Cloudflare 托管的数据库。

4. **运行服务端**：

```sh
npx wrangler dev --local --ip 127.0.0.1 --port 8787
```

用 `curl http://127.0.0.1:8787/health` 验证。返回 `200 OK`
即表示 Worker 和数据库工作正常。

让服务端只绑定 `127.0.0.1`，由反向代理处理外部流量（见下一步）。直接绑定
`0.0.0.0` 只在不参与联邦网络的受信任局域网中才算合理。

5. **配置带 TLS 的反向代理**

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

6. **保持运行**

`wrangler dev` 是前台进程；用进程守护工具让它开机自启并在崩溃后自动重启。一个最小的
systemd 单元（`/etc/systemd/system/shyake.service`）：

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

**数据位置与备份**

所有本地状态（D1 的 SQLite 数据库和 KV 缓存）都存放在
`server/.wrangler/state/` 目录下。备份实例就是备份这个目录（先停止服务端，或使用对
SQLite 安全的工具，避免在写入过程中复制数据库）。删除该目录会把实例重置为空数据库。可以给
`wrangler dev` 传 `--persist-to <dir>` 把状态存到别的位置。

**注意事项：了解你在运行什么**

`wrangler dev` 是 Wrangler
的开发服务器，不是加固过的生产服务器。它运行的正是驱动 Cloudflare
Workers 的同一个 `workerd` 运行时，对个人或小型社区实例来说完全够用，但要了解它面向开发的行为特性：

- **文件监听 / 热重载**: 它会监听源码目录，文件变更时重新加载
  Worker。开发时很方便，但在服务器上意味着在 `server/` 里编辑文件或执行
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
