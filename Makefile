# Builds jlb-c. `make` for this computer, `make windows` for Windows (needs MinGW-w64),
# `make test` to run the shared test programs.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

jlb-c: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm -lpthread

WIN_CC ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres

windows/jlb-c.exe: $(SRC) $(HDR) windows/jlb.ico
	mkdir -p build
	printf '1 ICON "windows/jlb.ico"\n' > build/jlb-c.rc
	$(WINDRES) -O coff -i build/jlb-c.rc -o build/jlb-c-icon.o
	$(WIN_CC) -O2 -Wall -std=c11 -static -o $@ $(SRC) build/jlb-c-icon.o -lm

windows: windows/jlb-c.exe

test: jlb-c
	./tests/run.sh ./jlb-c

clean:
	rm -rf jlb-c build

.PHONY: windows test clean
