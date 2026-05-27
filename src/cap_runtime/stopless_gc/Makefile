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
           -march=morello+c64 -mabi=purecap \
           --sysroot=$(SYSROOT) \
           -isystem $(SYSROOT)/usr/include
LDFLAGS += --target=aarch64-unknown-freebsd \
           -march=morello+c64 -mabi=purecap \
           --sysroot=$(SYSROOT)
endif

CFLAGS += -std=c11

OBJS = revoke.o forward_table.o handler.o
LIB  = libstopless_gc.a

TESTS = test_basic

.PHONY: all clean

all: $(LIB) $(TESTS)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_basic: tests/test_basic.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -L. -lstopless_gc -lcheri_caprevoke

clean:
	rm -f *.o $(LIB) $(TESTS)
