CC      = clang
CFLAGS  = -O2 -Wall -Wno-unused-variable
OBJC    = -fobjc-arc

all: build/btkbd build/btkbd-inject

build/btkbd: src/btkbd.c src/hci.c src/hidparse.c src/hci.h src/hidparse.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ src/btkbd.c src/hci.c src/hidparse.c -framework IOKit -framework CoreFoundation

build/btkbd-inject: src/inject.m
	@mkdir -p build
	$(CC) $(CFLAGS) $(OBJC) -o $@ src/inject.m -framework AppKit -framework ApplicationServices -framework IOKit -framework SystemConfiguration

firmware:
	sh scripts/get-firmware.sh

install: all
	sh scripts/install.sh

uninstall:
	sh scripts/uninstall.sh

clean:
	rm -rf build

.PHONY: all firmware install uninstall clean
