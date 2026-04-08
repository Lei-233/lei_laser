# 定义交叉编译器
CROSS_COMPILE := /home/lei/tspi_linux_sdk/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-

# 交叉编译工具链标准库路径
EXTRA_CFLAGS := -I/home/lei/tspi_linux_sdk/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/lib/gcc/aarch64-linux-gnu/6.3.1/include


obj-m += galvo.o

galvo-objs := galvo_drv.o galvo_ioctl.o

KDIR :=/home/lei/tspi_linux_sdk/kernel    #内核源码所在虚拟机 ubuntu 的实际路径
PWD ?= $(shell pwd)
all:
	make -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) EXTRA_CFLAGS=$(EXTRA_CFLAGS) modules	#make 操作
clean:
	make -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=$(CROSS_COMPILE) clean EXTRA_CFLAGS=$(EXTRA_CFLAGS)		#make clean 操作

