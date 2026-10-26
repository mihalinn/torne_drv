# SPDX-License-Identifier: GPL-2.0-only
#
#   make            build the pcscd reader driver (ifd/libifd_torne.so) and the user-space test tool (tools/torne_user)
#   make module     build the kernel module against the running kernel (to try it; use install.sh for DKMS)
#   make clean

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C ifd
	$(MAKE) -C tools

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver modules

clean:
	$(MAKE) -C ifd clean
	$(MAKE) -C tools clean
	-$(MAKE) -C $(KDIR) M=$(CURDIR)/driver clean

.PHONY: all module clean
