# Lab 08 Makefile — ARM64 内核模块交叉编译
# 用法:
#   make KDIR=/path/to/your/linux-tree        # 交叉编译 (x86 主机)
#   make KDIR=/lib/modules/$(uname -r)/build  # 本机编译 (ARM64 主机)
# 必须保证 KDIR 指向的内核版本 == 目标机运行内核版本 (vermagic 匹配)
CROSS   ?= aarch64-linux-gnu-
KDIR    ?= /lib/modules/$(shell uname -r)/build
ARCH    ?= arm64
PWD     := $(shell pwd)

obj-m += hello.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS) clean

.PHONY: all clean
