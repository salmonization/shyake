## Shyake

### Overview

Shyake is an **end-to-end encrypted mailing system** powered by
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

**Check command**:

```sh
shyake check inbox
shyake check sent
# check metadata of a piece of mail
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

You can also use heredoc, but please be careful of your shell history.

```sh
shyake -s "This is the subject" -t flat_white <<EOF
```

**Fetch command**:

This will fetch a piece of mail and decrypt it.

```sh
shyake fetch fQBjZnvJ56
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

**Burn command**:

This will delete a piece of mail.

```sh
shyake burn fQBjZnvJ56
```

**Rotate command**:

This will rotate your key pairs and clear all MESSAGES sent to or from you.

```sh
shyake rotate
```

**Destroy command**:

This will delete your local configuration and key pairs, and will also
destruct your account on the instance. All MESSAGES sent to or from you
will be cleared. Your username will be permanently locked and cannot be
registered again on this instance.

```sh
shyake destroy
```

### License

BSD 2-Clause License
