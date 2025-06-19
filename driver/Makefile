obj-m := fpga_driver.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install:
	sudo insmod fpga_driver.ko

remove:
	sudo rmmod fpga_driver

reload: remove install

log:
	dmesg | tail -20

status:
	lsmod | grep fpga_driver

.PHONY: all clean install remove reload log status
