MODULE = hack

obj-m :=$(MODULE).o
$(MODULE)-objs += core.o
$(MODULE)-objs += touch.o
$(MODULE)-objs += memory.o
$(MODULE)-objs += server.o

EXTRA_CFLAGS += -DOPEN_LOG=0	# 是否开启Debug日志输出

EXTRA_CFLAGS += -O2                # 优化等级2
EXTRA_CFLAGS += -fvisibility=hidden  # 隐藏符号
EXTRA_CFLAGS += -fomit-frame-pointer # 省略帧指针
EXTRA_CFLAGS += -g0 # 去除调试信息
EXTRA_CFLAGS += -fno-pic -fvisibility=hidden
EXTRA_CFLAGS += -fno-stack-protector

# ARM架构优化
EXTRA_CFLAGS += -march=armv8-a     # 适用于ARMv8架构

all:
	make -C $(KDIR) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean