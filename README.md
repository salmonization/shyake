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

# register on an instance
shyake register -u <username> -i https://shyake.eee.coffee
```

Config is stored at `~/.config/shyake/`.

You can create multiple profiles by specify a directory when init:

```sh
shyake init -c path/to/your/dir
```

And in that case you need always add `-c` option when you
want to use this profile.

**Check command**:

```sh
shyake check inbox
shyake check sent
# check metadata of a piece of mail
shyake check fQBjZnvJ56
```

**Send command**:

```sh
shyake -s "subject" -t recipient < body.txt
# first line of the input file will be the subject if -s is missing
shyake -t <recipient> < content.txt
# if you want to use heredoc
# (please be careful of your shell history)
shyake -s "subject" -t recipient <<EOF
```

**Fetch command**:

```sh
# fetch a piece of mail and decrypt it
shyake fetch fQBjZnvJ56
```

**Fingerprint command**:

```sh
# display your own fingerprint
shyake fingerprint
# check fingerprint of your communicators
shyake fingerprint
```

**Burn command**:

```sh
# this will delete a piece of mail
shyake burn fQBjZnvJ56
```

**Rotate command**:

```sh
# rotate your key pairs and
# your mail will be cleared at this point
shyake rotate
```

**Destroy command**:

```sh
# this will delete your local config and key pairs 
# and destruct your account on the instance.
# all messages sent to or from you will be cleared.
# your username will be locked forever
shyake destroy
```

### License

BSD 2-Clause License
