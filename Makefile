CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
ZIG ?= zig
WINDOWS_TARGET ?= x86_64-windows-gnu
WINDOWS_TOOL ?= build/fauxfat-qualify.exe
CLANG_FORMAT ?= clang-format-20

FAUXFAT_SOURCES := src/fauxfat.c
FAUXGPT_SOURCES := src/fauxgpt.c
BLOCK_SOURCES := $(FAUXFAT_SOURCES) $(FAUXGPT_SOURCES) src/fauxfat_block.c
INTERNAL_HEADERS := src/fauxbytes.h
STORAGE_HEADERS := include/fauxfat.h include/fauxgpt.h include/fauxfat_block.h $(INTERNAL_HEADERS)
FORMAT_FILES := $(wildcard src/*.[ch] src/*.cc src/*.cpp src/*.cxx src/*.hh src/*.hpp src/*.hxx \
	include/*.[ch] include/*.cc include/*.cpp include/*.cxx include/*.hh include/*.hpp include/*.hxx \
	tests/*.[ch] tests/*.cc tests/*.cpp tests/*.cxx tests/*.hh tests/*.hpp tests/*.hxx \
	tools/*.[ch] tools/*.cc tools/*.cpp tools/*.cxx tools/*.hh tools/*.hpp tools/*.hxx)

.PHONY: all format test test-fauxfat test-fauxgpt test-fauxfat-block test-faults test-all check-windows-tool windows-tool clean

all: tests/test_fauxfat tests/test_fauxgpt tests/test_fauxfat_block tests/test_fauxfat_faults

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

tests/test_fauxfat: $(FAUXFAT_SOURCES) include/fauxfat.h $(INTERNAL_HEADERS) tests/test_fauxfat.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FAUXFAT_SOURCES) tests/test_fauxfat.c -o $@

tests/test_fauxgpt: $(FAUXGPT_SOURCES) include/fauxgpt.h $(INTERNAL_HEADERS) tests/test_fauxgpt.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FAUXGPT_SOURCES) tests/test_fauxgpt.c -o $@

tests/test_fauxfat_block: $(BLOCK_SOURCES) $(STORAGE_HEADERS) tests/test_fauxfat_block.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BLOCK_SOURCES) tests/test_fauxfat_block.c -o $@

tests/test_fauxfat_faults: $(BLOCK_SOURCES) $(STORAGE_HEADERS) tests/test_fauxfat_faults.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BLOCK_SOURCES) tests/test_fauxfat_faults.c -o $@

test-fauxfat: tests/test_fauxfat
	./tests/test_fauxfat

test-fauxgpt: tests/test_fauxgpt
	./tests/test_fauxgpt

test-fauxfat-block: tests/test_fauxfat_block
	./tests/test_fauxfat_block

test: test-fauxfat test-fauxgpt test-fauxfat-block

test-faults: tests/test_fauxfat_faults
	./tests/test_fauxfat_faults

test-all: test test-faults

check-windows-tool: tools/fauxfat_qualify_win.c $(STORAGE_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only tools/fauxfat_qualify_win.c

windows-tool: $(WINDOWS_TOOL)

$(WINDOWS_TOOL): $(BLOCK_SOURCES) $(STORAGE_HEADERS) tools/fauxfat_qualify_win.c
	mkdir -p build
	$(ZIG) cc -target $(WINDOWS_TARGET) $(CPPFLAGS) -O2 -std=c99 -Wall -Wextra -Werror \
		$(BLOCK_SOURCES) tools/fauxfat_qualify_win.c \
		-lvirtdisk -lole32 -o $@

clean:
	rm -f tests/test_fauxfat tests/test_fauxgpt tests/test_fauxfat_block tests/test_fauxfat_faults $(WINDOWS_TOOL)
