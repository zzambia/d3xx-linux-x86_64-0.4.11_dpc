ifneq ("$(wildcard libftd3xx.dll)","")
ifneq (,$(findstring PE32+,$(shell file libftd3xx.dll)))
	CROSS_COMPILE := x86_64-w64-mingw32-
else
	CROSS_COMPILE := i686-w64-mingw32-
endif
AS = $(CROSS_COMPILE)as
LD = $(CROSS_COMPILE)ld
CC = $(CROSS_COMPILE)gcc-posix
CXX = $(CROSS_COMPILE)g++-posix
OBJDUMP = $(CROSS_COMPILE)objdump

DEMO0=streamer.exe
DEMO1=rw.exe
DEMO2=file_transfer.exe
DEMO3=zynqtest.exe
LIBS = -L . -lftd3xx -static
else
ifneq (,$(findstring 64-bit,$(shell file libftd3xx.so)))
ARCH=-m64
else
ARCH=-m32
endif
DEMO0=streamer
DEMO1=rw
DEMO2=file_transfer
DEMO3=zynqtest
LIBS = -L . -lftd3xx -pthread -lrt
endif

COMMON_FLAGS = -ffunction-sections -finline-limit=65535 -fmerge-all-constants \
			   -fno-stack-protector $(ARCH)
COMMON_CFLAGS = -g -O3 -Wall -Wextra $(COMMON_FLAGS) -fno-stack-check
CFLAGS = -std=c99  $(COMMON_CFLAGS) -D_POSIX_C_SOURCE
CXXFLAGS = -std=c++11 $(COMMON_CFLAGS)

all: $(DEMO0) $(DEMO1) $(DEMO2) $(DEMO3)

$(DEMO0): streamer.o
	$(CC) -Wl,--gc-sections $(COMMON_FLAGS) -o $@ $^ $(LIBS) -lstdc++

$(DEMO1): rw.o
	$(CC) -Wl,--gc-sections $(COMMON_FLAGS) -o $@ $^ $(LIBS)

$(DEMO2): file_transfer.o
	$(CC) -Wl,--gc-sections $(COMMON_FLAGS) -o $@ $^ $(LIBS) -lstdc++

$(DEMO3): zynqtest.o
	$(CC) -Wl,--gc-sections $(COMMON_FLAGS) -o $@ $^ $(LIBS) -lstdc++

clean:
	-rm -f streamer.o rw.o file_transfer.o zynqtest.o $(DEMO0) $(DEMO1) $(DEMO2) $(DEMO3)
