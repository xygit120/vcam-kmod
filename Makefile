# SPDX-License-Identifier: GPL-2.0
#
# vcam -- kernel-side frame-sink pool for the 18.0.apk virtual-camera port.
#
# Build against the kernel tree that matches the device:
#
#   make -C /path/to/kernel/tree M=$PWD modules
#   # or
#   make KDIR=/path/to/kernel/tree modules
#
# For Android that is usually the GKI prebuilt plus the vendor modules, i.e.
#
#   make -C $ANDROID_KERNEL/common M=$PWD modules
#
# The offline host harness (no kernel tree needed):
#
#   make host          # compiles the module sources against shim/ and runs them

obj-m += vcam.o
vcam-y := src/vcam_main.o src/vcam_ctrl.o src/vcam_pool.o src/vcam_logic.o

# The uapi header is shared with userspace; keep it on the include path.
ccflags-y += -I$(src)/include

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

HOST_CC ?= cc

.PHONY: all modules host host-run clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf build

host:
	mkdir -p build
	$(HOST_CC) -std=gnu11 -DVCAM_HOST_BUILD=1 -Wall -Wextra -Werror \
		-Ishim -Iinclude host/vcam_host_test.c -o build/vcam_host_test

host-run: host
	./build/vcam_host_test
