# pagecensus (what holds the phone's 2 MiB blocks; vm-boot --pagecensus build/android/pagecensus)
#   make        build/android/pagecensus (static, for the phone)
#   make test   the classifier on a made-up flags file, on the build host
include $(dir $(lastword $(MAKEFILE_LIST)))../host.mk
ANDROID_CC ?= $(TOOLS)/aarch64-linux-android30-clang
WARN := -Wall -Wextra -Werror
all: build/android/pagecensus
build/android/pagecensus: pagecensus.c
	@mkdir -p $(dir $@)
	$(ANDROID_CC) -std=gnu11 -O2 $(WARN) -static $< -o $@
build/host/pagecensus: pagecensus.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined $< -o $@
test: build/host/pagecensus
	sh test.sh build/host/pagecensus
clean:
	rm -rf build
.PHONY: all test clean
