#
# Makefile for EnhanceIO block device caching.
#
obj-$(CONFIG_ENHANCEIO)	+= enhanceio.o enhanceio_lru.o enhanceio_fifo.o
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
enhanceio_lru-y	+= eio_lru.o
