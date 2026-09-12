CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
CLANG_FORMAT ?= clang-format-20
FORMAT_FILES := $(wildcard src/*.[ch] src/*.cc src/*.cpp src/*.cxx src/*.hh src/*.hpp src/*.hxx \
	include/*.[ch] include/*.cc include/*.cpp include/*.cxx include/*.hh include/*.hpp include/*.hxx \
	tests/*.[ch] tests/*.cc tests/*.cpp tests/*.cxx tests/*.hh tests/*.hpp tests/*.hxx \
	tools/*.[ch] tools/*.cc tools/*.cpp tools/*.cxx tools/*.hh tools/*.hpp tools/*.hxx)

.PHONY: all format test clean

all: tests/test_fauxfat

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

tests/test_fauxfat: src/fauxfat.c src/fauxfat_upcase.inc include/fauxfat.h tests/test_fauxfat.c
	$(CC) $(CPPFLAGS) $(CFLAGS) src/fauxfat.c tests/test_fauxfat.c -o $@

test: tests/test_fauxfat
	./tests/test_fauxfat

clean:
	rm -f tests/test_fauxfat
