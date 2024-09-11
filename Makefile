CC=gcc
CFLAGS=$(shell pkg-config --cflags libpmem)
LIBS=$(shell pkg-config --libs libpmem libpmemobj)
TARGET=a.out
DEBUG=-g

# 申请内存类型
TRUERAM=DRAM

$(TARGET): wbtree.c
	$(CC) $(DEBUG) -D$(TRUERAM) -o $(TARGET) $(CFLAGS) wbtree.c $(LIBS)

clean:
	rm -rf a.out
