CXX = arm-linux-gnueabi-g++
CXXFLAGS = -std=c++11 -Iarmel/usr/include/arm-linux-gnueabi -Iarmel/usr/include
LDFLAGS = -Wl,--unresolved-symbols=ignore-in-shared-libs -Larmel/usr/lib/arm-linux-gnueabi
LDLIBS = -lapt-pkg

all: disposal

upload: disposal
	mkdir -p $@
	cp $? $@
	git add $@
	git push -f origin `git write-tree --prefix=$@`:tags/arm-test
	git rm -r --cached $@

.PHONY: all upload
