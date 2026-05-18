
## Shyake - E2EE digital mailer

Copyright (c) 2026 Salmonization

<table>
<tr><td>创建日期</td><td>2026-04-25 23:24 UTC+8</td></tr>
<tr><td>最后更新</td><td>2026-05-18 11:37 UTC+8</td></tr>
</table>

### Overview

`shyake` 是一款 POSIX 风格, 纯 CLI 驱动的端到端加密异步文本通信工具.
旨在提供抗审查, 防嗅探, 低依赖的文本通信方式.


### 一. 技术架构

客户端 (C):
   - 标准: C99/C11, POSIX.1-2008
   - CLI 解析: POSIX `getopt`, `stdin`
   - 网络: `libcurl`
   - JSON: `cJSON`
   - 密码学: `liboqs`, ML-KEM & ML-DSA

服务端 (TypeScript + CF Worker + D1):
   - 框架: Cloudflare Workers (Hono)
   - 数据库: Cloudflare D1
   - 验签: `liboqs` 中的 ML-DSA 相关部分, 编译为 `.wasm` 模块


### 二. 目录结构

```
shyake/
├── client/             # C client
│   ├── src/
│   ├── include/
│   └── Makefile
├── server/             # TypeScript server
│   ├── src/
│   ├── migrations/
│   └── wrangler.toml
├── docs/
│   ├── PLAN.md
│   └── DEPLOY.md       # deployment document
├── LICENSE
└── README.md
```


### 三. 数据库结构

1.  `users`:
    * `username` (TEXT, PRIMARY KEY, 匹配正则
    `^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$`)
    * `kem_pubkey` (TEXT/BLOB, 后量子加密公钥)
    * `sig_pubkey` (TEXT/BLOB, 后量子签名公钥)
    * `created_at` (INTEGER)

2.  `mail`:
    * `mail_id` (TEXT, PRIMARY KEY, 10位 base58, 由服务端分配并保证 UNIQUE)
    * `sender` (TEXT, 无硬外键约束. 本实例用户不含域名, 外部用户含域名)
    * `recipient` (TEXT, 无硬外键约束. 本实例用户不含域名, 外部用户含域名)
    * `enc_key_sender` (TEXT/BLOB, 使用发件人 KEM 公钥封装的对称密钥)
    * `enc_key_recipient` (TEXT/BLOB, 使用收件人 KEM 公钥封装的对称密钥)
    * `enc_subject` (TEXT, 对称加密后的主题)
    * `enc_body` (TEXT, 对称加密后的正文内容)
    * `size` (INTEGER, 正文的明文字节数, 用于 UI 显示)
    * `signature` (TEXT, 发件人对该记录的数字签名)
    * `timestamp` (INTEGER)


### 四. 交互与 UI 设计

全局选项:
   - `--plain`: 禁用终端自适应截断, 使用 \t 分隔, 无格式化,
   为 awk/jq 脚本设计.
   - `--debug`: 输出详细的 Curl 握手信息与内部变量到 stderr.
   - `--no-color` / `NO_COLOR=1`: 禁用颜色.
   当前计划先不做终端颜色输出, 后续可能会做 自定义 ANSI 16 色 / 256 色主题.

1. `shyake init`

逻辑: 生成目录树、默认 `config` 文件、`liboqs` 密钥对.

```bash
# ~/.config/shyake/config
# shyake global configuration file

INSTANCE=https://shyake.eee.coffee

# Date & Time format (strftime format)
# RECENT: less than 180 days old.
# ISO 8601 format
TIME_FORMAT="%Y-%m-%d %H:%M"
# POSIX format
# TIME_FORMAT="%b %d  %Y"
# TIME_FORMAT_RECENT="%b %d %H:%M"

# Time zone (default: UTC)
TIME_ZONE=0

# Display columns for `check` command
CHECK_COLUMNS=id,sender,subject,size,date

# Disable colors (1 = disable)
# NO_COLOR=0
```

2. `shyake register -u <username> -i "shyake.eee.coffee"`

逻辑: 将用户名和两把公钥打包. 使用签名私钥对 Payload 签名, 附带 PoW,
发送 `POST /api/register` 到 Worker. Worker 验证签名, 查重并落库.

任何人可自行部署 Worker 实例, 注册时需指定实例.
仅 localhost 允许 `http://`, 否则必须 `https://`.

实例拥有者可以在 `wrangler.toml` 中配置 `REGISTRATION_ENABLED = true`.
如果设置为 `false`, 则 Worker 会拒绝所有新的注册请求.

为防止冒充, 实例拥有者可以在 `wrangler.toml` 中配置 `RESERVED_USERNAMES`.
例如: `admin,system,support,noreply,shyake`), Worker 会拒绝这些注册请求.

3. `shyake send -t <recipient> [-s <subject>] [file]`

逻辑:
   * 从本地 `known_hosts` 获取对方公钥, 若无则静默发起
   `GET /api/pubkey/<recipient>` 并保存. 同时计算指纹
   `recipient_kem_fingerprint`.
   * 读取 stdin 或文件内容.
   * 计算信件明文的字节数 (将存入 `size` 字段以供 UI 展示).
   * 生成一个随机的对称密钥 (ChaCha20-Poly1305).
   * 使用该对称密钥加密 subject 和 body.
   * 使用接收方的 KEM 公钥封装该对称密钥, 存入 `enc_key_recipient`.
   * 使用发送方自己的 KEM 公钥封装该对称密钥, 存入 `enc_key_sender`.
   * 使用自己的 DSA 私钥对数据进行签名. 为防重放攻击, 签名必须覆盖:
   `sender`, `recipient`, `recipient_kem_fingerprint`, `enc_subject`,
   `enc_body`, `timestamp` 和 `size`. 注: mail_id 由服务端分配不签名.
   * 将完整 Payload 附带 PoW 经 `libcurl` 发送 `POST /api/mail`
   到自己所属的 Worker 实例. Worker 校验通过后分配唯一 `mail_id` 落库.

如果接收方 Worker 发现收到的 `recipient_kem_fingerprint`
与数据库中其当前公钥指纹不匹配, 则直接拒收并返回错误 `KEY_MISMATCH`.
客户端收到此错误后触发:

```
FATAL: Remote public key of recipient has changed!
RUN 'shyake fingerprint <username>' to inspect and update trust.
```

参数 `-s, --subject` 用于指定 subject, 如无此参数则将文件或 stdin 第一行作为
subject. Subject 不可以超过 128 Bytes, 应在发信前检查.

Worker 收到信件后, 虽然会将 `size` (明文字节数) 落库供客户端 UI 读取,
但为了防止滥用, Worker 会直接对接收到的 HTTP Body Payload 总体积
进行硬性限制. 默认设定为密文体积不可超过 196608 Bytes (192 KiB),
可在 `wrangler.toml` 更改. 考虑到 Cloudflare D1 单行数据限制,
最高不可超过 786432 (768 KiB).

如果是实例间通信, 则需满足两个实例最小的那个上限设置.

自己可以给自己发送信件.

收件人, 不填写 `@` 及其后的实例域名则认为是发送给本实例对应用户,
如需发送给外部实例用户, 则必须填写, 如 `bean@shyake.caffeine.ink`.

输出:

```
Your mail was sent.
```

4. `shyake check [inbox/sent]`

逻辑: 使用 DSA 签名发起鉴权请求 `GET /api/mail`.
Worker 校验后返回所有属于该用户的信件元数据.
客户端在本地使用 KEM 私钥解出封装的对称密钥
(收件箱使用 `enc_key_recipient`, 发件箱使用 `enc_key_sender`),
进而解密 `enc_subject`, 在 stdout 输出.

扩展 option:
   - `-c, --count`: 只打印计数
   - `--json`: 以 json 格式输出
   - `--csv`: 以 csv 格式输出
   - `--no-header`: 不显示 header row

如果内容过多则自动调用 `less`. 显示总宽度不超过屏幕宽度, 如果超过则对
`Subject` 输出进行截断, 且末三位显示为 `...`.

如果需要查看完整 subject, 则需使用 `check [inbox/sent] <id>`.
可以查看完整的 subject 和完整的字节数, subject 显示如果超出宽度则折行,
但缩进以保证行首和前一行内容对齐.

使用:

```bash
shyake check inbox
```

输出:

```
Mail ID    Sender     Subject                 Size Date
fQBjZnvJ56 flat_white This is subject, hello! 51   2026-04-11 14:30

Total: 1 item
```

其中 header row 的 `Mail ID` `Sender` 等均需使用转义字符加粗且下划线.
`Size` 项如果超过 1023 则开始使用 K 显示, 如 `10K`.
`Sender` 项如果是外部实例用户, 应在末尾显示一个 `@` 符号但不显示其实例域名.
如 `bean@`.

使用:

```bash
shyake check sent
shyake check -c inbox
shyake check inbox fQBjZnvJ56
```

输出:

```
No mail found.
```

```
1
```

```
Sender:     flat_white
Recipient:  salmon
Subject:    This is subject, hello!
Size:       51
Date:       2026-04-11 14:30
```

5. `shyake fetch [--raw] <id>`

默认输出信件元数据和正文.

```
FROM: flat_white@shyake.caffeine.ink  (stdout)
DATE: 2026-04-11 14:30                (stdout)
SUBJ: This is subject, hello!         (stdout)
                                      (stdout 空行)
This is body.                         (stdout)
Hello, world!                         (stdout)
                                      (stderr 空行)
```

如果使用 `--raw` 参数, 则仅将解密后的 `enc_body` 数据流输出到 `stdout`,
不打印任何元数据和空行. 此模式为管道传输设计 (例如直接解压 tarball).

6. `shyake burn <id>`

逻辑: 发件人或收件人均可发起. 通过 DSA 签名证明身份,
Worker 校验请求者是否为 `sender` 或 `recipient`,
如果是则在 D1 中 `DELETE`.

7. `shyake block <username>`

逻辑: 封锁某账户或实例. 可以用 `unblock` 解除封锁.

客户端将指令和签名发送给 Worker, Worker 将记录存入 `blocks` 表.
在接收外部信件时, Worker 会首先查询 `blocks` 表, 若命中则直接返回
HTTP 403 拒收, 从而在服务端层面切断恶意数据入库, 保护存储空间.

使用:

```bash
shyake block bot1010@shyake.example.com
shyake unblock bot1010@shyake.example.com
shyake block shyake.example.com
```

8. `shyake fingerprint`

诊断和验真工具, 必须每次都重新计算, 并且展示本地和远程的状态对比.

查询自己: 计算自己本地的公钥指纹.

```bash
shyake fingerprint
```

查询他人: 网络比对, 强制向实例拉取最新的公钥,
重新计算指纹, 然后与本地 known_hosts 进行比对, 输出诊断报告.

外部用户, 依然采用实例间通信拉取最新的公钥.

```bash
shyake fingerprint flat_white@shyake.eee.coffee
```

指纹输出可以采用 GPG 风格的十六进制字符组:

```
3F8A 09B1 C2D3 E4F5  6A7B 8C9D 0E1F 2A3B
4C5D 6E7F 8A9B 0C1D  2E3F 4A5B 6C7D 8E9F
```

也可以附带 OpenSSH 风格的 Randomart ASCII 字符画:

```
+------------------+
|      . o .       |
|     . o = .      |
|      . + +       |
|       = . +      |
|      o S o .     |
|       . o .      |
|        . .       |
|         E        |
|                  |
+------------------+
```

如果收件人轮换了密钥, 发件人可以用 `--update` 更新本地公钥指纹:

```bash
shyake fingerprint flat_white@shyake.eee.coffee --update
```

9. `shyake rotate`

逻辑: 轮换自己的公私钥对. 生成新的 `liboqs` 密钥对, 并将新的公钥使用旧的
DSA 私钥进行签名, 证明所有权, 然后提交给 Worker 进行平滑更新.

10. `shyake destroy`

```
WARNING: This will permanently delete your KEYS and ALL MESSAGES sent to or
from you. And your username will be locked forever and cannot be registered
again. Type your username to confirm:
```

确认后发送带有 DESTROY 指令和签名的请求给服务端. Worker 会清空该用户的公钥,
并删除 `mail` 表中 `sender` 或 `recipient` 为该用户的所有信件记录.
用户名将被永久锁定, 阻止任何人再次注册该用户名, 防止被身份冒充.

随后在本地执行:

```bash
rm -rf ~/.config/shyake/*
```


### 五. 联邦网络

寻址方式: `username@instance.domain` 不带 `@` 则为本用户所在实例.

路由机制:

采用 Server-to-Server 路由模式. 客户端只与自己所在的实例通信.
跨实例发信时, 客户端将加密且签名好的 Payload POST 给自己的实例 Worker.
发件人的 Worker 收到后存入本地数据库, 随后在后台使用 `fetch` 转发 Payload
给目标实例的 Worker. 确保了双边数据的原子性和一致性.

Server-to-Server 验证:

目标实例接收到外域实例转发的 POST 请求后, 向发件人所在实例发起 HTTP GET
获取其公钥进行验签. 信件在投递成功后,
在发件人实例和收件人实例的数据库中会各保存一份.

开启或关闭联邦网络:

任何自部署实例可自行选择是否参与联邦网络.
可以在 `wrangler.toml` 配置 `FEDERATION_ENABLED = true`.
如果设置为 `false`, 则不接受外部实例用户向本实例用户的发信请求,
也不接受本实例用户向外部实例的发信请求.


### 六. TOFU 与 OOB 验证

TOFU 不需要在第一次发信时阻断式询问, 默认信任第一次,
因为即使弹出提示用户也常常总是直接 yes 而不会真的检查,
所以在第一次阻断询问是缺乏意义的, 还可能会让人困惑.

向一个用户发送信件时, 客户端首先查找本地 `known_hosts` 文件:

首次通信: 如果在 `known_hosts` 中未找到对方指纹, 则向目标实例 `GET /pubkey`,
获取后计算指纹并静默追加到 `known_hosts`. 然后发信.

非首次通信: 默认直接信任并使用本地 `known_hosts` 中缓存的公钥进行加密,
不再主动发起额外的网络请求, 除非收到:
1. `KEY_MISMATCH`: 收件人公钥已轮换 (HTTP 409)
2. `USER_DESTROYED`: 收件人已销毁账号 (HTTP 410)

OOB 验证通过 `fingerprint` 命令进行.


### 七. 反滥用机制

Hashcash PoW (20-bit): 所有写操作均需提供 PoW.


### 八. 反重放攻击

任何改变服务端状态的操作, 如 `send`, `block`, `unblock`, `burn`, `rotate`,
`destroy` 都必须在 Payload 中包含客户端的当前 `timestamp`, 并被用户的 DSA
私钥进行数字签名覆盖. Worker 接收到请求时, 除了验证签名合法性, 还会验证
`timestamp` 是否处于合理的时间窗口内, 例如与服务器当前时间误差不超过 5 分钟.
这避免了攻击者截获旧的合法包并在未来进行恶意重放.
