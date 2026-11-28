# pacectl (README.md): the panel's vsync for the VM's refresh tick (oneios-tool phone vm-boot --vsync-lock).
#   make            build/android/pacectl (dynamic: it asks surfaceflinger through the NDK's AChoreographer, API 33)
#   make test       (with check-order: the seqlock's barriers in the NDK's arm64 assembly) the record, the arithmetic and the publisher against a stand-in page and clock, on the build host
# Needs the NDK (../host.mk).
include $(dir $(lastword $(MAKEFILE_LIST)))../host.mk
PACE_CC ?= $(TOOLS)/aarch64-linux-android33-clang
WARN := -Wall -Wextra -Werror
CFLAGS ?= -O2 -g
A := build/android
H := build/host
DEPS := src/pacectl.c ../display/src/xhd.c ../display/src/xhd.h ../display/src/scantime.h ../../kernel/patches/oneios-hosted/display/vsync_lock.h

all: $(A)/pacectl

# libandroid has AChoreographer and ALooper; an executable fails to link on an undefined symbol by itself.
$(A)/pacectl: $(DEPS)
	@mkdir -p $(A)
	$(PACE_CC) -std=gnu11 $(CFLAGS) $(WARN) src/pacectl.c ../display/src/xhd.c -landroid -lm -o $@

$(H)/pacectl: $(DEPS)
	@mkdir -p $(H)
	$(CC) -std=gnu11 -O1 -g $(WARN) src/pacectl.c ../display/src/xhd.c -lm -o $@
$(H)/test_pace: tests/test_pace.c ../../kernel/patches/oneios-hosted/display/vsync_lock.h ../display/src/scantime.h
	@mkdir -p $(H)
	$(CC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined tests/test_pace.c -lm -o $@

test: $(H)/test_pace $(H)/pacectl check-order
	$(H)/test_pace $(H)/pacectl

# The seqlock's barriers where arm64 needs them (the x86 test above cannot show a reordering): the NDK's assembly of the
# header, read by tests/check_order.sh.
check-order: tests/order_probe.c tests/check_order.sh ../../kernel/patches/oneios-hosted/display/vsync_lock.h
	$(PACE_CC) -std=gnu11 -O2 $(WARN) -S -o - tests/order_probe.c | sh tests/check_order.sh

clean:
	rm -rf build
.PHONY: all test check-order clean
