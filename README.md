# 🪙 9d - an embeddable 9p server

A small 9p server made to serve the root filesystem of embedded operating
systems. Used in [mountin](https://github.com/bitplane/mountin) for disk image
recovery.

Uses [libixp](https://github.com/0intro/libixp) with UNIX extensions (where
supported).

## Building

Run `make deps`, then `make` to build a server with connected-stream and network
transports.

For guests that only use an already-connected stream, such as a serial port,
build with `make NETWORK=0`.

`make release` produces an optimized, stripped binary. Embedded builds can set
`S9_PATH_MAX` to reduce request stack usage; `make check-embedded` verifies the
1024-byte appliance profile.

## Usage

```text
9d [-d] [-r] [-p address] [directory]
```

Use `-r` for read only.

Without a dir, 9d serves the platform's filesystem root: `/` on Unix-like and
RISC OS systems, or a path above the volumes elsewhere. 

The default address for a network server is `tcp!localhost!564`.
Use `-p -` for stdio or `-p stream!path` for an existing device like a serial
port.

## Status

This is slowly evolving into something that actually works. It's becoming more
robust, but don't consider it trustworthy - use at your own risk.

## License

WTFPL with one additional clause:

* Don't blame me.

Do what you want, but you get what you pay for.
