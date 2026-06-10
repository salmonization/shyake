## Shyake

### Overview

Shyake is an **end-to-end encrypted mail system** powered by
**post-quantum cryptography**, designed as a decentralized
communication method to resist censorship and surveillance.

It means you can also self-host the server on your own hardware,
instead of Cloudflare Global Network.

* **Why we choose Cloudflare Workers?**
  * Everyone can host their own instance without any cost!

### Documents

To deploy your own instance:

* [**Deployment Guide**](./docs/DEPLOY.md)

For developers:

* [**Developer Guide**](./docs/DEV.md)
* [**Technical Specification**](./docs/SPEC.md)

### Installation

Download from GitHub Release.

Extract the binary file and copy it to `$PATH`.

```sh
./shyake version
cp ./shyake /usr/local/bin/
```

Test to see if everything goes well:

```sh
shyake version
```

### Usage

**First use**:

```sh
# initialize local config and generate key pairs
shyake init

# register on an instance, -u for username
shyake register -u salmon -i https://shyake.eee.coffee
```

Configuration is stored at `~/.config/shyake/`.

You can create multiple profiles by specify a directory when init:

```sh
shyake init -c path/to/your/dir
```

And in that case you need always add `-c` option when you want to use
this profile.

Use `whoami` command to check your profile.

```sh
shyake whoami
```

**Check command**:

```sh
shyake check inbox
shyake check sent
```

You can use `--csv` and `--json` to format output for machine parsing. You
can also use `--no-header` to disable the header if you want.

To check metadata of a piece of mail:

```sh
shyake check fQBjZnvJ56
```

**Send command**:

```sh
shyake -s "This is the subject" -t flat_white < body.txt
```

First line of the input file will be the subject if `-s` is missing.

```sh
shyake -t flat_white < content.txt
```

Please note that the subject must not exceed 128 bytes in length.

You can also use heredoc, but please be careful of your shell history.

```sh
shyake -s "This is the subject" -t flat_white <<EOF
```

**Fetch command**:

This will fetch a piece of mail and decrypt it.

```sh
shyake fetch fQBjZnvJ56
```

If you want to save a piece of mail with the metadata:

```sh
shyake fetch fQBjZnvJ56 --no-color > saved-mail.txt
```

To output the body only, use `-r` or `--raw`.

```sh
shyake fetch fQBjZnvJ56 --raw
shyake fetch fQBjZnvJ56 -r > saved-mail.txt
```

**Fingerprint command**:

To display your own fingerprint:

```sh
shyake fingerprint
```

To check fingerprints of your communicators:

```sh
shyake fingerprint flat_white
```

You can update the fingerprints of your communicators if they have rotated
their key pairs. **Warning: Before running the update command, always verify
the new fingerprint through a secondary, trusted out-of-band channel (e.g.,
in person or via a different platform) to prevent identity impersonation.**

```sh
shyake fingerprint flat_white --update
```

**Burn command**:

This will delete a piece of mail.

```sh
shyake burn fQBjZnvJ56
```

**Rotate command**:

This will rotate your key pairs and clear all mail to and from you.

```sh
shyake rotate
```

**Destroy command**:

This will delete your local configuration and key pairs, also destruct
your account on the instance. All mail to and from you will be cleared.
Your username will be permanently locked and cannot be registered again
on this instance.

```sh
shyake destroy
```

### Advanced Usage

You can use `--no-color` to turn off the colored output.

```sh
shyake check inbox --no-color
shyake check fQBjZnvJ56 --no-color
shyake fetch fQBjZnvJ56 --no-color
```

Use `--plain` to disable the pager, color, and truncation for `check` and
`fetch` command.

You can edit the configuration file (located at `~/.config/shyake/config` or
the directories of your other profiles) to optimize your setup. For instance,
by changing the column layout for `check` command.

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

# Default action when running without arguments
# 0 = man, 1 = check inbox, 2 = check inbox --count
DEFAULT_ACTION=0
```

Use `block` or `unblock` command to block or unblock a user or instance.

### License

BSD 2-Clause License
