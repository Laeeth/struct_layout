ifneq ($(KERNELRELEASE),)

obj-m += dummy.o

CFLAGS_dummy.o = -fplugin=$(M)/../struct_layout.so -fplugin-arg-struct_layout-output=$(LAYOUT_OUTPUT)
ifneq ($(TARGET_STRUCT),)
CFLAGS_dummy.o += -fplugin-arg-struct_layout-struct=$(TARGET_STRUCT)
endif

$(warning $(CFLAGS_dummy.o))

else

KDIR ?= /lib/modules/$(shell uname -r)/build
M=$(abspath $(dir $(lastword $(MAKEFILE_LIST))))

all:
	make -C $(KDIR) M=$(M) modules

clean:
	make -C $(KDIR) M=$(M) clean

endif
