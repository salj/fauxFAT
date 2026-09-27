BUILD ?= build
CC ?= cc
AR ?= ar
HOST_CC ?= cc
ARM_CC ?= clang
ARM_AR ?= llvm-ar
RISCV_CC ?= clang
OBJDUMP ?= llvm-objdump
NM ?= nm
LD_LLD ?= ld.lld

CPPFLAGS_COMMON := -Iinclude -Isrc
CFLAGS ?= -O2
GENERIC_CFLAGS := -std=c99 -Wall -Wextra -Werror
M0PLUS_CFLAGS := -std=c99 -O3 -ffreestanding -fomit-frame-pointer -ffunction-sections -fdata-sections -Wall -Wextra -Werror
HOST_CFLAGS := -std=c11 -O3 -fomit-frame-pointer -ffunction-sections -fdata-sections -Wall -Wextra -Werror
ARMFLAGS := -target thumbv6m-none-eabi -mcpu=cortex-m0plus -mthumb

COMMON_NAMES := xxh32_oneshot xxh32_stream xxh32_copy xxh32_alloc xxh32_canonical xxh32_version
GENERIC_OBJS := $(addprefix $(BUILD)/generic/,$(addsuffix .o,$(COMMON_NAMES)))
M0PLUS_OBJS := $(addprefix $(BUILD)/m0plus/,$(addsuffix .o,$(COMMON_NAMES))) \
	$(BUILD)/m0plus/xxh32_m0plus_oneshot.o \
	$(BUILD)/m0plus/xxh32_m0plus_stream.o

.PHONY: all generic m0plus rp2040 test clean

all: generic
generic: $(BUILD)/generic/libxxh32_generic.a
m0plus: $(BUILD)/m0plus/libxxh32_m0plus.a
rp2040: m0plus

$(BUILD)/generic $(BUILD)/m0plus $(BUILD)/host:
	mkdir -p $@

$(BUILD)/generic/%.o: src/%.c src/xxh32_internal.h include/xxhash.h | $(BUILD)/generic
	$(CC) $(CPPFLAGS_COMMON) $(CFLAGS) $(GENERIC_CFLAGS) -c $< -o $@

$(BUILD)/generic/libxxh32_generic.a: $(GENERIC_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/m0plus/%.o: src/%.c src/xxh32_internal.h include/xxhash.h | $(BUILD)/m0plus
	$(ARM_CC) $(CPPFLAGS_COMMON) $(M0PLUS_CFLAGS) $(ARMFLAGS) -DXXH32_BACKEND_M0PLUS=1 -c $< -o $@

$(BUILD)/m0plus/xxh32_m0plus_oneshot.o: src/m0plus/xxh32_oneshot.S | $(BUILD)/m0plus
	$(ARM_CC) $(ARMFLAGS) -c $< -o $@

$(BUILD)/m0plus/xxh32_m0plus_stream.o: src/m0plus/xxh32_stream.S | $(BUILD)/m0plus
	$(ARM_CC) $(ARMFLAGS) -c $< -o $@

$(BUILD)/m0plus/libxxh32_m0plus.a: $(M0PLUS_OBJS)
	$(ARM_AR) rcs $@ $^

$(BUILD)/host/test_xxh32: tests/test_xxh32.c src/xxh32_oneshot.c src/xxh32_stream.c src/xxh32_internal.h include/xxhash.h | $(BUILD)/host
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror $(CPPFLAGS_COMMON) \
		tests/test_xxh32.c src/xxh32_oneshot.c src/xxh32_stream.c -ldl -o $@

$(BUILD)/host/test_public_api: tests/test_public_api.c $(addprefix src/,$(addsuffix .c,$(COMMON_NAMES))) src/xxh32_internal.h include/xxhash.h | $(BUILD)/host
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror $(CPPFLAGS_COMMON) \
		tests/test_public_api.c $(addprefix src/,$(addsuffix .c,$(COMMON_NAMES))) -o $@

$(BUILD)/host/test_namespace: tests/test_namespace.c src/xxh32_oneshot.c src/xxh32_stream.c src/xxh32_internal.h include/xxhash.h | $(BUILD)/host
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -DXXH_NAMESPACE=tiny_ $(CPPFLAGS_COMMON) \
		tests/test_namespace.c src/xxh32_oneshot.c src/xxh32_stream.c -o $@

$(BUILD)/host/test_build_compat.o: tests/test_build_compat.c include/xxhash.h | $(BUILD)/host
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -c $< -o $@

test: $(BUILD)/host/test_xxh32 $(BUILD)/host/test_public_api $(BUILD)/host/test_namespace $(BUILD)/host/test_build_compat.o m0plus generic
	$(BUILD)/host/test_xxh32
	$(BUILD)/host/test_public_api
	$(BUILD)/host/test_namespace
	CC=$(HOST_CC) tests/test_unsupported_poison.sh
	CC=$(HOST_CC) NM=$(NM) tests/test_no_stream.sh
	ARM_CC=$(ARM_CC) NM=$(NM) OBJDUMP=$(OBJDUMP) tests/test_m0plus_codegen.sh
	ARM_CC=$(ARM_CC) RISCV_CC=$(RISCV_CC) NM=$(NM) tests/test_portable_targets.sh
	ARM_CC=$(ARM_CC) NM=$(NM) LD_LLD=$(LD_LLD) tests/test_archive_members.sh
	tests/test_cmake_import.sh

clean:
	rm -rf $(BUILD)
