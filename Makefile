CXXFLAGS = -std=c++11
LDFLAGS = -Wl,--unresolved-symbols=ignore-in-shared-libs
LDLIBS = -lapt-pkg

all: disposal

.PHONY: all
