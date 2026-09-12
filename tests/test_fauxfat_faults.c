#include "fauxfat_block.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum fault_op_kind {
    FAULT_OP_READ = 1,
    FAULT_OP_WRITE,
    FAULT_OP_ZERO,
    FAULT_OP_SKIP,
    FAULT_OP_FLUSH
} fault_op_kind;

typedef enum fault_mode {
    FAULT_NONE = 0,
    FAULT_BEFORE,
    FAULT_AFTER,
    FAULT_TORN_WRITE
} fault_mode;

typedef struct fault_log_entry {
    uint64_t index;
    fault_op_kind kind;
    uint64_t first_block;
    uint64_t block_count;
} fault_log_entry;

#define FAULT_LOG_MAX       8192u
#define FAULT_FORBIDDEN_MAX 2u

typedef struct fault_range {
    uint64_t first_block;
    uint64_t block_count;
} fault_range;

typedef struct faultdev {
    uint8_t *data;
    uint64_t blocks;
    uint64_t op_index;
    uint64_t fail_at;
    fault_mode fail_mode;
    int fail_code;
    uint64_t mutation_calls;
    uint64_t forbidden_mutations;
    fault_range forbidden[FAULT_FORBIDDEN_MAX];
    size_t forbidden_count;
    fault_log_entry log[FAULT_LOG_MAX];
    size_t log_count;
} faultdev;

static int range_ok(uint64_t limit, uint64_t first, uint64_t count)
{
    return first <= limit && count <= limit - first;
}

static int ranges_overlap(uint64_t a_first, uint64_t a_count,
                          uint64_t b_first, uint64_t b_count)
{
    uint64_t a_last;
    uint64_t b_last;

    if (a_count == 0u || b_count == 0u)
        return 0;
    a_last = a_first + a_count - 1u;
    b_last = b_first + b_count - 1u;
    return a_first <= b_last && b_first <= a_last;
}

static uint64_t fault_begin(faultdev *d, fault_op_kind kind,
                            uint64_t first_block, uint64_t block_count)
{
    uint64_t index = ++d->op_index;

    assert(d->log_count < FAULT_LOG_MAX);
    d->log[d->log_count].index       = index;
    d->log[d->log_count].kind        = kind;
    d->log[d->log_count].first_block = first_block;
    d->log[d->log_count].block_count = block_count;
    ++d->log_count;
    return index;
}

static void note_mutation(faultdev *d, uint64_t first_block,
                          uint64_t block_count)
{
    size_t i;

    ++d->mutation_calls;
    for (i = 0u; i < d->forbidden_count; ++i) {
        if (ranges_overlap(first_block, block_count,
                           d->forbidden[i].first_block,
                           d->forbidden[i].block_count))
            ++d->forbidden_mutations;
    }
}

static int fault_before(const faultdev *d, uint64_t index)
{
    return d->fail_at == index && d->fail_mode == FAULT_BEFORE;
}

static int fault_after(const faultdev *d, uint64_t index)
{
    return d->fail_at == index && d->fail_mode == FAULT_AFTER;
}

static int fault_read(void *context, uint64_t first_block,
                      size_t block_count, void *data)
{
    faultdev *d    = (faultdev *)context;
    uint64_t count = (uint64_t)block_count;
    uint64_t index = fault_begin(d, FAULT_OP_READ, first_block, count);

    if (fault_before(d, index))
        return d->fail_code;
    if (!range_ok(d->blocks, first_block, count))
        return -701;
    memcpy(data, d->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE),
           block_count * FAUXFAT_BLOCK_SIZE);
    if (fault_after(d, index))
        return d->fail_code;
    return 0;
}

static int fault_write(void *context, uint64_t first_block,
                       size_t block_count, const void *data)
{
    faultdev *d    = (faultdev *)context;
    uint64_t count = (uint64_t)block_count;
    uint64_t index = fault_begin(d, FAULT_OP_WRITE, first_block, count);
    size_t bytes;

    if (fault_before(d, index))
        return d->fail_code;
    if (!range_ok(d->blocks, first_block, count))
        return -702;
    bytes = block_count * FAUXFAT_BLOCK_SIZE;
    note_mutation(d, first_block, count);
    if (d->fail_at == index && d->fail_mode == FAULT_TORN_WRITE) {
        size_t torn = bytes / 2u;
        assert(torn != 0u);
        memcpy(d->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE), data,
               torn);
        return d->fail_code;
    }
    memcpy(d->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE), data, bytes);
    if (fault_after(d, index))
        return d->fail_code;
    return 0;
}

static int fault_zero(void *context, uint64_t first_block,
                      uint64_t block_count)
{
    faultdev *d    = (faultdev *)context;
    uint64_t index = fault_begin(d, FAULT_OP_ZERO, first_block, block_count);

    if (fault_before(d, index))
        return d->fail_code;
    if (!range_ok(d->blocks, first_block, block_count))
        return -703;
    note_mutation(d, first_block, block_count);
    memset(d->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE), 0,
           (size_t)(block_count * FAUXFAT_BLOCK_SIZE));
    if (fault_after(d, index))
        return d->fail_code;
    return 0;
}

static int fault_skip(void *context, uint64_t first_block,
                      uint64_t block_count, fauxfat_skip_kind kind)
{
    faultdev *d    = (faultdev *)context;
    uint64_t index = fault_begin(d, FAULT_OP_SKIP, first_block, block_count);
    (void)kind;

    if (fault_before(d, index))
        return d->fail_code;
    if (!range_ok(d->blocks, first_block, block_count))
        return -704;
    if (fault_after(d, index))
        return d->fail_code;
    return 0;
}

static int fault_flush(void *context)
{
    faultdev *d    = (faultdev *)context;
    uint64_t index = fault_begin(d, FAULT_OP_FLUSH, 0u, 0u);

    if (fault_before(d, index) || fault_after(d, index))
        return d->fail_code;
    return 0;
}

static void fault_reset(faultdev *d, uint64_t fail_at, fault_mode mode)
{
    d->op_index            = 0u;
    d->fail_at             = fail_at;
    d->fail_mode           = mode;
    d->mutation_calls      = 0u;
    d->forbidden_mutations = 0u;
    d->log_count           = 0u;
}

static void fill_guid(uint8_t guid[16], uint8_t seed)
{
    unsigned i;

    for (i = 0u; i < 16u; ++i)
        guid[i] = (uint8_t)(seed + i * 13u);
}

static void fill_pattern(uint8_t *data, uint64_t first_block,
                         uint64_t block_count, uint32_t seed)
{
    uint64_t i;
    uint64_t bytes = block_count * FAUXFAT_BLOCK_SIZE;
    uint8_t *p     = data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE);
    uint32_t x     = seed;

    for (i = 0u; i < bytes; ++i) {
        x    = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
}

static int region_equal(const uint8_t *a, const uint8_t *b,
                        uint64_t first_block, uint64_t block_count)
{
    return memcmp(a + (size_t)(first_block * FAUXFAT_BLOCK_SIZE),
                  b + (size_t)(first_block * FAUXFAT_BLOCK_SIZE),
                  (size_t)(block_count * FAUXFAT_BLOCK_SIZE)) == 0;
}

static void init_small_fauxfat(fauxfat_config *cfg, fauxfat_view *view,
                               uint64_t partition_lba)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->data_cluster_count = 2u;
    cfg->partition_lba      = partition_lba;
    cfg->volume_serial      = 0x6a4f3012u;
    cfg->structural_epoch   = 11u;
    fill_guid(cfg->volume_guid, 0x21u);
    cfg->volume_label = "FAULTTEST";
    assert(fauxfat_init(view, cfg) == FAUXFAT_OK);
}

static void init_small_gpt(fauxgpt_partition parts[2], fauxgpt_layout *layout,
                           fauxgpt_view *gpt, uint64_t disk_blocks,
                           uint64_t p1_first, uint64_t p1_blocks)
{
    uint64_t p2_first = p1_first + p1_blocks;

    memset(parts, 0, 2u * sizeof(*parts));
    memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[0].unique_guid, 0x41u);
    parts[0].first_lba   = p1_first;
    parts[0].block_count = p1_blocks;
    parts[0].name        = "FAUXFAT";

    memcpy(parts[1].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[1].unique_guid, 0x61u);
    parts[1].first_lba   = p2_first;
    parts[1].block_count = (disk_blocks - 34u) - p2_first + 1u;
    parts[1].name        = "USER";

    memset(layout, 0, sizeof(*layout));
    layout->disk_blocks = disk_blocks;
    fill_guid(layout->disk_guid, 0x81u);
    layout->partitions      = parts;
    layout->partition_count = 2u;
    assert(fauxgpt_init(gpt, layout) == FAUXGPT_OK);
}

static void init_block_device(fauxfat_block_device *device, faultdev *media)
{
    memset(device, 0, sizeof(*device));
    device->block_count = media->blocks;
    device->io.read     = fault_read;
    device->io.write    = fault_write;
    device->io.zero     = fault_zero;
    device->io.skip     = fault_skip;
    device->io.context  = media;
    device->flush       = fault_flush;
}

static void assert_expected_copy_survives(faultdev *media,
                                          const fauxgpt_layout *expected)
{
    fauxgpt_device device;
    fauxgpt_probe_info probe;
    int primary_valid;
    int backup_valid;

    fault_reset(media, 0u, FAULT_NONE);
    memset(&device, 0, sizeof(device));
    device.read    = fault_read;
    device.context = media;
    memset(&probe, 0, sizeof(probe));
    assert(fauxgpt_probe(&probe, &device, media->blocks) == FAUXGPT_OK);
    primary_valid = (probe.primary.flags & FAUXGPT_COPY_VALID) != 0u;
    backup_valid  = (probe.backup.flags & FAUXGPT_COPY_VALID) != 0u;
    assert(primary_valid || backup_valid);
    if (primary_valid) {
        assert(fauxgpt_geometry_matches(&probe.primary.info, expected));
        assert(fauxgpt_identity_matches(&probe.primary.info, expected));
    }
    if (backup_valid) {
        assert(fauxgpt_geometry_matches(&probe.backup.info, expected));
        assert(fauxgpt_identity_matches(&probe.backup.info, expected));
    }
}

static void test_format_failpoints_and_preservation(void)
{
    fauxfat_config cfg;
    fauxfat_view view;
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view gpt;
    fauxfat_block_device device;
    faultdev media;
    uint8_t *baseline;
    uint64_t p1_first = 128u;
    uint64_t reserve_first;
    uint64_t reserve_blocks;
    uint64_t disk_blocks;
    uint64_t success_ops;
    uint64_t i;
    size_t bytes;
    fault_mode modes[2] = { FAULT_BEFORE, FAULT_AFTER };
    size_t mi;

    init_small_fauxfat(&cfg, &view, p1_first);
    disk_blocks = p1_first + view.volume_blocks + 512u + 34u;
    init_small_gpt(parts, &layout, &gpt, disk_blocks, p1_first,
                   view.volume_blocks);

    memset(&media, 0, sizeof(media));
    media.blocks    = disk_blocks;
    media.fail_code = -777;
    bytes           = (size_t)(disk_blocks * FAUXFAT_BLOCK_SIZE);
    media.data      = (uint8_t *)calloc(1u, bytes);
    baseline        = (uint8_t *)malloc(bytes);
    assert(media.data != NULL && baseline != NULL);
    init_block_device(&device, &media);

    reserve_first = p1_first + view.cluster_heap_block +
                    (uint64_t)(view.data_first_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    reserve_blocks = (uint64_t)cfg.data_cluster_count *
                     FAUXFAT_BLOCKS_PER_CLUSTER;
    fill_pattern(media.data, reserve_first, reserve_blocks, 0x12345678u);
    fill_pattern(media.data, parts[1].first_lba, parts[1].block_count,
                 0x87654321u);

    media.forbidden_count          = 2u;
    media.forbidden[0].first_block = reserve_first;
    media.forbidden[0].block_count = reserve_blocks;
    media.forbidden[1].first_block = parts[1].first_lba;
    media.forbidden[1].block_count = parts[1].block_count;

    fault_reset(&media, 0u, FAULT_NONE);
    assert(fauxfat_block_format(&view, &device, FAUXFAT_BLOCK_WRAPPER_GPT,
                                &gpt, NULL, NULL,
                                FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);
    assert(media.forbidden_mutations == 0u);
    memcpy(baseline, media.data, bytes);

    /* A safe-format ownership failure is observational only. */
    {
        fauxfat_config foreign_cfg = cfg;
        fauxfat_view foreign_view;
        uint64_t mutations_before;

        foreign_cfg.volume_serial ^= 0x01020304u;
        assert(fauxfat_init(&foreign_view, &foreign_cfg) == FAUXFAT_OK);
        fault_reset(&media, 0u, FAULT_NONE);
        mutations_before = media.mutation_calls;
        assert(fauxfat_block_format(&foreign_view, &device,
                                    FAUXFAT_BLOCK_WRAPPER_GPT, &gpt,
                                    NULL, NULL, 0u) ==
               FAUXFAT_BLOCK_EIDENTITY);
        assert(media.mutation_calls == mutations_before);
        assert(memcmp(media.data, baseline, bytes) == 0);
    }

    fault_reset(&media, 0u, FAULT_NONE);
    assert(fauxfat_block_format(&view, &device, FAUXFAT_BLOCK_WRAPPER_GPT,
                                &gpt, NULL, NULL, 0u) == FAUXFAT_BLOCK_OK);
    success_ops = media.op_index;
    assert(success_ops != 0u);
    assert(media.forbidden_mutations == 0u);
    assert(region_equal(media.data, baseline, reserve_first, reserve_blocks));
    assert(region_equal(media.data, baseline, parts[1].first_lba,
                        parts[1].block_count));

    for (mi = 0u; mi < 2u; ++mi) {
        for (i = 1u; i <= success_ops; ++i) {
            int rc;

            memcpy(media.data, baseline, bytes);
            fault_reset(&media, i, modes[mi]);
            rc = fauxfat_block_format(&view, &device,
                                      FAUXFAT_BLOCK_WRAPPER_GPT, &gpt,
                                      NULL, NULL, 0u);
            assert(rc == FAUXFAT_BLOCK_EIO);
            assert(media.forbidden_mutations == 0u);
            assert(region_equal(media.data, baseline, reserve_first,
                                reserve_blocks));
            assert(region_equal(media.data, baseline, parts[1].first_lba,
                                parts[1].block_count));
            if (media.mutation_calls == 0u)
                assert(memcmp(media.data, baseline, bytes) == 0);
        }
    }

    free(baseline);
    free(media.data);
}

static void test_repair_failpoints_keep_one_copy(void)
{
    fauxfat_config cfg;
    fauxfat_view view;
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view gpt;
    fauxfat_block_device device;
    faultdev media;
    uint8_t *valid;
    uint8_t *damaged;
    uint64_t p1_first = 128u;
    uint64_t disk_blocks;
    uint64_t success_ops;
    uint64_t i;
    size_t bytes;
    size_t write_count = 0u;
    uint64_t write_indices[128];
    fault_mode modes[2] = { FAULT_BEFORE, FAULT_AFTER };
    size_t mi;
    size_t li;

    init_small_fauxfat(&cfg, &view, p1_first);
    disk_blocks = p1_first + view.volume_blocks + 512u + 34u;
    init_small_gpt(parts, &layout, &gpt, disk_blocks, p1_first,
                   view.volume_blocks);

    memset(&media, 0, sizeof(media));
    media.blocks    = disk_blocks;
    media.fail_code = -778;
    bytes           = (size_t)(disk_blocks * FAUXFAT_BLOCK_SIZE);
    media.data      = (uint8_t *)calloc(1u, bytes);
    valid           = (uint8_t *)malloc(bytes);
    damaged         = (uint8_t *)malloc(bytes);
    assert(media.data != NULL && valid != NULL && damaged != NULL);
    init_block_device(&device, &media);

    fill_pattern(media.data, parts[1].first_lba, parts[1].block_count,
                 0x10293847u);
    fault_reset(&media, 0u, FAULT_NONE);
    assert(fauxfat_block_format(&view, &device, FAUXFAT_BLOCK_WRAPPER_GPT,
                                &gpt, NULL, NULL,
                                FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);
    memcpy(valid, media.data, bytes);

    /* Start from a state where the primary copy remains authoritative while
     * the backup header and protective MBR need repair. */
    memcpy(damaged, valid, bytes);
    damaged[(size_t)((disk_blocks - 1u) * FAUXFAT_BLOCK_SIZE + 8u)] ^= 0x5au;
    damaged[510u] = 0u;
    memcpy(media.data, damaged, bytes);

    media.forbidden_count          = 2u;
    media.forbidden[0].first_block = parts[0].first_lba;
    media.forbidden[0].block_count = parts[0].block_count;
    media.forbidden[1].first_block = parts[1].first_lba;
    media.forbidden[1].block_count = parts[1].block_count;

    fault_reset(&media, 0u, FAULT_NONE);
    assert(fauxfat_block_repair_gpt(&view, &device, &gpt) ==
           FAUXFAT_BLOCK_OK);
    success_ops = media.op_index;
    assert(media.forbidden_mutations == 0u);
    assert(region_equal(media.data, damaged, parts[0].first_lba,
                        parts[0].block_count));
    assert(region_equal(media.data, damaged, parts[1].first_lba,
                        parts[1].block_count));
    for (li = 0u; li < media.log_count; ++li) {
        if (media.log[li].kind == FAULT_OP_WRITE) {
            assert(write_count < sizeof(write_indices) / sizeof(write_indices[0]));
            write_indices[write_count++] = media.log[li].index;
        }
    }
    assert(write_count == (size_t)(2u * FAUXGPT_ENTRY_ARRAY_BLOCKS + 3u));

    for (mi = 0u; mi < 2u; ++mi) {
        for (i = 1u; i <= success_ops; ++i) {
            int rc;

            memcpy(media.data, damaged, bytes);
            fault_reset(&media, i, modes[mi]);
            rc = fauxfat_block_repair_gpt(&view, &device, &gpt);
            assert(rc == FAUXFAT_BLOCK_EIO);
            assert(media.forbidden_mutations == 0u);
            assert(region_equal(media.data, damaged, parts[0].first_lba,
                                parts[0].block_count));
            assert(region_equal(media.data, damaged, parts[1].first_lba,
                                parts[1].block_count));
            assert_expected_copy_survives(&media, &layout);
        }
    }

    /* Sector tearing is nastier than a clean callback failure. Every GPT write
     * may be half-written before power disappears; backup-first ordering must
     * still leave one complete expected GPT copy. */
    for (li = 0u; li < write_count; ++li) {
        int rc;

        memcpy(media.data, damaged, bytes);
        fault_reset(&media, write_indices[li], FAULT_TORN_WRITE);
        rc = fauxfat_block_repair_gpt(&view, &device, &gpt);
        assert(rc == FAUXFAT_BLOCK_EIO);
        assert(media.forbidden_mutations == 0u);
        assert(region_equal(media.data, damaged, parts[0].first_lba,
                            parts[0].block_count));
        assert(region_equal(media.data, damaged, parts[1].first_lba,
                            parts[1].block_count));
        assert_expected_copy_survives(&media, &layout);
    }

    free(damaged);
    free(valid);
    free(media.data);
}

static uint32_t rng_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void random_guid(uint8_t guid[16], uint32_t *state)
{
    unsigned i;

    for (i = 0u; i < 16u; ++i)
        guid[i] = (uint8_t)(rng_next(state) >> 24);
    guid[0] |= 1u;
}

static void test_random_gpt_roundtrips(void)
{
    uint32_t state   = 0xc0ffee11u;
    uint8_t *storage = (uint8_t *)malloc(4096u * FAUXGPT_BLOCK_SIZE);
    unsigned iteration;

    assert(storage != NULL);
    for (iteration = 0u; iteration < 64u; ++iteration) {
        fauxgpt_partition parts[2];
        fauxgpt_layout layout;
        fauxgpt_view view;
        fauxgpt_info info;
        fauxgpt_device gd;
        faultdev media;
        uint64_t disk_blocks = 512u + (rng_next(&state) % (4096u - 512u));
        uint64_t last_usable = disk_blocks - 34u;
        uint64_t usable      = last_usable - FAUXGPT_FIRST_USABLE_LBA + 1u;
        uint64_t p1_first;
        uint64_t p1_blocks;
        uint64_t p2_first;
        uint64_t left;
        unsigned count = 1u + (rng_next(&state) & 1u);

        memset(storage, 0xa5, 4096u * FAUXGPT_BLOCK_SIZE);
        memset(parts, 0, sizeof(parts));
        p1_first = FAUXGPT_FIRST_USABLE_LBA +
                   (rng_next(&state) % (usable / 4u));
        left      = last_usable - p1_first + 1u;
        p1_blocks = 1u + (rng_next(&state) % (left / (count == 2u ? 2u : 1u)));
        memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
        random_guid(parts[0].unique_guid, &state);
        parts[0].first_lba   = p1_first;
        parts[0].block_count = p1_blocks;
        parts[0].attributes  = (uint64_t)(rng_next(&state) & 0xffu);
        parts[0].name        = "P1";

        if (count == 2u) {
            p2_first = p1_first + p1_blocks;
            assert(p2_first <= last_usable);
            left = last_usable - p2_first + 1u;
            memcpy(parts[1].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
            random_guid(parts[1].unique_guid, &state);
            parts[1].first_lba   = p2_first;
            parts[1].block_count = 1u + (rng_next(&state) % left);
            parts[1].attributes  = (uint64_t)(rng_next(&state) & 0xffu);
            parts[1].name        = "P2";
        }

        memset(&layout, 0, sizeof(layout));
        layout.disk_blocks = disk_blocks;
        random_guid(layout.disk_guid, &state);
        layout.partitions      = parts;
        layout.partition_count = count;
        assert(fauxgpt_init(&view, &layout) == FAUXGPT_OK);

        memset(&media, 0, sizeof(media));
        media.data      = storage;
        media.blocks    = disk_blocks;
        media.fail_code = -779;
        fault_reset(&media, 0u, FAULT_NONE);
        memset(&gd, 0, sizeof(gd));
        gd.read    = fault_read;
        gd.write   = fault_write;
        gd.flush   = fault_flush;
        gd.context = &media;
        assert(fauxgpt_format(&view, &gd) == FAUXGPT_OK);
        assert(fauxgpt_verify(&view, &gd) == FAUXGPT_OK);
        memset(&info, 0, sizeof(info));
        assert(fauxgpt_open(&info, &gd, disk_blocks) == FAUXGPT_OK);
        assert(fauxgpt_geometry_matches(&info, &layout));
        assert(fauxgpt_identity_matches(&info, &layout));
    }
    free(storage);
}

int main(void)
{
    test_format_failpoints_and_preservation();
    test_repair_failpoints_keep_one_copy();
    test_random_gpt_roundtrips();
    puts("fauxfat fault tests: ok");
    return 0;
}
