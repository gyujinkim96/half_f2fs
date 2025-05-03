# Makefile (for out-of-tree kernel module build)
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += f2fs.o

f2fs-y := dir.o file.o inode.o namei.o hash.o super.o inline.o \
          checkpoint.o gc.o data.o node.o segment.o recovery.o \
          shrinker.o extent_cache.o sysfs.o

f2fs-$(CONFIG_F2FS_STAT_FS) += debug.o
f2fs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
f2fs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
f2fs-$(CONFIG_FS_VERITY) += verity.o
f2fs-$(CONFIG_F2FS_FS_COMPRESSION) += compress.o
f2fs-$(CONFIG_F2FS_IOSTAT) += iostat.o

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install:
	sudo make -C $(KDIR) M=$(PWD) modules_install
	sudo depmod -a