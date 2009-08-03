obj-m := gm965temp.o

KDIR := /lib/modules/`uname -r`/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install:
	cp gm965temp.ko /lib/modules/`uname -r`/kernel/drivers/hwmon
	depmod

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
