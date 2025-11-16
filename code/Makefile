obj-m += mem_tool.o

mem_tool-objs := entry.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

ccflags-y := -I$(PWD) -Wno-declaration-after-statement -Wno-incompatible-pointer-types

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install: all
	sudo insmod mem_tool.ko

uninstall:
	sudo rmmod mem_tool

.PHONY: all clean install uninstall