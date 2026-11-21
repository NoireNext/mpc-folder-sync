# Mitgelieferte Programme neu bauen / Rebuilding the bundled programs

`Content/bin/{arm32,arm64}/` enthält rsync und Dropbear als statische Binaries. Quelltext und
Lizenzen: siehe `Content/bin/THIRD_PARTY.txt`. Hier steht, wie sie entstanden sind.
(rsync steht unter GPLv3: Dieses Dokument und der Verweis auf den Upstream-Tag sind das
"Angebot" des Quelltexts. Wer Binaries weitergibt, muss dasselbe tun.)

`Content/bin/{arm32,arm64}/` holds rsync and Dropbear as static binaries; the exact source
versions and licences are in `Content/bin/THIRD_PARTY.txt`.

## Werkzeug / toolchain

zig 0.17.1 als C-Cross-Compiler (`python -m ziglang cc`), Ziele (targets):

- arm32: `arm-linux-musleabihf`, `-mcpu=cortex_a17`
- arm64: `aarch64-linux-musl`, `-mcpu=cortex_a55`

## rsync 3.4.1 (Tag `v3.4.1`, Commit `3305a7a`, https://github.com/RsyncProject/rsync)

```
./configure --host=<ziel> --disable-openssl --disable-xxhash --disable-zstd --disable-lz4 \
  --disable-md2man --disable-acl-support --disable-xattr-support --disable-iconv \
  --disable-locale --with-included-popt --with-included-zlib CFLAGS="-Os" LDFLAGS="-static"
make
```

## Dropbear (Commit `99baf18`, https://github.com/mkj/dropbear)

```
./configure --host=<ziel> --enable-static --disable-zlib --disable-pam --disable-lastlog \
  --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc \
  --disable-pututline --disable-pututxline CFLAGS="-Os" LDFLAGS="-s"
make MULTI=1 PROGRAMS="dbclient dropbearconvert" STATIC=1
```

Das Ergebnis `dropbearmulti` wird als `Content/bin/<arch>/dropbearmulti` abgelegt, rsync als
`Content/bin/<arch>/rsync`.
