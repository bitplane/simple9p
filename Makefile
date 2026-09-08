include deps.mk

CC ?= gcc
AR ?= ar
STRIP ?= strip
API_CPPFLAGS ?= -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L
CPPFLAGS += $(API_CPPFLAGS) -Ilibixp/include
CFLAGS += -g -O0
DEPFLAGS = -MMD -MP
THREAD_LIBS ?= -lpthread
LIBS = build/libixp.a $(THREAD_LIBS)
LINK.c = $(CC) $(LDFLAGS)
LIBIXP_CFLAGS = $(filter-out -Wall -Wextra -Wpedantic -Wconversion \
	-Wshadow -Wformat=2 -Werror,$(CFLAGS))
WARNING_CFLAGS = -g -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wformat=2 -Werror
SANITIZER_FLAGS = -g -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined
RELEASE_CFLAGS ?= -Os -DNDEBUG
EMBEDDED_PATH_MAX ?= 1024
MAX_FIDS ?= 1024
CPPFLAGS += -DS9_MAX_FIDS=$(MAX_FIDS)

STATIC ?= 1
ifeq ($(STATIC),1)
LDFLAGS += -static
else ifneq ($(STATIC),0)
$(error STATIC must be 0 or 1)
endif

NETWORK ?= 1
ifeq ($(NETWORK),0)
CPPFLAGS += -DNINED_NO_NETWORK
else ifneq ($(NETWORK),1)
$(error NETWORK must be 0 or 1)
endif

PLATFORM ?= posix
SRCS = 9d.c alloc.c path.c namespace.c platform_$(PLATFORM).c \
	fs_ops.c fs_io.c fs_stat.c fs_dir.c
OBJS = $(patsubst %.c,build/%.o,$(SRCS))
TARGET = build/9d
LIBIXP_NAMES = convert error map message request server transport util timer \
	thread
ifeq ($(NETWORK),1)
LIBIXP_NAMES += rpc socket client
endif
LIBIXP_SRCS = $(addprefix libixp/lib/libixp/,$(addsuffix .c,$(LIBIXP_NAMES)))
LIBIXP_OBJS = $(addprefix build/libixp/,$(addsuffix .o,$(LIBIXP_NAMES)))
DEPS = $(OBJS:.o=.d) $(LIBIXP_OBJS:.o=.d) \
	build/platform_posix-synthetic.d

ifeq ($(wildcard libixp/lib/libixp),)
ifneq ($(filter deps clean distclean dist,$(MAKECMDGOALS)),)
else
$(error libixp is missing; run 'make deps' first)
endif
endif

all: $(TARGET)

$(TARGET): $(OBJS) build/libixp.a
	$(LINK.c) -o $@ $(OBJS) $(LIBS)

build/%.o: %.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

build/libixp/%.o: libixp/lib/libixp/%.c | build/libixp
	$(CC) $(CPPFLAGS) $(LIBIXP_CFLAGS) $(DEPFLAGS) \
		-Ilibixp/include -c $< -o $@

build/libixp.a: $(LIBIXP_OBJS)
	$(AR) rcs $@ $(LIBIXP_OBJS)

build build/libixp:
	mkdir -p $@

deps:
	./scripts/deps.sh

clean:
	rm -rf build

distclean: clean
	./scripts/clean-deps.sh
	rm -rf dist

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(RELEASE_CFLAGS)" all
	$(STRIP) $(TARGET)

dist: deps
	VERSION="$(VERSION)" ./scripts/dist.sh

test/9pfuse/build/9pfuse:
	@if [ ! -d test/9pfuse ]; then \
		git clone -b mountin-2026-08-15 https://github.com/bitplane/9pfuse.git test/9pfuse; \
	fi
	cd test/9pfuse && meson setup build && meson compile -C build

test: test/9pfuse/build/9pfuse
	$(MAKE) test-build-options
	$(MAKE) test-namespace
	$(MAKE) test-allocations
	$(MAKE) test-protocol
	$(MAKE) $(TARGET)
	cd test && ./run.sh

test-build-options:
	./test/build_options.sh

test-namespace: build/namespace_test
	./build/namespace_test

test-protocol: build/protocol_test build/9d build/9d-riscos build/9d-synthetic
	./build/protocol_test ./build/9d ./build/9d-synthetic
	./build/protocol_test ./build/9d-riscos

test-allocations: build/allocation_test
	./build/allocation_test

check-warnings:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(WARNING_CFLAGS)" LIBIXP_CFLAGS="-g -O2 -w" LDFLAGS= \
		test-namespace test-allocations test-protocol

check-sanitizers:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) CFLAGS="$(SANITIZER_FLAGS)" \
		LIBIXP_CFLAGS="$(SANITIZER_FLAGS) -w" \
		LDFLAGS="-fsanitize=address,undefined" \
		test-namespace test-allocations test-protocol

check-embedded:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(WARNING_CFLAGS) -DS9_PATH_MAX=$(EMBEDDED_PATH_MAX)" \
		LIBIXP_CFLAGS="-g -O2 -w" LDFLAGS= \
		test-namespace test-allocations test-protocol

build/namespace_test: test/namespace_test.c namespace.c namespace.h path.c \
		alloc.c server.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ \
		test/namespace_test.c namespace.c path.c alloc.c

build/protocol_test: test/protocol_test.c build/libixp.a | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test/protocol_test.c $(LIBS)

build/allocation_test: test/allocation_test.c alloc.c path.c namespace.c \
		platform_posix.c fs_ops.c fs_stat.c build/libixp.a | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DNINED_TESTING -o $@ \
		test/allocation_test.c alloc.c path.c namespace.c platform_posix.c \
		fs_ops.c fs_stat.c $(LIBS)

build/9d-riscos: 9d.c alloc.c path.c namespace.c platform_riscos.c \
		fs_ops.c fs_io.c fs_stat.c fs_dir.c server.h namespace.h \
		platform.h build/libixp.a | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
		9d.c alloc.c path.c namespace.c platform_riscos.c \
		fs_ops.c fs_io.c fs_stat.c fs_dir.c $(LIBS)

build/platform_posix-synthetic.o: platform_posix.c platform.h namespace.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) \
		-Dplatform_namespace_init=platform_namespace_init_native \
		-Dplatform_namespace_discover=platform_namespace_discover_native \
		-Dplatform_remove=platform_remove_native \
		-c platform_posix.c -o $@

build/9d-synthetic: 9d.c alloc.c path.c namespace.c \
		test/platform_synthetic.c fs_ops.c fs_io.c fs_stat.c fs_dir.c \
		server.h namespace.h build/platform_posix-synthetic.o build/libixp.a | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
		9d.c alloc.c path.c namespace.c test/platform_synthetic.c \
		fs_ops.c fs_io.c fs_stat.c fs_dir.c \
		build/platform_posix-synthetic.o $(LIBS)

-include $(DEPS)

.PHONY: all deps clean distclean release dist test test-build-options \
	test-namespace test-protocol test-allocations check-warnings \
	check-sanitizers check-embedded
