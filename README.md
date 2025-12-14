# A simple 9p server

A smallish (<1MB) 9p server that can be static linked in busybox images.

Built for [qemount](https://github.com/bitplane/qemount)

Uses [libixp](https://github.com/0intro/libixp)

## Status

This was cobbled together in a few hours using Claude for a proof of concept.
It will eventually evolve into the default `qemount`'s back-end, unless I find
something better.

Probably safe for read access in non-critical situations, but don't trust
anything there's not a test for, and you've read the test, and... well...
just don't trust it yet.

## License

WTFPL with one additional clause:

* Don't blame me.

Do whatever the fuck you want with it, but if it goes wrong it's on you.
