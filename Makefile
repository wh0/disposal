CXX = arm-linux-gnueabi-g++
CXXFLAGS = -std=c++11 -Iarmel/usr/include/arm-linux-gnueabi -Iarmel/usr/include
LDFLAGS = -Wl,--unresolved-symbols=ignore-in-shared-libs -Larmel/usr/lib/arm-linux-gnueabi
LDLIBS = -lapt-pkg

all: disposal

.PHONY: all
