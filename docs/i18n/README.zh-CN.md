## Shyake

[English](../../README.md) | 简体中文 | [日本語](./README.ja.md)

> Translated by Claude Fable 5

### 概述

Shyake 是一个由**后量子密码学**驱动的**端到端加密邮件系统**，旨在提供一种去中心化，抗审查，防监控的通信方式。

服务端运行在 Cloudflare Workers 上，因此任何人都可以零成本托管自己的实例。你也可以在自己的硬件上自托管服务端，而不使用 Cloudflare 全球网络。

### 文档

部署自己的实例：

* [**部署指南**](./zh-CN/DEPLOY.md)

面向开发者：

* [**开发者指南**](./zh-CN/DEV.md)
* [**技术规格**](./zh-CN/SPEC.md)

### 安装

从 [GitHub Releases](https://github.com/salmonization/shyake/releases)
下载二进制文件，解压后复制到 `$PATH` 中的目录：

```sh
sudo cp ./shyake /usr/local/bin/
```

测试：

```sh
shyake version
```

安装完成后，可以使用 `shyake update` 对客户端进行更新（参见下文的 update 命令）。

### 图形界面

一个实验性的本地 Web 图形界面位于 [`webui/`](../../webui)，基于 [Bun](https://bun.sh) 运行，通过 FFI 调用 `libshyake`：

```sh
cd webui && bun run start
```

详见 [webui/README.md](../../webui/README.md)。

### 使用方法

**首次使用**：

```sh
# 初始化本地配置并生成密钥对
shyake init

# 在实例上注册，-u 指定用户名，-i 指定实例 URL
shyake register -u salmon -i https://shyake.eee.coffee
```

`init` 会要求你设置一个用于保护私钥的 passphrase，留空则不设置 passphrase。执行需要使用到密钥的命令时会提示输入该 passphrase。

配置文件目录默认为 `~/.config/shyake/`。

你可以在初始化时指定目录来创建多个 profile：

```sh
shyake init -c path/to/your/dir
```

这种情况下，使用该 profile 时总是需要加上 `-c` 选项。

使用 `whoami` 命令查看当前配置文件。

```sh
shyake whoami
```

运行 `shyake man` 查看所有命令列表，运行 `shyake man <command>`
查看每个命令的详细用法。

**check 命令**：

`check` 命令可用于列出收件箱和已发送的邮件。

```sh
shyake check inbox
shyake check sent
```

可以使用 `--csv` 和 `--json` 将输出格式化，以便于机器解析。也可以使用 `--no-header` 关闭列标题，或使用 `--count` 以仅打印计数。

查看某封邮件的邮件头：

```sh
shyake check fQBjZnvJ56
```

列出本地保存的邮件（参见下文的 save 命令），或查看某封已保存邮件的邮件头：

```sh
shyake check saved
shyake check saved fQBjZnvJ56
```

列出本地加密草稿（参见下文的 compose 命令），或查看某份草稿的头部信息：

```sh
shyake check drafts
shyake check drafts 3
```

**send 命令**：

```sh
shyake send -s "This is the subject" -t flat_white < body.txt
```

如果缺少 `-s`，输入文件的第一行将作为主题。

```sh
shyake send -t flat_white < content.txt
```

主题长度不得超过 128 字节。

对于外部实例上的用户，发件时需使用 `username@instance` 作为收件人。

```sh
shyake send -s "Hello" -t flat_white@shyake.example.com < body.txt
```

你也可以使用 heredoc，但请务必小心 shell 历史记录泄漏。

```sh
shyake send -s "This is the subject" -t flat_white <<EOF
Hello, this is the mail body.
EOF
```

Shyake 只能传输文本。二进制数据在发送前必须先进行 base64 编码。

```sh
# 发送弱影像
base64 < image.png | shyake send -t flat_white -s "image.png"

# 发送小型磁带归档
tar czf - ./source | base64 | shyake send -t flat_white -s "source.tar.gz"
```

**compose 命令**：

`compose` 用于撰写邮件草稿，也可作个人日记等用途。该命令用你指定的编辑器（默认为 `ed`）打开一个简单的模板，并将结果作为草稿存入 `~/.config/shyake/drafts/`，使用 ML-KEM-768 + ChaCha20-Poly1305 加密给你自己的密钥。

```sh
shyake compose
```

```
To: flat_white
Subject: Coffee tomorrow?
---
The mail body goes here.
```

收件人、主题和正文在磁盘上全部加密。保存草稿不需要 passphrase（只用到你的公钥）。列出或阅读草稿则需要解锁你的私钥。

`To:` 栏可以留空，`check drafts` 会将其显示为 `(null)`：

```sh
shyake compose
shyake check drafts       # 列出所有条目
shyake read drafts 3      # 阅读第 3 条
shyake compose 3          # 继续编辑第 3 条
```

要发送草稿，使用 `send --draft`。收件人和主题取自草稿本身（可用 `-t`/`-s` 覆盖），发送成功后草稿会被自动删除：

```sh
shyake send --draft 3
shyake send --draft 3 -t flat_white
```

编辑器默认为 `ed`。可通过配置文件中的 `EDITOR` 键或 `$VISUAL`/`$EDITOR` 环境变量换用其他编辑器；使用 `vim` 或 `nvim` 时会以 `-n -i NONE` 启动，确保明文不会泄漏到交换文件中。草稿只是普通的本地文件，要删除某份草稿，直接移除 `~/.config/shyake/drafts/<id>.json` 即可。

> 经典的 `ed` 编辑器（以及早期 `ex`/`vi`）曾自带一个基于 `crypt(1)` 的加密功能，用于满足多用户分时系统下基本的隐私防窥和敏感数据保护（因为 `root` 可以查看任意文件）。常用于日记与私人信件，密码管理，以及未公开的代码和设计稿等。但如今几乎所有现代 Unix 和 Unix-like 操作系统的 ed 都已不再保留此功能（除了 NetBSD 的 `ed`），因为其加密方案早已被攻破。Shyake `compose` 是对 `ed -x` 的致敬。

**fetch 命令**：

获取一封邮件并解密。

```sh
shyake fetch fQBjZnvJ56
```

如果想将某封邮件导出为包含邮件头的纯文本（不要与下文的 `save`
命令混淆，后者存储的是加密邮件）：

```sh
shyake fetch fQBjZnvJ56 --no-color > exported-mail.txt
```

只输出正文时，使用 `-r` 或 `--raw`。

```sh
shyake fetch fQBjZnvJ56 --raw
shyake fetch fQBjZnvJ56 -r > exported-mail.txt
```

解码收到的 base64 编码二进制数据：

```sh
# 解码收到的影像
shyake fetch fQBjZnvJ56 -r | base64 -d > image.png

# 解码并解压收到的磁带归档
mkdir -p ./output
shyake fetch fQBjZnvJ56 -r | base64 -d | tar xzf - -C ./output
```

**save 和 read 命令**：

`save` 从服务器获取加密邮件并存储到
`~/.config/shyake/saved/<id>.json`。此阶段邮件不会被解密。

```sh
shyake save fQBjZnvJ56
```

`read` 解密并显示一封已保存的邮件。输出与 `fetch` 相同，同样支持 `-r`/`--raw`。

```sh
shyake read fQBjZnvJ56
```

`read drafts <id>` 对本地草稿做同样的事。

```sh
shyake read drafts 3
```

简言之，`read` 负责本地，`fetch` 负责远端。

**fingerprint 命令**：

显示你自己的指纹：

```sh
shyake fingerprint
```

查看通信对象的指纹：

```sh
shyake fingerprint flat_white
```

如果通信对象轮换了密钥对，你可以更新他们的指纹。**但在运行更新命令之前，请务必通过另外的可信带外渠道（例如当面确认或通过其他通信方式）核实新指纹，以防身份冒用。**

```sh
shyake fingerprint flat_white --update
```

**burn 命令**：

删除一封邮件。

```sh
shyake burn fQBjZnvJ56
```

**block 和 unblock 命令**：

屏蔽或取消屏蔽某个用户或实例。目标可以是用户名或实例 URL。

```sh
shyake block flat_white
shyake block bad.example.com
shyake unblock flat_white
```

使用 `blocklist` 查看当前的屏蔽列表：

```sh
shyake blocklist
```

**update 命令**：

`shyake update` 显示已安装版本和可用版本。版本查询通过你自己的实例进行（由实例中继 GitHub Releases API）；`shyake.eee.coffee` 只是内置的回退目标，仅在未配置实例时使用。使用 `stable` 或
`preview` 安装对应渠道的最新版本。

```sh
shyake update
shyake update stable
shyake update preview
```

**rotate 命令**：

轮换你的密钥对，并清除所有与你相关的收发邮件。

```sh
shyake rotate
```

**destroy 命令**：

删除你的本地配置和密钥对，并销毁你在实例上的账户。所有与你相关的收发邮件都会被清除。你的用户名将被永久锁定，无法在该实例上再次注册。

```sh
shyake destroy
```

### 高级用法

可以使用 `--no-color` 关闭彩色输出。标准的 `NO_COLOR` 环境变量亦受支持。

```sh
shyake check inbox --no-color
shyake check fQBjZnvJ56 --no-color
shyake fetch fQBjZnvJ56 --no-color
```

对 `check` 和 `fetch` 命令使用 `--plain` 可以禁用分页器、颜色和截断。

你可以编辑配置文件（位于 `~/.config/shyake/config` 或其他配置文件所在目录）来优化设置，例如修改 `check` 命令的列布局。

```sh
# Date & Time format (strftime format)
# RECENT: less than 180 days old.
# ISO 8601 format
TIME_FORMAT="%Y-%m-%d %H:%M"
# POSIX format
# TIME_FORMAT="%b %d  %Y"
# TIME_FORMAT_RECENT="%b %d %H:%M"

# Time zone (default: auto)
# Integer offset in hours: 0=UTC, 8=UTC+8, -6=UTC-6
TIME_ZONE=auto

# Display columns for `check` command
CHECK_COLUMNS=id,sender,subject,size,date

# Disable colors (1 = disable)
# NO_COLOR=0

# Editor for 'shyake compose' (default: ed)
# Falls back to $VISUAL, then $EDITOR
# EDITOR="ed"

# Default action when running without arguments
# 0 = man, 1 = check inbox, 2 = check inbox --count
DEFAULT_ACTION=0
```

使用 `enc` 和 `dec` 可以通过 ML-KEM-768 + ChaCha20-Poly1305
加密或解密独立文件。这些命令用于调试/测试目的。

```sh
# 使用你自己的公钥加密（输出默认为 <file>.enc）
shyake enc secret.txt

# 为某个收件人加密，并指定输出路径
shyake enc secret.txt -t flat_white -o secret.enc

# 使用你的 KEM 私钥解密（未指定 -o 时输出到 stdout）
shyake dec secret.enc -o secret.txt
```

使用 `--debug` 将详细的 `curl` 日志（握手、HTTP 头和内部变量）输出到 `stderr`。

### 许可证

BSD 2-Clause License
