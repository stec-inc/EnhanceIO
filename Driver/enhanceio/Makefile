# test
# Makefile for EnhanceIO block device caching.
#

COMMIT_REV ?= $(shell echo)
KERNEL_SOURCE_VERSION ?= $(shell uname -r)
KERNEL_TREE ?= /lib/modules/$(KERNEL_SOURCE_VERSION)/build
EXTRA_CFLAGS += -I$(KERNEL_TREE)/drivers/md -I./ -DCOMMIT_REV="\"$(COMMIT_REV)\""
EXTRA_CFLAGS += -I$(KERNEL_TREE)/include/ -I$(KERNEL_TREE)/include/linux 
# Check for RHEL/CentOS
RHEL5_VER ?= $(shell  if [ -e /etc/redhat-release ]; then grep 5.[0-9] /etc/redhat-release; else false; fi)
RHEL5_SETUP :=
ifneq "$(RHEL5_VER)" ""
	RHEL5_SETUP := rhel5-setup
	RHEL5_SPEC := /usr/src/redhat/SPECS/kernel.spec
	RHEL5_TREE := /usr/src/redhat/BUILD/kernel-2.6.18/linux-$(shell uname -r).$(shell uname -i)
	RHEL5_SRC := /usr/src/kernels/$(shell uname -r)-$(shell uname -i)
	KERNEL_TREE := $(RHEL5_TREE)
endif
# Check for OpenVZ (/proc/vz)
OPENVZ_VER ?= $(shell if [ -e /proc/vz ]; then grep 5.[0-9] /etc/redhat-release; else false; fi)
ifneq "$(OPENVZ_VER)" ""
        RHEL5_SPEC := /usr/src/redhat/SPECS/kernel-ovz.spec
        RHEL5_TREE := /usr/src/redhat/BUILD/ovzkernel-2.6.18/linux-$(shell uname -r).$(shell uname -i)
        KERNEL_TREE := $(RHEL5_TREE)
endif
obj-m	+= enhanceio.o enhanceio_lru.o enhanceio_fifo.o  enhanceio_rand.o
enhanceio-y	+= \
	eio_conf.o \
	eio_ioctl.o \
	eio_main.o \
	eio_mem.o \
	eio_policy.o \
	eio_procfs.o \
	eio_setlru.o \
	eio_subr.o \
	eio_ttc.o
enhanceio_fifo-y	+= eio_fifo.o
enhanceio_rand-y	+= eio_rand.o
enhanceio_lru-y	+= eio_lru.o
.PHONY: all
all: modules 
.PHONY:    modules
modules: $(RHEL5_SETUP)
	make -C $(KERNEL_TREE) M=$(PWD) modules V=0
.PHONY: modules_install
modules_install: modules
	install -o root -g root -m 0755 -d $(DESTDIR)/lib/modules/$(KERNEL_SOURCE_VERSION)/extra/enhanceio/
	install -o root -g root -m 0755 enhanceio.ko $(DESTDIR)/lib/modules/$(KERNEL_SOURCE_VERSION)/extra/enhanceio/
	install -o root -g root -m 0755 enhanceio_rand.ko $(DESTDIR)/lib/modules/$(KERNEL_SOURCE_VERSION)/extra/enhanceio/
	install -o root -g root -m 0755 enhanceio_fifo.ko $(DESTDIR)/lib/modules/$(KERNEL_SOURCE_VERSION)/extra/enhanceio/
	install -o root -g root -m 0755 enhanceio_lru.ko $(DESTDIR)/lib/modules/$(KERNEL_SOURCE_VERSION)/extra/enhanceio/
	depmod -a
.PHONY: install
install: modules_install
.PHONY: clean
clean:
	make -C $(KERNEL_TREE) M=$(PWD) clean
.PHONY: rhel5-setup
rhel5-setup: $(RHEL5_TREE)
	make -C $(RHEL5_TREE) oldconfig ; \
	make -C $(RHEL5_TREE) prepare modules_prepare
	ln -s -f $(RHEL5_SRC)/Module.symvers $(RHEL5_TREE)/Module.symvers
$(RHEL5_TREE):
	rpmbuild -bp --target=`uname -m` $(RHEL5_SPEC) 2>&1 | tee `dirname $(RHEL5_SPEC)`/prep.log

