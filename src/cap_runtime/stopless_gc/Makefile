# Makefile for cap_runtime/stopless_gc.
#
# On hasee cross-build:
#   make CROSS=1
# On CheriBSD/QEMU guest:
#   make

CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -g

ifeq ($(CROSS),1)
SDK     ?= $(HOME)/projs/stopless-java-gc/third_party/output/morello-sdk
SYSROOT ?= $(HOME)/projs/stopless-java-gc/third_party/output/rootfs-morello-purecap
CC       = $(SDK)/bin/clang
CFLAGS  += --target=aarch64-unknown-freebsd \
           -march=morello -mabi=purecap \
           --sysroot=$(SYSROOT) \
           -isystem $(SYSROOT)/usr/include
LDFLAGS += --target=aarch64-unknown-freebsd \
           -march=morello -mabi=purecap \
           --sysroot=$(SYSROOT)
endif

CFLAGS += -std=c11

OBJS = revoke.o forward_table.o handler.o allocator.o
LIB  = libstopless_gc.a

TESTS = test_basic test_multi test_alloc test_alloc_concurrent test_concurrent_move test_clg_probe

.PHONY: all clean

all: $(LIB) $(TESTS)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_basic: tests/test_basic.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -L. -lstopless_gc -lcheri_caprevoke

test_multi: tests/test_multi.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -L. -lstopless_gc -lcheri_caprevoke -lpthread

test_alloc: tests/test_alloc.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -L. -lstopless_gc -lcheri_caprevoke

test_alloc_concurrent: tests/test_alloc_concurrent.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -L. -lstopless_gc -lcheri_caprevoke -lpthread

test_concurrent_move: tests/test_concurrent_move.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -L. -lstopless_gc -lcheri_caprevoke -lpthread

clean:
	rm -f *.o $(LIB) $(TESTS)

test_clg_probe: tests/test_clg_probe.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -L. -lstopless_gc -lcheri_caprevoke
