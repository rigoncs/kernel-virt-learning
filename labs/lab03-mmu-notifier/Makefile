# Lab 03 Makefile
# 前置: 内核头文件在 /lib/modules/$(shell uname -r)/build
obj-m += notifier_demo.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: notifier_demo.ko trigger

notifier_demo.ko: notifier_demo.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules

trigger: trigger.c
	gcc -O2 -o trigger trigger.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f trigger

.PHONY: all clean
