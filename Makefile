obj-m += marco_fq.o

KDIR = /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod marco_fq.ko

unload:
	sudo rmmod marco_fq

check:
	sudo dmesg