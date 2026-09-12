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

typedef struct render_source {
    const fauxgpt_view *view;
    const fauxgpt_view *backup_view;
    uint64_t corrupt_lba;
    int corrupt_pmbr;
} render_source;

static int render_read(void *context, uint64_t first_block,
                       size_t block_count, void *data)
{
    render_source *src = (render_source *)context;
    uint8_t *out       = (uint8_t *)data;
    size_t i;

    for (i = 0u; i < block_count; ++i) {
        uint64_t lba             = first_block + i;
        const fauxgpt_view *view = src->view;
        int rc;

        if (src->backup_view &&
            lba >= src->view->backup_array_lba)
            view = src->backup_view;
        rc = fauxgpt_render_block(view, lba,
                                  out + i * FAUXGPT_BLOCK_SIZE);
        if (rc == FAUXGPT_EUNMAPPED) {
            memset(out + i * FAUXGPT_BLOCK_SIZE, 0, FAUXGPT_BLOCK_SIZE);
        } else if (rc != FAUXGPT_OK) {
            return rc;
        }
        if (lba == src->corrupt_lba)
            out[i * FAUXGPT_BLOCK_SIZE + 17u] ^= 0x80u;
        if (lba == 0u && src->corrupt_pmbr)
            out[i * FAUXGPT_BLOCK_SIZE + 510u] = 0u;
    }
    return 0;
}

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
    memset(&dev, 0, sizeof(dev));
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

static void test_open_and_partition_match(void)
{
    fauxgpt_partition parts[3];
    fauxgpt_partition expected_parts[2];
    fauxgpt_layout layout;
    fauxgpt_layout expected;
    fauxgpt_partition disagree_parts[2];
    fauxgpt_layout disagree_layout;
    fauxgpt_view disagree_view;
    fauxgpt_view view;
    fauxgpt_device dev;
    fauxgpt_info info;
    fauxgpt_probe_info probe;
    render_source src;

    memset(parts, 0, sizeof(parts));
    memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[0].unique_guid, 0x10u);
    parts[0].first_lba   = 2048u;
    parts[0].block_count = 4096u;
    parts[0].name        = "FAUXFAT";
    memcpy(parts[1].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[1].unique_guid, 0x40u);
    parts[1].first_lba   = 8192u;
    parts[1].block_count = 8192u;
    parts[1].name        = "USER";

    memset(&layout, 0, sizeof(layout));
    layout.disk_blocks = 32768u;
    fill_guid(layout.disk_guid, 0x90u);
    layout.partitions      = parts;
    layout.partition_count = 2u;
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_OK);

    memset(&src, 0, sizeof(src));
    src.view        = &view;
    src.corrupt_lba = UINT64_MAX;
    memset(&dev, 0, sizeof(dev));
    dev.read    = render_read;
    dev.context = &src;

    assert(fauxgpt_probe(&probe, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert((probe.flags & FAUXGPT_PROBE_PMBR_MARKER) != 0u);
    assert((probe.flags & FAUXGPT_PROBE_PMBR_VALID) != 0u);
    assert((probe.primary.flags & FAUXGPT_COPY_MARKER) != 0u);
    assert((probe.primary.flags & FAUXGPT_COPY_VALID) != 0u);
    assert((probe.backup.flags & FAUXGPT_COPY_MARKER) != 0u);
    assert((probe.backup.flags & FAUXGPT_COPY_VALID) != 0u);
    assert(probe.primary.info.partition_count == 2u);
    assert(probe.primary.partition_array_crc32 == view.partition_array_crc32);

    assert(fauxgpt_open(&info, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert(info.flags == (FAUXGPT_INFO_PRIMARY_VALID |
                          FAUXGPT_INFO_BACKUP_VALID |
                          FAUXGPT_INFO_PMBR_VALID));
    assert(info.partition_count == 2u);
    assert(info.partitions[0].first_lba == parts[0].first_lba);
    assert(info.partitions[1].block_count == parts[1].block_count);
    assert(fauxgpt_partitioning_matches(&info, &layout));

    /* GUID/name identity changes do not make the partition geometry unsafe. */
    memcpy(expected_parts, parts, sizeof(expected_parts));
    expected            = layout;
    expected.partitions = expected_parts;
    fill_guid(expected.disk_guid, 0xe0u);
    fill_guid(expected_parts[0].unique_guid, 0xe8u);
    expected_parts[0].name = "RENAMED";
    assert(fauxgpt_partitioning_matches(&info, &expected));
    expected_parts[0].block_count += 1u;
    assert(!fauxgpt_partitioning_matches(&info, &expected));
    expected_parts[0].block_count -= 1u;

    /* One intact copy is enough to open; the missing copy is visible. */
    src.corrupt_lba = FAUXGPT_PRIMARY_HEADER_LBA;
    assert(fauxgpt_probe(&probe, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert((probe.primary.flags & FAUXGPT_COPY_MARKER) != 0u);
    assert((probe.primary.flags & FAUXGPT_COPY_VALID) == 0u);
    assert((probe.backup.flags & FAUXGPT_COPY_VALID) != 0u);
    assert(fauxgpt_open(&info, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert((info.flags & FAUXGPT_INFO_PRIMARY_VALID) == 0u);
    assert((info.flags & FAUXGPT_INFO_BACKUP_VALID) != 0u);
    src.corrupt_lba = UINT64_MAX;

    /* A bad PMBR is recoverable but is reported instead of silently blessed. */
    src.corrupt_pmbr = 1;
    assert(fauxgpt_probe(&probe, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert((probe.flags & FAUXGPT_PROBE_PMBR_MARKER) != 0u);
    assert((probe.flags & FAUXGPT_PROBE_PMBR_VALID) == 0u);
    assert(fauxgpt_open(&info, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert((info.flags & FAUXGPT_INFO_PMBR_VALID) == 0u);
    src.corrupt_pmbr = 0;

    /* Individually valid primary/backup copies are still rejected if they
     * describe different partition maps. */
    memcpy(disagree_parts, parts, sizeof(disagree_parts));
    disagree_parts[1].first_lba += 1u;
    disagree_parts[1].block_count -= 1u;
    disagree_layout            = layout;
    disagree_layout.partitions = disagree_parts;
    assert(fauxgpt_init(&disagree_view, &disagree_layout) == FAUXGPT_OK);
    src.backup_view = &disagree_view;
    assert(fauxgpt_probe(&probe, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert((probe.primary.flags & FAUXGPT_COPY_VALID) != 0u);
    assert((probe.backup.flags & FAUXGPT_COPY_VALID) != 0u);
    assert(probe.primary.partition_array_crc32 !=
           probe.backup.partition_array_crc32);
    assert(fauxgpt_open(&info, &dev, layout.disk_blocks) ==
           FAUXGPT_ESTRUCTURE);
    src.backup_view = NULL;

    /* A valid GPT with a third active entry is outside our bounded profile. */
    memcpy(parts[2].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[2].unique_guid, 0x70u);
    parts[2].first_lba     = 20000u;
    parts[2].block_count   = 1024u;
    parts[2].name          = "THIRD";
    layout.partition_count = 3u;
    assert(fauxgpt_init(&view, &layout) == FAUXGPT_OK);
    src.view = &view;
    assert(fauxgpt_probe(&probe, &dev, layout.disk_blocks) == FAUXGPT_OK);
    assert((probe.primary.flags & FAUXGPT_COPY_TOO_MANY) != 0u);
    assert(probe.primary.info.partition_count == 3u);
    assert(fauxgpt_open(&info, &dev, layout.disk_blocks) ==
           FAUXGPT_EPARTITIONS);
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
    memset(&dev, 0, sizeof(dev));
    dev.write   = log_write;
    dev.flush   = NULL;
    dev.context = &log;
    assert(fauxgpt_format(&view, &dev) == FAUXGPT_EINVAL);
    assert(log.writes == 0u);
}

int main(void)
{
    test_render_and_format();
    test_open_and_partition_match();
    test_reject_bad_layouts();
    test_large_disk_pmbr_and_flush_requirement();
    puts("fauxgpt tests: ok");
    return 0;
}
