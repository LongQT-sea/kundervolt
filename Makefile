MODULE_NAME := kundervolt
MODULE_VERSION := 0.1

KVERSION ?= $(shell uname -r)

obj-m += kundervolt.o
kundervolt-objs := module.o fp_util.o ftoa/ftoa.o
CFLAGS_fp_util.o := $(CC_FLAGS_FPU)
CFLAGS_REMOVE_fp_util.o += $(CC_FLAGS_NO_FPU)
CFLAGS_ftoa/ftoa.o := $(CC_FLAGS_FPU)
CFLAGS_REMOVE_ftoa/ftoa.o += $(CC_FLAGS_NO_FPU)

PWD := $(CURDIR)

all:
	make -C /lib/modules/$(KVERSION)/build/ M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVERSION)/build/ M=$(PWD) clean

compile-commands: all
	python3 /usr/src/kernels/$(KVERSION)/scripts/clang-tools/gen_compile_commands.py

install: all
	make -C /lib/modules/$(KVERSION)/build/ M=$(PWD) modules_install

dkms-install:
	sudo install -D -t /usr/src/$(MODULE_NAME)-$(MODULE_VERSION) *.c *.h dkms.conf README.md Makefile COPYING
	sudo install -D -t /usr/src/$(MODULE_NAME)-$(MODULE_VERSION)/ftoa ftoa/*
	sudo dkms add -m $(MODULE_NAME) -v $(MODULE_VERSION)
	sudo dkms build -m $(MODULE_NAME) -v $(MODULE_VERSION)
	sudo dkms install -m $(MODULE_NAME) -v $(MODULE_VERSION)
	@sudo modprobe kundervolt || echo "Failed to load module. The first time you use DKMS you need to register the DKMS signing key with Secure Boot. Follow the guide on https://github.com/dkms-project/dkms#secure-boot"

dkms-uninstall:
	sudo modprobe -r kundervolt
	sudo dkms remove -m $(MODULE_NAME) -v $(MODULE_VERSION) --all
	sudo rm -r /usr/src/$(MODULE_NAME)-$(MODULE_VERSION)
