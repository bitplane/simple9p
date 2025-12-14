CC ?= gcc
CFLAGS += -g -O0 -D_XOPEN_SOURCE=600 -Ilibixp/include
LDFLAGS += -static
LIBS = build/libixp.a -lpthread

SRCS = simple9p.c path.c fs_ops.c fs_io.c fs_stat.c fs_dir.c
OBJS = $(patsubst %.c,build/%.o,$(SRCS))
TARGET = build/simple9p

all: build libixp $(TARGET)

$(TARGET): $(OBJS) libixp
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

build/%.o: %.c server.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

libixp: | build
	cd libixp/lib/libixp && \
	for f in convert.c error.c map.c message.c request.c rpc.c server.c socket.c transport.c util.c timer.c client.c thread.c; do \
		$(CC) $(CFLAGS) -I../../include -c $$f -o $$(pwd)/../../../build/$${f%.c}.o || exit 1; \
	done
	ar rcs build/libixp.a build/convert.o build/error.o build/map.o build/message.o \
		build/request.o build/rpc.o build/server.o build/socket.o build/transport.o \
		build/util.o build/timer.o build/client.o build/thread.o

clean:
	rm -rf build

test/9pfuse/build/9pfuse:
	@if [ ! -d test/9pfuse ]; then \
		git clone -b qemount https://github.com/bitplane/9pfuse.git test/9pfuse; \
	fi
	cd test/9pfuse && meson setup build && meson compile -C build

test: $(TARGET) test/9pfuse/build/9pfuse
	cd test && ./run.sh

.PHONY: all clean libixp test
