#include "fauxgpt.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t load16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t load64(const uint8_t *p)
{
    return (uint64_t)load32(p) | ((uint64_t)load32(p + 4) << 32);
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < length; ++i) {
        unsigned bit;
        crc ^= p[i];
        for (bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return crc;
}

static uint32_t crc32(const void *data, size_t length)
{
    return ~crc32_update(UINT32_MAX, data, length);
}

typedef struct write_log {
    uint64_t lba[67];
    uint8_t blocks[67][FAUXGPT_BLOCK_SIZE];
    size_t writes;
    size_t flushes;
} write_log;

static int log_write(void *context, uint64_t first_block,
                     size_t block_count, const void *data)
{
    write_log *log = (write_log *)context;

    assert(block_count == 1u);
    assert(log->writes < 67u);
    log->lba[log->writes] = first_block;
    memcpy(log->blocks[log->writes], data, FAUXGPT_BLOCK_SIZE);
    ++log->writes;
    return 0;
}

static int log_flush(void *context)
{
    write_log *log = (write_log *)context;
    ++log->flushes;
    return 0;
}

static void fill_guid(uint8_t guid[16], uint8_t seed)
{
    unsigned i;

    for (i = 0u; i < 16u; ++i)
        guid[i] = (uint8_t)(seed + i * 7u);
}

static void test_render_and_format(void)
{
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view view;
    fauxgpt_device dev;
    write_log log;
    uint8_t block[FAUXGPT_BLOCK_SIZE];
    uint8_t backup[FAUXGPT_BLOCK_SIZE];
    uint8_t array[FAUXGPT_ENTRY_ARRAY_BLOCKS * FAUXGPT_BLOCK_SIZE];
    uint8_t header_copy[FAUXGPT_BLOCK_SIZE];
    uint32_t header_crc;
    uint32_t array_crc;
    uint64_t p2_first = 34816u;
    size_t i;
    int rc;

    memset(parts, 0, sizeof(parts));
    memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[0].unique_guid, 0x20u);
    parts[0].first_lba   = 2048u;
    parts[0].block_count = 32768u;
    parts[0].name        = "FAUXFAT";

    memcpy(parts[1].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[1].unique_guid, 0x60u);
    parts[1].first_lba   = p2_first;
    parts[1].block_count = (131072u - 34u) - p2_first + 1u;
    parts[1].name        = "USER DATA";

    memset(&layout, 0, sizeof(layout));
    layout.disk_blocks = 131072u;
    fill_guid(layout.disk_guid, 0xa0u);
    layout.partitions      = parts;
    layout.partition_count = 2u;

    rc = fauxgpt_init(&view, &layout);
    assert(rc == FAUXGPT_OK);
    assert(view.backup_header_lba == 131071u);
    assert(view.backup_array_lba == 131039u);
    assert(view.last_usable_lba == 131038u);
    assert(view.partition_array_crc32 == 0xd9bcc023u);

    rc = fauxgpt_render_block(&view, 0u, block);
    assert(rc == FAUXGPT_OK);
    assert(block[446u + 4u] == 0xeeu);
    assert(load32(block + 446u + 8u) == 1u);
    assert(load32(block + 446u + 12u) == 131071u);
    assert(block[510] == 0x55u && block[511] == 0xaau);

    for (i = 0u; i < FAUXGPT_ENTRY_ARRAY_BLOCKS; ++i) {
        rc = fauxgpt_render_block(&view, FAUXGPT_PRIMARY_ARRAY_LBA + i,
                                  array + i * FAUXGPT_BLOCK_SIZE);
        assert(rc == FAUXGPT_OK);
    }

    array_crc = crc32(array, sizeof(array));
    assert(array_crc == view.partition_array_crc32);

    assert(memcmp(array, fauxgpt_type_microsoft_basic_data, 16u) == 0);
    assert(load64(array + 32u) == parts[0].first_lba);
    assert(load64(array + 40u) == parts[0].first_lba +
                                      parts[0].block_count - 1u);
    assert(load16(array + 56u) == 'F');
    assert(load16(array + 58u) == 'A');

    assert(memcmp(array + FAUXGPT_ENTRY_SIZE,
                  fauxgpt_type_microsoft_basic_data, 16u) == 0);
    assert(load64(array + FAUXGPT_ENTRY_SIZE + 32u) == parts[1].first_lba);
    assert(load64(array + FAUXGPT_ENTRY_SIZE + 40u) == view.last_usable_lba);

    rc = fauxgpt_render_block(&view, FAUXGPT_PRIMARY_HEADER_LBA, block);
    assert(rc == FAUXGPT_OK);
    assert(memcmp(block, "EFI PART", 8u) == 0);
    assert(load32(block + 8u) == 0x00010000u);
    assert(load32(block + 12u) == 92u);
    assert(load64(block + 24u) == 1u);
    assert(load64(block + 32u) == view.backup_header_lba);
    assert(load64(block + 40u) == FAUXGPT_FIRST_USABLE_LBA);
    assert(load64(block + 48u) == view.last_usable_lba);
    assert(load64(block + 72u) == FAUXGPT_PRIMARY_ARRAY_LBA);
    assert(load32(block + 80u) == FAUXGPT_ENTRY_COUNT);
    assert(load32(block + 84u) == FAUXGPT_ENTRY_SIZE);
    assert(load32(block + 88u) == array_crc);
    header_crc = load32(block + 16u);
    assert(header_crc == 0x71559159u);
    memcpy(header_copy, block, sizeof(header_copy));
    memset(header_copy + 16u, 0, 4u);
    assert(crc32(header_copy, 92u) == header_crc);

    rc = fauxgpt_render_block(&view, view.backup_header_lba, backup);
    assert(rc == FAUXGPT_OK);
    assert(load64(backup + 24u) == view.backup_header_lba);
    assert(load64(backup + 32u) == 1u);
    assert(load64(backup + 72u) == view.backup_array_lba);
    header_crc = load32(backup + 16u);
    assert(header_crc == 0xff595d9cu);
    memcpy(header_copy, backup, sizeof(header_copy));
    memset(header_copy + 16u, 0, 4u);
    assert(crc32(header_copy, 92u) == header_crc);

    rc = fauxgpt_render_block(&view, parts[0].first_lba, block);
    assert(rc == FAUXGPT_EUNMAPPED);
    rc = fauxgpt_render_block(&view, parts[1].first_lba, block);
    assert(rc == FAUXGPT_EUNMAPPED);
    rc = fauxgpt_render_block(&view, layout.disk_blocks, block);
    assert(rc == FAUXGPT_ERANGE);

    memset(&log, 0, sizeof(log));
    dev.write   = log_write;
    dev.flush   = log_flush;
    dev.context = &log;
    rc          = fauxgpt_format(&view, &dev);
    assert(rc == FAUXGPT_OK);
    assert(log.writes == 67u);
    assert(log.flushes == 2u);

    for (i = 0u; i < FAUXGPT_ENTRY_ARRAY_BLOCKS; ++i)
        assert(log.lba[i] == view.backup_array_lba + i);
    assert(log.lba[32] == view.backup_header_lba);
    for (i = 0u; i < FAUXGPT_ENTRY_ARRAY_BLOCKS; ++i)
        assert(log.lba[33u + i] == FAUXGPT_PRIMARY_ARRAY_LBA + i);
    assert(log.lba[65] == FAUXGPT_PRIMARY_HEADER_LBA);
    assert(log.lba[66] == 0u);

    /* The primary and backup entry arrays are byte-for-byte identical. */
    for (i = 0u; i < FAUXGPT_ENTRY_ARRAY_BLOCKS; ++i)
        assert(memcmp(log.blocks[i], log.blocks[33u + i],
                      FAUXGPT_BLOCK_SIZE) == 0);

    /* No partition body, including the blank user partition, was written. */
    for (i = 0u; i < log.writes; ++i) {
        assert(log.lba[i] < FAUXGPT_FIRST_USABLE_LBA ||
               log.lba[i] > view.last_usable_lba);
    }
}

static void test_reject_bad_layouts(void)
{
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view view;
    int rc;

    memset(parts, 0, sizeof(parts));
    memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[0].unique_guid, 0x10u);
    parts[0].first_lba   = FAUXGPT_FIRST_USABLE_LBA;
    parts[0].block_count = 100u;
    parts[0].name        = "ONE";

    layout.disk_blocks = 4096u;
    fill_guid(layout.disk_guid, 0xa0u);
    layout.partitions      = parts;
    layout.partition_count = 1u;

    rc = fauxgpt_init(&view, &layout);
    assert(rc == FAUXGPT_OK);

    layout.disk_blocks = 67u;
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_EGEOMETRY);
    layout.disk_blocks = 4096u;

    parts[0].first_lba = 33u;
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_EGEOMETRY);
    parts[0].first_lba = FAUXGPT_FIRST_USABLE_LBA;

    parts[0].block_count = 4096u;
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_EGEOMETRY);
    parts[0].block_count = 100u;

    parts[0].name = "012345678901234567890123456789012345X";
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_EINVAL);
    parts[0].name = "ONE";

    parts[1]               = parts[0];
    parts[1].first_lba     = 200u;
    parts[1].name          = "TWO";
    layout.partition_count = 2u;
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_EINVAL);

    fill_guid(parts[1].unique_guid, 0x50u);
    parts[1].first_lba = 100u;
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_EGEOMETRY);
}

static void test_large_disk_pmbr_and_flush_requirement(void)
{
    fauxgpt_layout layout;
    fauxgpt_view view;
    fauxgpt_device dev;
    write_log log;
    uint8_t block[FAUXGPT_BLOCK_SIZE];

    memset(&layout, 0, sizeof(layout));
    layout.disk_blocks = (uint64_t)UINT32_MAX + 4096u;
    fill_guid(layout.disk_guid, 0xb0u);

    assert(fauxgpt_init(&view, &layout) == FAUXGPT_OK);
    assert(fauxgpt_render_block(&view, 0u, block) == FAUXGPT_OK);
    assert(load32(block + 446u + 12u) == UINT32_MAX);

    memset(&log, 0, sizeof(log));
    dev.write   = log_write;
    dev.flush   = NULL;
    dev.context = &log;
    assert(fauxgpt_format(&view, &dev) == FAUXGPT_EINVAL);
    assert(log.writes == 0u);
}

int main(void)
{
    test_render_and_format();
    test_reject_bad_layouts();
    test_large_disk_pmbr_and_flush_requirement();
    puts("fauxgpt tests: ok");
    return 0;
}
