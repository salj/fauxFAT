CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
CLANG_FORMAT ?= clang-format-20
FORMAT_FILES := $(wildcard src/*.[ch] src/*.cc src/*.cpp src/*.cxx src/*.hh src/*.hpp src/*.hxx \
	include/*.[ch] include/*.cc include/*.cpp include/*.cxx include/*.hh include/*.hpp include/*.hxx \
	tests/*.[ch] tests/*.cc tests/*.cpp tests/*.cxx tests/*.hh tests/*.hpp tests/*.hxx \
	tools/*.[ch] tools/*.cc tools/*.cpp tools/*.cxx tools/*.hh tools/*.hpp tools/*.hxx)

.PHONY: all format test test-fauxfat test-fauxgpt clean

all: tests/test_fauxfat tests/test_fauxgpt

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

tests/test_fauxfat: src/fauxfat.c include/fauxfat.h tests/test_fauxfat.c
	$(CC) $(CPPFLAGS) $(CFLAGS) src/fauxfat.c tests/test_fauxfat.c -o $@

tests/test_fauxgpt: src/fauxgpt.c include/fauxgpt.h tests/test_fauxgpt.c
	$(CC) $(CPPFLAGS) $(CFLAGS) src/fauxgpt.c tests/test_fauxgpt.c -o $@

test-fauxfat: tests/test_fauxfat
	./tests/test_fauxfat

test-fauxgpt: tests/test_fauxgpt
	./tests/test_fauxgpt

test: test-fauxfat test-fauxgpt

clean:
	rm -f tests/test_fauxfat tests/test_fauxgpt
