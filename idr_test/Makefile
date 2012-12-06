obj-m := idr_test.o
KDIR := /storage/kernel/kernel-3
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules
clean:
	rm -f *.ko  *.mod.c  *.mod.o  *.o  modules.order  Module.symvers

