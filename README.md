## Shyake

English | [简体中文](./docs/i18n/README.zh-CN.md) | [日本語](./docs/i18n/README.ja.md)

### Overview

Shyake is an **end-to-end encrypted mail system**. It uses
**post-quantum cryptography**. The design is decentralized. This
resists censorship and surveillance.

The server runs on Cloudflare Workers, so everyone can host an
instance at no cost. You can also self-host the server on your own
hardware instead of the Cloudflare Global Network.

### Documents

To deploy your own instance:

* [**Deployment Guide**](./docs/DEPLOY.md)

For developers:

* [**Developer Guide**](./docs/DEV.md)
* [**Technical Specification**](./docs/SPEC.md)

### Installation

Download the binary from
[GitHub Releases](https://github.com/salmonization/shyake/releases),
extract it, and copy it to a directory in your `$PATH`:

```sh
sudo cp ./shyake /usr/local/bin/
```

Test the install:

```sh
shyake version
```

Once installed, you can upgrade in place with `shyake update`
(see the update command below).

### Usage

**First use**:

```sh
# initialize local config and generate key pairs
shyake init

# register on an instance, -u for username, -i for instance URL
shyake register -u salmon -i https://shyake.eee.coffee
```

`init` asks you to set a passphrase that protects your secret keys.
Leave it empty for no passphrase. Commands that use your keys then
prompt for this passphrase.

The config directory defaults to `~/.config/shyake/`.

You can create multiple profiles by specifying a directory at init:

```sh
shyake init -c path/to/your/dir
```

When you use this profile, you must always add the `-c` option.

Use the `whoami` command to check your profile.

```sh
shyake whoami
```

Run `shyake man` for a list of all commands. Run `shyake man
<command>` for detailed usage of one command.

**Check command**:

The `check` command lists your inbox and sent mail.

```sh
shyake check inbox
shyake check sent
```

You can use `--csv` and `--json` to format output for machine
parsing. You can also use `--no-header` to disable the column
header. Use `--count` to print the count only.

To check the header of a piece of mail:

```sh
shyake check fQBjZnvJ56
```

To list locally saved mail (see the save command below), or check the
header of a saved one:

```sh
shyake check saved
shyake check saved fQBjZnvJ56
```

To list local encrypted drafts (see the compose command below), or
check the header of a draft:

```sh
shyake check drafts
shyake check drafts 3
```

**Send command**:

```sh
shyake send -s "This is the subject" -t flat_white < body.txt
```

If `-s` is missing, the first line of the input file becomes the
subject.

```sh
shyake send -t flat_white < content.txt
```

The subject must not exceed 128 bytes.

If a send fails, for any reason, the client saves what you wrote as
a draft instead of losing it. The message shows the draft id. Retry
it with `shyake send -d <id>`.

Use `username@instance` as the recipient to reach a user on an
external instance.

```sh
shyake send -s "Hello" -t flat_white@shyake.example.com < body.txt
```

You can also use heredoc. Be careful of your shell history.

```sh
shyake send -s "This is the subject" -t flat_white <<EOF
Hello, this is the mail body.
EOF
```

Shyake transmits text only. You must base64-encode binary data
before you send it.

```sh
# send a poor image
base64 < image.png | shyake send -t flat_white -s "image.png"

# send a small tape archive
tar czf - ./source | base64 | shyake send -t flat_white -s "source.tar.gz"
```

**Compose command**:

`compose` writes mail drafts. It also works as a personal diary. It
opens your editor (default: `ed`) on a simple template. It stores
the result as a draft under `~/.config/shyake/drafts/`. The client
encrypts the draft to your own key with ML-KEM-768 +
ChaCha20-Poly1305.

```sh
shyake compose
```

```
To: flat_white
Subject: Coffee tomorrow?
---
The mail body goes here.
```

The client encrypts recipient, subject, and body at rest. Saving a
draft needs no passphrase, because it uses only your public key.
Listing or reading drafts needs your secret key unlocked.

You may leave the `To:` field empty. `check drafts` then shows such
a draft as `(null)`:

```sh
shyake compose
shyake check drafts       # list entries
shyake read drafts 3      # read entry 3
shyake compose 3          # continue writing entry 3
```

To send a draft, use `send --draft`. The recipient and subject come
from the draft. Use `-t` or `-s` to override them. The client
deletes the draft after it sends successfully:

```sh
shyake send --draft 3
shyake send --draft 3 -t flat_white
```

The editor defaults to `ed`. To use another editor, set the `EDITOR`
key in the config file, or set the `$VISUAL` or `$EDITOR`
environment variable. The client invokes `vim` and `nvim` with `-n
-i NONE`, so no plaintext leaks into swap files. Drafts are plain
local files. To delete one, remove
`~/.config/shyake/drafts/<id>.json`.

> The classic `ed` editor (and the early `ex`/`vi`) shipped with a
> `crypt(1)`-based encryption feature. This gave basic privacy for
> sensitive data on multi-user, time-sharing systems, where `root`
> could read any file. People commonly used it for diaries, private
> letters, password management, and unpublished code or design
> drafts. Nearly every modern Unix and Unix-like system has since
> dropped the feature, except NetBSD's `ed`, because its encryption
> scheme is long broken. Shyake `compose` is an homage to `ed -x`.

**Fetch command**:

This fetches a piece of mail and decrypts it.

```sh
shyake fetch fQBjZnvJ56
```

To export a piece of mail as plain text, with its header included,
run this command. Do not confuse it with the `save` command below,
which stores the encrypted mail:

```sh
shyake fetch fQBjZnvJ56 --no-color > exported-mail.txt
```

To output the body only, use `-r` or `--raw`.

```sh
shyake fetch fQBjZnvJ56 --raw
shyake fetch fQBjZnvJ56 -r > exported-mail.txt
```

To decode received base64-encoded binary data:

```sh
# decode a received image
shyake fetch fQBjZnvJ56 -r | base64 -d > image.png

# decode and extract a received archive
mkdir -p ./output
shyake fetch fQBjZnvJ56 -r | base64 -d | tar xzf - -C ./output
```

**Save and read commands**:

`save` fetches the encrypted mail from the server and stores it to
`~/.config/shyake/saved/<id>.json`. The client does NOT decrypt the
mail at this stage.

```sh
shyake save fQBjZnvJ56
```

`read` decrypts and displays a saved mail. Output matches `fetch`,
and `read` also supports `-r`/`--raw`.

```sh
shyake read fQBjZnvJ56
```

`read drafts <id>` does the same for a local draft.

```sh
shyake read drafts 3
```

In short, `read` handles everything local, while `fetch` handles the
remote side.

**Fingerprint command**:

To display your own fingerprint:

```sh
shyake fingerprint
```

To check fingerprints of your communicators:

```sh
shyake fingerprint flat_white
```

If a communicator rotates their key pair, you can update the
fingerprint you have for them. **Warning: before you run the update
command, always verify the new fingerprint. Use a second, trusted,
out-of-band channel, such as in person or a different platform. This
prevents identity impersonation.**

```sh
shyake fingerprint flat_white --update
```

**Burn command**:

This deletes a piece of mail.

```sh
shyake burn fQBjZnvJ56
```

**Block and unblock commands**:

Block or unblock a user or an instance. The target can be a username
or an instance URL.

```sh
shyake block flat_white
shyake block bad.example.com
shyake unblock flat_white
```

Use `blocklist` to review your current blocks:

```sh
shyake blocklist
```

**Update command**:

`shyake update` shows the installed and available versions. The
version lookup goes through your own instance, which relays the
GitHub Releases API. `shyake.eee.coffee` is only a built-in
fallback. The client uses it only when no instance is configured.
Use `stable` or `preview` to install the latest release from that
channel. The system offers the preview channel only when it is
newer than stable.

```sh
shyake update
shyake update stable
shyake update preview
```

**Rotate command**:

This rotates your key pairs and clears all mail to and from you.

```sh
shyake rotate
```

**Destroy command**:

This deletes your local configuration and key pairs. It also
destroys your account on the instance. It clears all mail to and
from you. Your username stays permanently locked. You cannot
register it again on this instance.

```sh
shyake destroy
```

### Advanced Usage

You can use `--no-color` to turn off colored output. Shyake also
respects the standard `NO_COLOR` environment variable.

```sh
shyake check inbox --no-color
shyake check fQBjZnvJ56 --no-color
shyake fetch fQBjZnvJ56 --no-color
```

Use `--plain` to disable the pager, color, and truncation for the
`check` and `fetch` commands.

You can edit the configuration file, at `~/.config/shyake/config` or
in the directories of your other profiles, to fit your setup. For
example, you can change the column layout for the `check` command.

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

Use `enc` and `dec` to encrypt or decrypt a standalone file with
ML-KEM-768 + ChaCha20-Poly1305. Use these commands for debugging and
testing only.

```sh
# encrypt with your own public key (output defaults to <file>.enc)
shyake enc secret.txt

# encrypt for a recipient, with a custom output path
shyake enc secret.txt -t flat_white -o secret.enc

# decrypt with your KEM secret key (stdout unless -o is given)
shyake dec secret.enc -o secret.txt
```

Use `--debug` to output verbose `curl` logs (handshakes, HTTP headers,
and internal variables) to `stderr`.

### License

BSD 2-Clause License
