#!/bin/sh
set -eu

target=$1

case "$target" in
    x86_64-linux|aarch64-linux)
        make release \
            RELEASE_CFLAGS="-Os -DNDEBUG -DS9_PATH_MAX=1024"
        ;;
    i386-aros|aarch64-aros)
        make release \
            PLATFORM=amiga NETWORK=0 STATIC=0 THREAD_LIBS= \
            STRIP="$STRIP --strip-unneeded -R.comment" \
            API_CPPFLAGS=-D_POSIX_C_SOURCE=200809L \
            RELEASE_CFLAGS="-Os -fno-common -fno-asynchronous-unwind-tables -fno-unwind-tables -DS9_PATH_MAX=1024"
        ;;
    x86_64-netbsd|aarch64-netbsd)
        make release \
            LDFLAGS=-static \
            RELEASE_CFLAGS="-Os -DNDEBUG -DS9_PATH_MAX=1024"
        ;;
    x86_64-illumos)
        make release \
            NETWORK=0 STATIC=0 THREAD_LIBS= \
            API_CPPFLAGS="-D_XOPEN_SOURCE=700 -D__EXTENSIONS__ -D_REENTRANT" \
            RELEASE_CFLAGS="-Os -DNDEBUG -DS9_PATH_MAX=1024"
        ;;
    x86_64-haiku|aarch64-haiku)
        make release \
            NETWORK=0 STATIC=0 THREAD_LIBS= \
            STRIP="$STRIP --strip-unneeded" \
            RELEASE_CFLAGS="-Os -g0 -DNDEBUG -DS9_PATH_MAX=1024"
        ;;
    arm-riscos)
        make release \
            PLATFORM=riscos NETWORK=0 THREAD_LIBS= \
            API_CPPFLAGS="-D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L" \
            RELEASE_CFLAGS="-Os -fno-common -DS9_PATH_MAX=1024"
        ;;
    x86_64-darwin)
        make release \
            NETWORK=0 STATIC=0 THREAD_LIBS= \
            STRIP="$STRIP -S" \
            API_CPPFLAGS=-D_XOPEN_SOURCE=600 \
            RELEASE_CFLAGS="-Os -g0 -DNDEBUG -DS9_PATH_MAX=1024" \
            LDFLAGS=-Wl,-dead_strip
        ;;
    *)
        echo "Unsupported release target: $target" >&2
        exit 1
        ;;
esac
