CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
CLANG_FORMAT ?= clang-format-20
FORMAT_FILES := $(wildcard src/*.[ch] src/*.cc src/*.cpp src/*.cxx src/*.hh src/*.hpp src/*.hxx \
	include/*.[ch] include/*.cc include/*.cpp include/*.cxx include/*.hh include/*.hpp include/*.hxx \
	tests/*.[ch] tests/*.cc tests/*.cpp tests/*.cxx tests/*.hh tests/*.hpp tests/*.hxx \
	tools/*.[ch] tools/*.cc tools/*.cpp tools/*.cxx tools/*.hh tools/*.hpp tools/*.hxx)

.PHONY: all format test test-fauxfat test-fauxgpt test-fauxfat-block test-faults test-all clean

all: tests/test_fauxfat tests/test_fauxgpt tests/test_fauxfat_block tests/test_fauxfat_faults

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

tests/test_fauxfat: src/fauxfat.c include/fauxfat.h tests/test_fauxfat.c
	$(CC) $(CPPFLAGS) $(CFLAGS) src/fauxfat.c tests/test_fauxfat.c -o $@

tests/test_fauxgpt: src/fauxgpt.c include/fauxgpt.h tests/test_fauxgpt.c
	$(CC) $(CPPFLAGS) $(CFLAGS) src/fauxgpt.c tests/test_fauxgpt.c -o $@

tests/test_fauxfat_block: src/fauxfat.c src/fauxgpt.c src/fauxfat_block.c include/fauxfat.h include/fauxgpt.h include/fauxfat_block.h tests/test_fauxfat_block.c
	$(CC) $(CPPFLAGS) $(CFLAGS) src/fauxfat.c src/fauxgpt.c src/fauxfat_block.c tests/test_fauxfat_block.c -o $@

tests/test_fauxfat_faults: src/fauxfat.c src/fauxgpt.c src/fauxfat_block.c include/fauxfat.h include/fauxgpt.h include/fauxfat_block.h tests/test_fauxfat_faults.c
	$(CC) $(CPPFLAGS) $(CFLAGS) src/fauxfat.c src/fauxgpt.c src/fauxfat_block.c tests/test_fauxfat_faults.c -o $@

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

clean:
	rm -f tests/test_fauxfat tests/test_fauxgpt tests/test_fauxfat_block tests/test_fauxfat_faults
