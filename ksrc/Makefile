KDIR = /lib/modules/`uname -r`/build
MODULE_NAME = rdma_kclient

SRCS = kclient.c
# SRCS = kclient.c krdma.c kcommon.c
OBJS = $(SRCS:.c=.o)

obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-y = $(OBJS)

MY_CFLAGS += -g -DDEBUG

all:
	make -C $(KDIR) M=$(PWD) modules

debug:
	make -C $(KDIR) M=$(PWD) modules EXTRA_CFLAGS="$(MY_CFLAGS)"

clean:
	make -C $(KDIR) M=$(PWD) clean 
