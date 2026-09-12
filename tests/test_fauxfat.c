#include "fauxfat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t load16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
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

static uint32_t ror32(uint32_t v)
{
    return (v >> 1) | (v << 31);
}

static void store16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t ror16(uint16_t v)
{
    return (uint16_t)((v >> 1) | (v << 15));
}

static uint16_t entry_set_checksum(const uint8_t *p, size_t bytes)
{
    uint16_t sum = 0u;
    size_t i;

    for (i = 0; i < bytes; ++i) {
        if (i == 2u || i == 3u)
            continue;
        sum = (uint16_t)(ror16(sum) + p[i]);
    }
    return sum;
}

static uint32_t boot_checksum(const fauxfat_view *v)
{
    uint8_t b[512];
    uint32_t sum = 0;
    uint32_t sector;
    unsigned i;

    for (sector = 0; sector <= 10; ++sector) {
        assert(fauxfat_read_block(v, sector, b) == FAUXFAT_OK);
        for (i = 0; i < 512; ++i) {
            if (sector == 0 && (i == 106u || i == 107u || i == 112u))
                continue;
            sum = ror32(sum) + b[i];
        }
    }
    return sum;
}

static uint32_t upcase_checksum(const fauxfat_view *v)
{
    uint8_t b[512];
    uint32_t sum          = 0;
    uint64_t upcase_block = v->cluster_heap_block +
                            (uint64_t)(v->upcase_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    size_t remaining = 128u;
    uint32_t block   = 0;

    while (remaining) {
        size_t n = remaining > 512u ? 512u : remaining;
        size_t i;
        assert(fauxfat_read_block(v, upcase_block + block, b) == FAUXFAT_OK);
        for (i = 0; i < n; ++i)
            sum = ror32(sum) + b[i];
        remaining -= n;
        ++block;
    }
    return sum;
}

static void fill_pattern(uint8_t *p, size_t n, uint8_t seed)
{
    size_t i;
    for (i = 0; i < n; ++i)
        p[i] = (uint8_t)(seed + (uint8_t)(i * 29u) + (uint8_t)(i >> 9));
}

typedef struct test_backing {
    int fd;
    uint8_t *data;
    size_t size;
} test_backing;

typedef struct test_io {
    test_backing backing[8];
    size_t backing_count;
    unsigned read_calls;
    unsigned write_calls;
    int last_fd;
    uint64_t last_offset;
    size_t last_length;
    int fail_read_fd;
    int fail_write_fd;
} test_io;

static test_backing *find_backing(test_io *io, int fd)
{
    size_t i;
    for (i = 0; i < io->backing_count; ++i) {
        if (io->backing[i].fd == fd)
            return &io->backing[i];
    }
    return NULL;
}

static int test_read(void *context, int fd, uint64_t offset,
                     void *data, size_t length)
{
    test_io *io     = (test_io *)context;
    test_backing *b = find_backing(io, fd);

    ++io->read_calls;
    io->last_fd     = fd;
    io->last_offset = offset;
    io->last_length = length;
    if (fd == io->fail_read_fd)
        return -101;
    if (!b || offset > b->size || length > b->size - (size_t)offset)
        return -103;
    memcpy(data, b->data + (size_t)offset, length);
    return 0;
}

static int test_write(void *context, int fd, uint64_t offset,
                      const void *data, size_t length)
{
    test_io *io     = (test_io *)context;
    test_backing *b = find_backing(io, fd);

    ++io->write_calls;
    io->last_fd     = fd;
    io->last_offset = offset;
    io->last_length = length;
    if (fd == io->fail_write_fd)
        return -102;
    if (!b || offset > b->size || length > b->size - (size_t)offset)
        return -104;
    memcpy(b->data + (size_t)offset, data, length);
    return 0;
}

typedef struct test_device {
    uint8_t *data;
    uint64_t blocks;
    unsigned write_calls;
    unsigned zero_calls;
    unsigned undefined_skips;
    unsigned preserve_skips;
    uint64_t preserve_blocks;
    uint64_t fail_read_block;
    int fail_read_code;
} test_device;

static int view_dev_read(void *context, uint64_t first_block,
                         size_t block_count, void *data)
{
    return fauxfat_read_blocks((const fauxfat_view *)context, first_block,
                               block_count, (uint8_t *)data);
}

static int dev_read(void *context, uint64_t first_block,
                    size_t block_count, void *data)
{
    test_device *d = (test_device *)context;
    uint64_t bytes = (uint64_t)block_count * FAUXFAT_BLOCK_SIZE;

    assert(first_block <= d->blocks);
    assert((uint64_t)block_count <= d->blocks - first_block);
    if (d->fail_read_code != 0 &&
        d->fail_read_block >= first_block &&
        d->fail_read_block - first_block < block_count)
        return d->fail_read_code;
    assert(bytes <= SIZE_MAX);
    memcpy(data, d->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE),
           (size_t)bytes);
    return 0;
}

static int dev_write(void *context, uint64_t first_block,
                     size_t block_count, const void *data)
{
    test_device *d = (test_device *)context;
    uint64_t bytes = (uint64_t)block_count * FAUXFAT_BLOCK_SIZE;

    assert(first_block <= d->blocks);
    assert((uint64_t)block_count <= d->blocks - first_block);
    assert(bytes <= SIZE_MAX);
    memcpy(d->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE),
           data, (size_t)bytes);
    ++d->write_calls;
    return 0;
}

static int dev_zero(void *context, uint64_t first_block,
                    uint64_t block_count)
{
    test_device *d = (test_device *)context;
    uint64_t bytes = block_count * FAUXFAT_BLOCK_SIZE;

    assert(first_block <= d->blocks);
    assert(block_count <= d->blocks - first_block);
    assert(bytes <= SIZE_MAX);
    memset(d->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE),
           0, (size_t)bytes);
    ++d->zero_calls;
    return 0;
}

static int dev_skip(void *context, uint64_t first_block,
                    uint64_t block_count, fauxfat_skip_kind kind)
{
    test_device *d = (test_device *)context;

    assert(first_block <= d->blocks);
    assert(block_count <= d->blocks - first_block);
    if (kind == FAUXFAT_SKIP_PRESERVE) {
        ++d->preserve_skips;
        d->preserve_blocks += block_count;
    } else {
        assert(kind == FAUXFAT_SKIP_UNDEFINED);
        ++d->undefined_skips;
    }
    return 0;
}

typedef struct preserve_test {
    const char *name;
    unsigned calls;
    fauxfat_disk_file last;
} preserve_test;

typedef struct emit_test {
    fauxfat_disk_file file[8];
    size_t count;
    int fail_code;
} emit_test;

typedef struct recovered_preserve_test {
    const emit_test *recovered;
    unsigned calls;
} recovered_preserve_test;

static int collect_file(void *context, unsigned index,
                        const fauxfat_disk_file *file)
{
    emit_test *e = (emit_test *)context;

    assert(index == e->count);
    assert(index < sizeof(e->file) / sizeof(e->file[0]));
    if (e->fail_code != 0)
        return e->fail_code;
    e->file[e->count++] = *file;
    return 0;
}

static int preserve_named(void *context, const fauxfat_disk_file *wanted)
{
    preserve_test *p = (preserve_test *)context;

    ++p->calls;
    p->last = *wanted;
    return strcmp(wanted->name, p->name) == 0;
}

static int preserve_recovered(void *context, const fauxfat_disk_file *wanted)
{
    recovered_preserve_test *p = (recovered_preserve_test *)context;
    size_t i;

    ++p->calls;
    for (i = 0u; i < p->recovered->count; ++i) {
        const fauxfat_disk_file *have = &p->recovered->file[i];

        if (have->kind != FAUXFAT_DISK_FILE_PUBLIC ||
            wanted->kind != FAUXFAT_DISK_FILE_PUBLIC)
            continue;
        if (strcmp(have->name, wanted->name) != 0)
            continue;
        if (have->first_block != wanted->first_block ||
            have->data_length != wanted->data_length ||
            have->allocation_blocks != wanted->allocation_blocks)
            continue;
        return 1;
    }
    return 0;
}

int main(void)
{
    static uint8_t solver[2u * FAUXFAT_CLUSTER_SIZE];
    static uint8_t config_data[FAUXFAT_CLUSTER_SIZE];
    static uint8_t opaque_data[FAUXFAT_CLUSTER_SIZE];
    static const uint8_t guid[16] = {
        0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a, 0xf0, 0xde,
        0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    };
    fauxfat_file files[] = {
        { "SOLVER.DB", 10, sizeof(solver), (time_t)1735787045 },
        { "CONFIG.BIN", 11, sizeof(config_data), (time_t)2114380798 }
    };

    test_io io;
    fauxfat_config cfg;
    fauxfat_view view;
    uint8_t b[512];
    uint64_t root_block;
    uint64_t solver_block;
    uint64_t config_block;
    unsigned i;

    fill_pattern(solver, sizeof(solver), 0x31u);
    fill_pattern(config_data, sizeof(config_data), 0xa7u);
    fill_pattern(opaque_data, sizeof(opaque_data), 0xd3u);

    memset(&io, 0, sizeof(io));
    io.backing[0].fd   = 10;
    io.backing[0].data = solver;
    io.backing[0].size = sizeof(solver);
    io.backing[1].fd   = 11;
    io.backing[1].data = config_data;
    io.backing[1].size = sizeof(config_data);
    io.backing_count   = 2u;
    io.fail_read_fd    = -1;
    io.fail_write_fd   = -1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.files            = files;
    cfg.file_count       = sizeof(files) / sizeof(files[0]);
    cfg.read             = test_read;
    cfg.write            = test_write;
    cfg.io_context       = &io;
    cfg.partition_lba    = 2048u;
    cfg.volume_serial    = 0x13579bdfu;
    cfg.structural_epoch = 0x1122334455667788ull;
    memcpy(cfg.volume_guid, guid, sizeof(guid));
    cfg.volume_label = "RP UPDATE";

    assert(fauxfat_init(&view, &cfg) == FAUXFAT_OK);

    /* One bitmap cluster, one upcase cluster, one root, three data clusters. */
    assert(view.bitmap_clusters == 1u);
    assert(view.upcase_cluster == 3u);
    assert(view.root_cluster == 4u);
    assert(view.data_first_cluster == 5u);
    assert(view.cluster_count == 6u);
    assert(view.fat_length_blocks == 128u);
    assert(view.cluster_heap_block == 256u);
    assert(view.volume_blocks == 1024u);
    /* Fixed fauxFAT-private XXH32 test vector, checked against libxxhash. */
    assert(view.map_xxh32 == 0xedb7c56au);
    assert(view.fat_xxh32 == 0xd0470b77u);
    assert(view.bitmap_xxh32 == 0x080118f4u);
    assert(view.root_xxh32 == 0xa1daab59u);
    assert(view.upcase_xxh32 == 0x70b6935bu);
    assert(fauxfat_block_count(&view) == 1024u);
    assert(fauxfat_disk_file_count(&view) == 2u);
    {
        fauxfat_disk_file d;
        assert(fauxfat_describe_disk_file(&view, 0u, &d) == FAUXFAT_OK);
        assert(d.kind == FAUXFAT_DISK_FILE_PUBLIC);
        assert(strcmp(d.name, "SOLVER.DB") == 0);
        assert(d.data_length == sizeof(solver));
        assert(d.allocation_blocks == 2u * FAUXFAT_BLOCKS_PER_CLUSTER);
    }

    assert(fauxfat_read_block(&view, 0, b) == FAUXFAT_OK);
    assert(b[0] == 0xeb && b[1] == 0x76 && b[2] == 0x90);
    assert(memcmp(b + 3, "EXFAT   ", 8) == 0);
    assert(load64(b + 64) == 2048u);
    assert(load64(b + 72) == 1024u);
    assert(load32(b + 80) == 128u);
    assert(load32(b + 84) == 128u);
    assert(load32(b + 88) == 256u);
    assert(load32(b + 92) == 6u);
    assert(load32(b + 96) == 4u);
    assert(load32(b + 100) == 0x13579bdfu);
    assert(load16(b + 104) == 0x0100u);
    assert(b[108] == 9u && b[109] == 7u && b[110] == 1u);
    assert(b[112] == 100u);
    assert(b[510] == 0x55u && b[511] == 0xaau);

    assert(fauxfat_read_block(&view, 1, b) == FAUXFAT_OK);
    for (i = 0; i < 508; ++i)
        assert(b[i] == 0u);
    assert(b[508] == 0u && b[509] == 0u && b[510] == 0x55u && b[511] == 0xaau);

    assert(fauxfat_read_block(&view, 9, b) == FAUXFAT_OK);
    assert(memcmp(b, "\xe7\xe6\x5b\x99\x45\x34\xdc\x46\xa2\x13\x74\xd9\x85\xb3\x01\x34", 16) == 0);
    assert(load32(b + 16) == view.map_xxh32);
    assert(load32(b + 20) == 0u);
    for (i = 24; i < 48; ++i)
        assert(b[i] == 0u);
    assert(memcmp(b + 64, "FFV1", 4) == 0);
    assert(load16(b + 68) == 1u);
    assert(load64(b + 72) == 0x1122334455667788ull);
    assert(load32(b + 80) == view.fat_xxh32);
    assert(load32(b + 84) == view.bitmap_xxh32);
    assert(load32(b + 88) == view.root_xxh32);
    assert(load32(b + 92) == view.upcase_xxh32);

    assert(boot_checksum(&view) == view.boot_checksum);
    assert(fauxfat_read_block(&view, 11, b) == FAUXFAT_OK);
    for (i = 0; i < 128; ++i)
        assert(load32(b + 4u * i) == view.boot_checksum);

    assert(fauxfat_read_block(&view, 128, b) == FAUXFAT_OK);
    assert(load32(b + 0) == 0xfffffff8u);
    assert(load32(b + 4) == 0xffffffffu);
    assert(load32(b + 8) == 0xffffffffu);  /* bitmap */
    assert(load32(b + 12) == 0xffffffffu); /* upcase */
    assert(load32(b + 16) == 0xffffffffu); /* root */
    assert(load32(b + 20) == 0u);          /* SOLVER.DB cluster 0 */
    assert(load32(b + 24) == 0u);          /* SOLVER.DB cluster 1 */
    assert(load32(b + 28) == 0u);          /* CONFIG.BIN */

    /* Six meaningful allocation bits, no phantom allocation above ClusterCount. */
    assert(fauxfat_read_block(&view, view.cluster_heap_block, b) == FAUXFAT_OK);
    assert(b[0] == 0x3fu);
    for (i = 1; i < 512; ++i)
        assert(b[i] == 0u);

    assert(upcase_checksum(&view) == 0xa872cee1u);
    assert(view.upcase_checksum == 0xa872cee1u);

    /* fauxFAT's custom compressed table covers all UTF-16 code units while
     * only implementing useful case folding for ASCII and ISO-8859-1. */
    {
        uint64_t upcase_block = view.cluster_heap_block +
                                (uint64_t)(view.upcase_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        static const uint16_t expected_prefix[] = {
            0xffffu, 0x0061u,
            0x0041u, 0x0042u, 0x0043u, 0x0044u, 0x0045u, 0x0046u,
            0x0047u, 0x0048u, 0x0049u, 0x004au, 0x004bu, 0x004cu,
            0x004du, 0x004eu, 0x004fu, 0x0050u, 0x0051u, 0x0052u,
            0x0053u, 0x0054u, 0x0055u, 0x0056u, 0x0057u, 0x0058u,
            0x0059u, 0x005au, 0xffffu, 0x0065u
        };
        size_t j;

        assert(fauxfat_read_block(&view, upcase_block, b) == FAUXFAT_OK);
        for (j = 0; j < sizeof(expected_prefix) / sizeof(expected_prefix[0]); ++j)
            assert(load16(b + 2u * j) == expected_prefix[j]);
        assert(load16(b + 2u * 30u) == 0x00c0u); /* à -> À */
        assert(load16(b + 2u * 53u) == 0x00f7u); /* division sign unchanged */
        assert(load16(b + 2u * 61u) == 0x00ffu); /* ÿ stays Latin-1 */
        assert(load16(b + 2u * 62u) == 0xffffu);
        assert(load16(b + 2u * 63u) == 0xff00u); /* U+0100..FFFF identity */
        for (j = 128u; j < 512u; ++j)
            assert(b[j] == 0u);
    }

    root_block = view.cluster_heap_block +
                 (uint64_t)(view.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    assert(fauxfat_read_block(&view, root_block, b) == FAUXFAT_OK);
    assert(b[0] == 0x81u);
    assert(b[32] == 0x82u);
    assert(load32(b + 32u + 4u) == 0xa872cee1u);
    assert(load64(b + 32u + 24u) == 128u);
    assert(b[64] == 0x83u);
    assert(b[96] == 0xa0u);

    /* First file set starts at root entry 4, i.e. byte 128. */
    assert(b[128] == 0x85u);
    assert(b[129] == 4u);
    assert(b[160] == 0xc0u);
    assert(b[192] == 0xc1u);
    assert(b[224] == 0xe0u);
    assert(b[256] == 0xe0u);
    {
        static const uint8_t public_name_guid0[16] = {
            0x65, 0xc7, 0x13, 0xae, 0x8b, 0x27, 0x2f, 0x4b,
            0x9d, 0x4e, 0x74, 0x26, 0xb0, 0xc6, 0x18, 0x91
        };
        static const uint8_t public_name_guid1[16] = {
            0x0e, 0x4a, 0xb1, 0x58, 0x9c, 0x89, 0x34, 0x47,
            0xb3, 0xe1, 0x5a, 0x71, 0x7d, 0x0a, 0x2f, 0xcc
        };

        assert(memcmp(b + 226u, public_name_guid0, 16u) == 0);
        assert(memcmp(b + 258u, public_name_guid1, 16u) == 0);
    }
    assert(load32(b + 128u + 8u) == 0x5a221882u);
    assert(load32(b + 128u + 12u) == 0x5a221882u);
    assert(load32(b + 128u + 16u) == 0x5a221882u);
    assert(b[128u + 20u] == 100u);
    assert(b[128u + 21u] == 100u);
    assert(b[128u + 22u] == 0x80u);
    assert(b[128u + 23u] == 0x80u);
    assert(b[128u + 24u] == 0x80u);
    assert(b[161] == 0x03u);
    assert(b[163] == 9u);
    assert(load32(b + 160 + 20) == 5u);
    assert(load64(b + 160 + 24) == sizeof(solver));

    /* Persisted original name is independent of the visible namespace name. */
    assert(b[224u + 18u] == 9u);
    assert(memcmp(b + 224u + 19u, "SOLVER.DB", 9u) == 0);
    assert(b[256u + 20u] == 9u);

    /* Second file begins at entry 9, and entry 14 is the next padded slot. */
    assert(b[288] == 0x85u);
    assert(load32(b + 320 + 20) == 7u);
    assert(b[448] == 0xa1u);
    assert(load16(b + 450) == 0x0508u);
    assert(fauxfat_read_block(&view, root_block + 127u, b) == FAUXFAT_OK);
    assert(b[480] == 0xa1u); /* final root entry: no end marker, no free slot */

    solver_block = view.cluster_heap_block +
                   (uint64_t)(5u - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    config_block = view.cluster_heap_block +
                   (uint64_t)(7u - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;

    assert(fauxfat_read_block(&view, solver_block, b) == FAUXFAT_OK);
    assert(memcmp(b, solver, sizeof(b)) == 0);
    {
        uint8_t pair[1024];
        unsigned reads_before = io.read_calls;
        assert(fauxfat_read_blocks(&view, solver_block, 2u, pair) == FAUXFAT_OK);
        assert(memcmp(pair, solver, sizeof(pair)) == 0);
        assert(io.read_calls == reads_before + 1u);
        assert(io.last_fd == 10);
        assert(io.last_offset == 0u);
        assert(io.last_length == sizeof(pair));
    }
    assert(fauxfat_read_block(&view, solver_block + 128u, b) == FAUXFAT_OK);
    assert(memcmp(b, solver + FAUXFAT_CLUSTER_SIZE, sizeof(b)) == 0);
    assert(fauxfat_read_block(&view, config_block + 17u, b) == FAUXFAT_OK);
    assert(memcmp(b, config_data + 17u * 512u, sizeof(b)) == 0);

    /* A disk write translates directly into a bounded caller-owned range. */
    {
        fauxfat_write_mapping m;
        uint8_t in[512];

        memset(in, 0x5au, sizeof(in));
        assert(fauxfat_translate_write(&view, solver_block + 17u, &m) == FAUXFAT_OK);
        assert(m.file_index == 0u);
        assert(m.fd == 10);
        assert(m.file_offset == 17u * 512u);
        assert(m.length == 512u);

        assert(fauxfat_write_block(&view, solver_block + 17u, in) == FAUXFAT_OK);
        assert(memcmp(solver + 17u * 512u, in, sizeof(in)) == 0);
        assert(fauxfat_read_block(&view, solver_block + 17u, b) == FAUXFAT_OK);
        assert(memcmp(b, in, sizeof(in)) == 0);

        assert(fauxfat_translate_write(&view, 0u, &m) == FAUXFAT_EUNMAPPED);
        assert(fauxfat_write_block(&view, root_block, in) == FAUXFAT_EUNMAPPED);
        assert(fauxfat_translate_write(&view, view.volume_blocks, &m) == FAUXFAT_ERANGE);
    }

    /* Adjacent requests may cross between adjacent file extents. */
    {
        uint8_t in[1024];
        uint64_t last_solver_block = solver_block +
                                     (sizeof(solver) / FAUXFAT_BLOCK_SIZE) - 1u;
        unsigned writes_before = io.write_calls;

        memset(in, 0x61u, 512u);
        memset(in + 512u, 0x7cu, 512u);
        assert(last_solver_block + 1u == config_block);
        assert(fauxfat_write_blocks(&view, last_solver_block, 2u, in) == FAUXFAT_OK);
        assert(memcmp(solver + sizeof(solver) - 512u, in, 512u) == 0);
        assert(memcmp(config_data, in + 512u, 512u) == 0);
        assert(io.write_calls == writes_before + 2u);
    }

    /* Multi-block accesses within one file collapse to one backend range. */
    {
        uint8_t in[1024];
        uint8_t out[1024];
        unsigned writes_before = io.write_calls;
        unsigned reads_before;

        memset(in, 0x93u, sizeof(in));
        assert(fauxfat_write_blocks(&view, solver_block + 8u, 2u, in) == FAUXFAT_OK);
        assert(io.write_calls == writes_before + 1u);
        assert(io.last_fd == 10);
        assert(io.last_offset == 8u * 512u);
        assert(io.last_length == sizeof(in));

        reads_before = io.read_calls;
        assert(fauxfat_read_blocks(&view, solver_block + 8u, 2u, out) == FAUXFAT_OK);
        assert(io.read_calls == reads_before + 1u);
        assert(memcmp(out, in, sizeof(out)) == 0);
    }

    /* Backend errors are returned unchanged. */
    io.fail_read_fd = 10;
    assert(fauxfat_read_block(&view, solver_block, b) == -101);
    io.fail_read_fd  = -1;
    io.fail_write_fd = 10;
    assert(fauxfat_write_block(&view, solver_block, b) == -102);
    io.fail_write_fd = -1;

    assert(fauxfat_read_block(&view, view.volume_blocks, b) == FAUXFAT_ERANGE);

    /* File names are ISO-8859-1 bytes rendered directly as UTF-16 code units. */
    {
        fauxfat_file latin = {
            "caf\xe9.bin", 11, sizeof(config_data), (time_t)1735787045
        };

        fauxfat_config latincfg = cfg;
        fauxfat_view latinview;
        uint64_t latin_root;

        latincfg.files      = &latin;
        latincfg.file_count = 1;
        assert(fauxfat_init(&latinview, &latincfg) == FAUXFAT_OK);
        latin_root = latinview.cluster_heap_block +
                     (uint64_t)(latinview.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        assert(fauxfat_read_block(&latinview, latin_root, b) == FAUXFAT_OK);
        assert(load16(b + 192u + 2u + 6u) == 0x00e9u);
    }
    {
        fauxfat_file collision[] = {
            { "caf\xe9.bin", 10, sizeof(solver), (time_t)1735787045 },
            { "CAF\xc9.BIN", 11, sizeof(config_data), (time_t)1735787045 }
        };

        fauxfat_config badcfg = cfg;
        badcfg.files          = collision;
        badcfg.file_count     = 2;
        assert(fauxfat_init(&view, &badcfg) == FAUXFAT_EINVAL);
    }
    {
        fauxfat_file bad = {
            "BAD/NAME", 11, sizeof(config_data), (time_t)1735787045
        };

        fauxfat_config badcfg = cfg;
        badcfg.files          = &bad;
        badcfg.file_count     = 1;
        assert(fauxfat_init(&view, &badcfg) == FAUXFAT_EINVAL);
    }
    {
        fauxfat_config badcfg = cfg;
        badcfg.read           = NULL;
        assert(fauxfat_init(&view, &badcfg) == FAUXFAT_EINVAL);
        badcfg       = cfg;
        badcfg.write = NULL;
        assert(fauxfat_init(&view, &badcfg) == FAUXFAT_EINVAL);
    }
    {
        uint8_t short_data[1234];
        fauxfat_file short_file = {
            "SHORT.BIN", 12, sizeof(short_data), (time_t)1735787045
        };

        fauxfat_config short_cfg = cfg;
        fauxfat_view short_view;
        uint64_t data_block;

        fill_pattern(short_data, sizeof(short_data), 0x42u);
        io.backing[2].fd     = 12;
        io.backing[2].data   = short_data;
        io.backing[2].size   = sizeof(short_data);
        io.backing_count     = 3u;
        short_cfg.files      = &short_file;
        short_cfg.file_count = 1;
        assert(fauxfat_init(&short_view, &short_cfg) == FAUXFAT_OK);
        data_block = short_view.cluster_heap_block +
                     (uint64_t)(short_view.data_first_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        assert(fauxfat_read_block(&short_view, data_block + 2u, b) == FAUXFAT_OK);
        assert(memcmp(b, short_data + 1024u, 210u) == 0);
        for (i = 210u; i < 512u; ++i)
            assert(b[i] == 0u);
        assert(fauxfat_read_block(&short_view, data_block + 3u, b) == FAUXFAT_OK);
        for (i = 0; i < 512u; ++i)
            assert(b[i] == 0u);

        /* Last-sector writes are clipped at DataLength, not backing-buffer size. */
        {
            fauxfat_write_mapping m;
            uint8_t in[512];
            uint8_t before[210];

            memset(in, 0xa5u, sizeof(in));
            assert(fauxfat_translate_write(&short_view, data_block + 2u, &m) == FAUXFAT_OK);
            assert(m.file_index == 0u);
            assert(m.fd == 12);
            assert(m.file_offset == 1024u);
            assert(m.length == 210u);
            assert(fauxfat_write_block(&short_view, data_block + 2u, in) == FAUXFAT_OK);
            for (i = 1024u; i < sizeof(short_data); ++i)
                assert(short_data[i] == 0xa5u);
            assert(fauxfat_read_block(&short_view, data_block + 2u, b) == FAUXFAT_OK);
            for (i = 0; i < 210u; ++i)
                assert(b[i] == 0xa5u);
            for (i = 210u; i < 512u; ++i)
                assert(b[i] == 0u);

            assert(fauxfat_translate_write(&short_view, data_block + 3u, &m) ==
                   FAUXFAT_EUNMAPPED);

            /* Multi-block writes preflight the full mapping before mutation. */
            memcpy(before, short_data + 1024u, sizeof(before));
            {
                uint8_t two[1024];
                memset(two, 0x3cu, sizeof(two));
                assert(fauxfat_write_blocks(&short_view, data_block + 2u, 2u, two) ==
                       FAUXFAT_EUNMAPPED);
            }
            assert(memcmp(before, short_data + 1024u, sizeof(before)) == 0);
        }
    }

    /*
     * Opaque/private allocations get an indirect, host-hidden descriptor:
     * zero-length File stream + vendor metadata + Vendor Allocation.  The
     * logical name lives in the vendor records, so it may intentionally match
     * a public A/B alternative without becoming a duplicate exFAT filename.
     */
    {
        fauxfat_opaque_file opaque[] = {
            { "SOLVER.DB", 13, sizeof(opaque_data), (time_t)1735787045 }
        };

        fauxfat_config ocfg = cfg;
        fauxfat_view ov;
        uint64_t oroot;
        uint64_t oblock;
        fauxfat_write_mapping m;
        static const uint8_t meta_guid[16] = {
            0x21, 0x72, 0xa6, 0xe2, 0x24, 0x0b, 0xfe, 0x41,
            0xa8, 0xad, 0xa8, 0x30, 0x3d, 0xfa, 0xa8, 0x43
        };
        static const uint8_t alloc_guid[16] = {
            0xea, 0xbd, 0x0d, 0x94, 0xf9, 0xce, 0xaa, 0x4c,
            0x85, 0x55, 0x0f, 0x60, 0xe0, 0x5b, 0xa9, 0x3c
        };

        io.backing[3].fd   = 13;
        io.backing[3].data = opaque_data;
        io.backing[3].size = sizeof(opaque_data);
        io.backing_count   = 4u;

        ocfg.opaque_files      = opaque;
        ocfg.opaque_file_count = 1u;
        assert(fauxfat_init(&ov, &ocfg) == FAUXFAT_OK);
        assert(ov.cluster_count == 7u);
        assert(ov.volume_blocks == 1152u);

        /* Public data clusters 5..7 remain canonical zero-FAT NoFatChain. */
        assert(fauxfat_read_block(&ov, 128u, b) == FAUXFAT_OK);
        assert(load32(b + 20u) == 0u);
        assert(load32(b + 24u) == 0u);
        assert(load32(b + 28u) == 0u);
        assert(load32(b + 32u) == 0xfffffff7u); /* opaque cluster 8 */

        oroot = ov.cluster_heap_block +
                (uint64_t)(ov.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        {
            uint8_t root2[1024];
            uint8_t *op;

            assert(fauxfat_read_blocks(&ov, oroot, 2u, root2) == FAUXFAT_OK);

            /* Two public 5-entry sets occupy entries 4..13. Opaque is 14..18. */
            op = root2 + 14u * 32u;
            assert(op[0] == 0x85u);
            assert(op[1] == 4u);
            assert(load16(op + 4u) == 0x0007u); /* R|H|S */
            assert(op[32u] == 0xc0u);
            assert(op[33u] == 0x01u);
            assert(load64(op + 32u + 8u) == 0u);
            assert(load32(op + 32u + 20u) == 0u);
            assert(load64(op + 32u + 24u) == 0u);
            assert(op[64u] == 0xc1u);
            assert(load16(op + 66u) == '$');
            assert(load16(op + 68u) == 'F');
            assert(load16(op + 70u) == 'F');

            assert(op[96u] == 0xe0u);
            assert(op[97u] == 0u);
            assert(memcmp(op + 98u, meta_guid, sizeof(meta_guid)) == 0);
            assert(op[114u] == 9u);
            assert(memcmp(op + 115u, "SOLVER.DB", 9u) == 0);

            assert(op[128u] == 0xe1u);
            assert(op[129u] == 0x03u);
            assert(memcmp(op + 130u, alloc_guid, sizeof(alloc_guid)) == 0);
            assert(load32(op + 148u) == 8u);
            assert(load64(op + 152u) == sizeof(opaque_data));
            assert(root2[19u * 32u] == 0xa1u);
        }

        oblock = ov.cluster_heap_block +
                 (uint64_t)(8u - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        assert(fauxfat_read_block(&ov, oblock + 9u, b) == FAUXFAT_OK);
        assert(memcmp(b, opaque_data + 9u * 512u, 512u) == 0);
        assert(io.last_fd == 13);
        assert(io.last_offset == 9u * 512u);
        assert(io.last_length == 512u);

        /* The raw bytes exist, but the host block-write translator cannot hit them. */
        assert(fauxfat_translate_write(&ov, oblock, &m) == FAUXFAT_EUNMAPPED);

        /* Opaque reads now share the same range classifier and coalesce too. */
        {
            uint8_t pair[1024];
            unsigned reads_before = io.read_calls;
            assert(fauxfat_read_blocks(&ov, oblock + 4u, 2u, pair) == FAUXFAT_OK);
            assert(io.read_calls == reads_before + 1u);
            assert(io.last_fd == 13);
            assert(io.last_offset == 4u * 512u);
            assert(io.last_length == sizeof(pair));
            assert(memcmp(pair, opaque_data + 4u * 512u, sizeof(pair)) == 0);
        }

        {
            fauxfat_disk_file d;
            assert(fauxfat_disk_file_count(&ov) == 3u);
            assert(fauxfat_describe_disk_file(&ov, 2u, &d) == FAUXFAT_OK);
            assert(d.kind == FAUXFAT_DISK_FILE_OPAQUE);
            assert(strcmp(d.name, "SOLVER.DB") == 0);
            assert(d.first_block == oblock);
            assert(d.data_length == sizeof(opaque_data));
            assert(d.allocation_blocks == FAUXFAT_BLOCKS_PER_CLUSTER);
            assert(fauxfat_describe_disk_file(&ov, 3u, &d) == FAUXFAT_ERANGE);
        }
    }

    /*
     * Sparse formatting distinguishes exact writes, must-be-zero ranges,
     * don't-care holes, and payload/private preservation.  Dirty backing
     * makes accidental zeroing/skipping visible instead of rewarding us with
     * a test that only passes because calloc is excessively polite.
     */
    {
        uint8_t short_data[1234];
        fauxfat_file short_file = {
            "SHORT.BIN", 12, sizeof(short_data), (time_t)1735787045
        };
        fauxfat_opaque_file opaque = {
            "SECRET.BIN", 13, sizeof(opaque_data), (time_t)1735787045
        };

        fauxfat_config fcfg = cfg;
        fauxfat_view fv;
        fauxfat_device dev;
        test_device media;
        fauxfat_disk_file pubd, opqd;
        preserve_test keep;
        uint8_t expected[512];
        uint64_t upcase_block;
        uint64_t bytes;
        unsigned reads_before  = io.read_calls;
        unsigned writes_before = io.write_calls;

        fcfg.files             = &short_file;
        fcfg.file_count        = 1u;
        fcfg.opaque_files      = &opaque;
        fcfg.opaque_file_count = 1u;
        assert(fauxfat_init(&fv, &fcfg) == FAUXFAT_OK);
        assert(fauxfat_describe_disk_file(&fv, 0u, &pubd) == FAUXFAT_OK);
        assert(fauxfat_describe_disk_file(&fv, 1u, &opqd) == FAUXFAT_OK);

        bytes = fv.volume_blocks * FAUXFAT_BLOCK_SIZE;
        assert(bytes <= SIZE_MAX);
        memset(&media, 0, sizeof(media));
        media.blocks = fv.volume_blocks;
        media.data   = (uint8_t *)malloc((size_t)bytes);
        assert(media.data != NULL);
        memset(media.data, 0xa9, (size_t)bytes);

        memset(&dev, 0, sizeof(dev));
        dev.read    = dev_read;
        dev.write   = dev_write;
        dev.zero    = dev_zero;
        dev.skip    = dev_skip;
        dev.context = &media;

        assert(fauxfat_format(&fv, &dev, NULL, NULL, 0u) == FAUXFAT_OK);
        assert(io.read_calls == reads_before);
        assert(io.write_calls == writes_before);
        assert(media.write_calls != 0u);
        assert(media.zero_calls != 0u);
        assert(media.undefined_skips != 0u);
        assert(media.preserve_skips == 1u);
        assert(media.preserve_blocks == opqd.allocation_blocks);

        /* Schema-free reopen reconstructs geometry, identity and physical
         * descriptors using only the block accessor. */
        {
            fauxfat_reopen_info ri;
            fauxfat_volume_class vc;
            emit_test got;
            size_t count = 999u;

            memset(&ri, 0xcc, sizeof(ri));
            memset(&got, 0, sizeof(got));
            assert(fauxfat_reopen(&dev, collect_file, &got, &count, &vc,
                                  &ri) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
            assert(count == 2u && got.count == 2u);
            assert(strcmp(got.file[0].name, "SHORT.BIN") == 0);
            assert(got.file[0].first_block == pubd.first_block);
            assert(got.file[1].kind == FAUXFAT_DISK_FILE_OPAQUE);
            assert(strcmp(got.file[1].name, "SECRET.BIN") == 0);
            assert(got.file[1].first_block == opqd.first_block);
            assert(ri.partition_lba == fcfg.partition_lba);
            assert(ri.volume_blocks == fv.volume_blocks);
            assert(ri.structural_epoch == fcfg.structural_epoch);
            assert(ri.volume_serial == fcfg.volume_serial);
            assert(ri.fat_length_blocks == fv.fat_length_blocks);
            assert(ri.cluster_heap_block == fv.cluster_heap_block);
            assert(ri.cluster_count == fv.cluster_count);
            assert(ri.root_cluster == fv.root_cluster);
            assert(memcmp(ri.volume_guid, fcfg.volume_guid, 16u) == 0);
            assert(strcmp(ri.volume_label, fcfg.volume_label) == 0);
        }

        /* Structural bytes are rendered exactly. */
        assert(fauxfat_read_block(&fv, 0u, expected) == FAUXFAT_OK);
        assert(memcmp(media.data, expected, sizeof(expected)) == 0);

        /* FAT alignment is undefined, not required-zero. */
        for (i = 0; i < FAUXFAT_BLOCK_SIZE; ++i)
            assert(media.data[24u * FAUXFAT_BLOCK_SIZE + i] == 0xa9u);

        /* Public valid bytes are zeroed, allocation slack is left alone. */
        for (i = 0; i < 3u * FAUXFAT_BLOCK_SIZE; ++i)
            assert(media.data[pubd.first_block * FAUXFAT_BLOCK_SIZE + i] == 0u);
        for (i = 3u * FAUXFAT_BLOCK_SIZE;
             i < 4u * FAUXFAT_BLOCK_SIZE; ++i)
            assert(media.data[pubd.first_block * FAUXFAT_BLOCK_SIZE + i] == 0xa9u);

        /* The complete opaque allocation is untouched. */
        for (i = 0; i < FAUXFAT_BLOCK_SIZE; ++i)
            assert(media.data[opqd.first_block * FAUXFAT_BLOCK_SIZE + i] == 0xa9u);

        /* Upcase contents are generated, the rest of its cluster is undefined. */
        upcase_block = fv.cluster_heap_block +
                       (uint64_t)(fv.upcase_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        assert(fauxfat_read_block(&fv, upcase_block, expected) == FAUXFAT_OK);
        assert(memcmp(media.data + upcase_block * FAUXFAT_BLOCK_SIZE,
                      expected, sizeof(expected)) == 0);
        for (i = 0; i < FAUXFAT_BLOCK_SIZE; ++i)
            assert(media.data[(upcase_block + 1u) * FAUXFAT_BLOCK_SIZE + i] == 0xa9u);

        /* ZERO_UNDEFINED strengthens don't-care holes but never touches opaque. */
        memset(media.data, 0xa9, (size_t)bytes);
        media.write_calls = media.zero_calls = 0u;
        media.undefined_skips = media.preserve_skips = 0u;
        media.preserve_blocks                        = 0u;
        assert(fauxfat_format(&fv, &dev, NULL, NULL,
                              FAUXFAT_FORMAT_ZERO_UNDEFINED) == FAUXFAT_OK);
        for (i = 0; i < FAUXFAT_BLOCK_SIZE; ++i) {
            assert(media.data[24u * FAUXFAT_BLOCK_SIZE + i] == 0u);
            assert(media.data[(upcase_block + 1u) * FAUXFAT_BLOCK_SIZE + i] == 0u);
            assert(media.data[opqd.first_block * FAUXFAT_BLOCK_SIZE + i] == 0xa9u);
        }
        assert(media.undefined_skips == 0u);
        assert(media.preserve_skips == 1u);

        /* An exact public descriptor can be preserved in place as well. */
        memset(media.data, 0x6du, (size_t)bytes);
        media.undefined_skips = media.preserve_skips = 0u;
        media.preserve_blocks                        = 0u;
        memset(&keep, 0, sizeof(keep));
        keep.name = "SHORT.BIN";
        assert(fauxfat_format(&fv, &dev, preserve_named, &keep, 0u) == FAUXFAT_OK);
        assert(keep.calls == 1u);
        assert(keep.last.kind == FAUXFAT_DISK_FILE_PUBLIC);
        assert(keep.last.first_block == pubd.first_block);
        assert(keep.last.allocation_blocks == pubd.allocation_blocks);
        for (i = 0; i < FAUXFAT_BLOCK_SIZE; ++i)
            assert(media.data[pubd.first_block * FAUXFAT_BLOCK_SIZE + i] == 0x6du);
        assert(media.preserve_skips == 2u); /* public + opaque */
        assert(media.preserve_blocks == pubd.allocation_blocks + opqd.allocation_blocks);

        /* Strict root parsing reconstructs direct physical descriptors without
         * retaining a file table or touching FAT chains. */
        {
            emit_test got;
            size_t count        = 999u;
            uint64_t root_block = fv.cluster_heap_block +
                                  (uint64_t)(fv.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
            uint8_t *root = media.data + root_block * FAUXFAT_BLOCK_SIZE;
            uint8_t saved_public[160];

            memset(&got, 0, sizeof(got));
            assert(fauxfat_parse_root_strict(&fv, &dev, collect_file, &got,
                                             &count) == FAUXFAT_OK);
            assert(count == 2u && got.count == 2u);
            assert(got.file[0].kind == FAUXFAT_DISK_FILE_PUBLIC);
            assert(strcmp(got.file[0].name, "SHORT.BIN") == 0);
            assert(got.file[0].first_block == pubd.first_block);
            assert(got.file[0].data_length == pubd.data_length);
            assert(got.file[0].allocation_blocks == pubd.allocation_blocks);
            assert(got.file[0].mtime == short_file.mtime);
            assert(got.file[0].flags == 0u);
            assert(got.file[1].kind == FAUXFAT_DISK_FILE_OPAQUE);
            assert(strcmp(got.file[1].name, "SECRET.BIN") == 0);
            assert(got.file[1].first_block == opqd.first_block);
            assert(got.file[1].allocation_blocks == opqd.allocation_blocks);
            assert(got.file[1].flags == 0u);
            memcpy(saved_public, root + 4u * 32u, sizeof(saved_public));

            /*
             * A host namespace rename does not erase the manufactured logical
             * identity.  NameHash is case-folded, so a case-only rename lets
             * us exercise this without inventing a second copy of the hash
             * algorithm in the test.
             */
            {
                fauxfat_volume_class vc;
                unsigned j;

                for (j = 0u; j < 9u; ++j) {
                    uint8_t *lo = root + 6u * 32u + 2u + 2u * j;
                    if (*lo >= 'A' && *lo <= 'Z')
                        *lo = (uint8_t)(*lo - 'A' + 'a');
                }
                store16(root + 4u * 32u + 2u,
                        entry_set_checksum(root + 4u * 32u, 160u));

                memset(&got, 0, sizeof(got));
                assert(fauxfat_parse_root_strict(&fv, &dev, collect_file, &got,
                                                 &count) == FAUXFAT_OK);
                assert(strcmp(got.file[0].name, "SHORT.BIN") == 0);
                assert((got.file[0].flags & FAUXFAT_DISK_FILE_NAME_CHANGED) != 0u);

                memset(&got, 0, sizeof(got));
                assert(fauxfat_scan_loose(&fv, &dev, collect_file, &got,
                                          &count, &vc) == FAUXFAT_OK);
                assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
                assert(count == 2u && got.count == 2u);
                assert(strcmp(got.file[0].name, "SHORT.BIN") == 0);
                assert((got.file[0].flags & FAUXFAT_DISK_FILE_NAME_CHANGED) != 0u);

                memcpy(root + 4u * 32u, saved_public, sizeof(saved_public));
            }

            /* Loose scan of the canonical image reports full fauxFAT validity. */
            {
                fauxfat_volume_class vc;
                memset(&got, 0, sizeof(got));
                assert(fauxfat_scan_loose(&fv, &dev, collect_file, &got,
                                          &count, &vc) == FAUXFAT_OK);
                assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
                assert(count == 2u && got.count == 2u);
            }

            /*
             * Best effort also understands an ordinary three-entry contiguous
             * NoFatChain file.  Remove fauxFAT's two origin-name vendor records
             * and turn those slots back into benign padding.
             */
            {
                fauxfat_volume_class vc;
                uint8_t *set = root + 4u * 32u;

                set[1] = 2u;
                memset(set + 96u, 0, 64u);
                set[96u] = 0xa1u;
                store16(set + 98u, 0x0508u);
                set[128u] = 0xa1u;
                store16(set + 130u, 0x0508u);
                store16(set + 2u, entry_set_checksum(set, 96u));

                memset(&got, 0, sizeof(got));
                assert(fauxfat_scan_loose(&fv, &dev, collect_file, &got,
                                          &count, &vc) == FAUXFAT_OK);
                assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
                assert(count == 2u && got.count == 2u);
                assert(strcmp(got.file[0].name, "SHORT.BIN") == 0);
                assert(got.file[0].flags == 0u);

                /* A fragmented ordinary file is valid-but-unsupported: skip it. */
                set[32u + 1u] = 0x01u;
                store16(set + 2u, entry_set_checksum(set, 96u));
                memset(&got, 0, sizeof(got));
                assert(fauxfat_scan_loose(&fv, &dev, collect_file, &got,
                                          &count, &vc) == FAUXFAT_OK);
                assert(count == 1u && got.count == 1u);
                assert(got.file[0].kind == FAUXFAT_DISK_FILE_OPAQUE);

                /* But a malformed set is ambiguity, not something to shrug at. */
                set[2u] ^= 0x40u;
                assert(fauxfat_scan_loose(&fv, &dev, NULL, NULL,
                                          NULL, &vc) == FAUXFAT_ESTRUCTURE);

                memcpy(root + 4u * 32u, saved_public, sizeof(saved_public));
            }

            /* The parser callback is a streaming sink, so caller errors pass through. */
            memset(&got, 0, sizeof(got));
            got.fail_code = -77;
            assert(fauxfat_parse_root_strict(&fv, &dev, collect_file, &got,
                                             NULL) == -77);

            /* Archive plus modify/access time changes are host-volatile, but the
             * entry-set checksum must still describe the bytes actually on disk. */
            memcpy(saved_public, root + 4u * 32u, sizeof(saved_public));
            store16(root + 4u * 32u + 4u, 0x0020u);
            root[4u * 32u + 12u] ^= 0x01u;
            root[4u * 32u + 21u] = 17u;
            root[4u * 32u + 23u] = 0u;
            root[4u * 32u + 24u] = 0x80u;
            store16(root + 4u * 32u + 2u,
                    entry_set_checksum(root + 4u * 32u, 160u));
            memset(&got, 0, sizeof(got));
            assert(fauxfat_parse_root_strict(&fv, &dev, collect_file, &got,
                                             &count) == FAUXFAT_OK);
            assert(got.file[0].mtime == short_file.mtime);

            /* A structural field is not made acceptable by recomputing the native
             * exFAT checksum. Moving SHORT into SECRET's cluster overlaps the map. */
            memcpy(root + 4u * 32u, saved_public, sizeof(saved_public));
            root[5u * 32u + 20u] = (uint8_t)(fv.data_first_cluster + 1u);
            root[5u * 32u + 21u] = 0u;
            root[5u * 32u + 22u] = 0u;
            root[5u * 32u + 23u] = 0u;
            store16(root + 4u * 32u + 2u,
                    entry_set_checksum(root + 4u * 32u, 160u));
            assert(fauxfat_parse_root_strict(&fv, &dev, NULL, NULL, NULL) ==
                   FAUXFAT_ESTRUCTURE);

            /* Malformed SetChecksum also fails immediately. */
            memcpy(root + 4u * 32u, saved_public, sizeof(saved_public));
            root[4u * 32u + 2u] ^= 0x80u;
            assert(fauxfat_parse_root_strict(&fv, &dev, NULL, NULL, NULL) ==
                   FAUXFAT_ESTRUCTURE);

            /* Leave the test image canonical for any later checks. */
            memcpy(root + 4u * 32u, saved_public, sizeof(saved_public));
        }

        /*
         * Whole-volume strict validation deliberately ignores payload and
         * undefined slack, but checks every structural byte after masking the
         * small exFAT host-mutable set.
         */
        {
            fauxfat_volume_class vc;
            uint64_t root_block = fv.cluster_heap_block +
                                  (uint64_t)(fv.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
            uint8_t *root         = media.data + root_block * FAUXFAT_BLOCK_SIZE;
            uint64_t upcase_first = fv.cluster_heap_block +
                                    (uint64_t)(fv.upcase_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
            uint32_t pub_cluster = 2u + (uint32_t)((pubd.first_block - fv.cluster_heap_block) /
                                                   FAUXFAT_BLOCKS_PER_CLUSTER);
            uint64_t fat_byte    = (uint64_t)pub_cluster * 4u;
            uint64_t fat_block   = 128u + fat_byte / FAUXFAT_BLOCK_SIZE;
            size_t fat_off       = (size_t)(fat_byte % FAUXFAT_BLOCK_SIZE);
            uint8_t saved;
            uint8_t saved_public[160];

            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);

            /* Main VolumeDirty is current volatile state. */
            media.data[106u] = 0x02u;
            media.data[107u] = 0u;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
            media.data[106u] = 0u;

            /* Backup volatile state is stale and need not match Main. */
            media.data[12u * FAUXFAT_BLOCK_SIZE + 106u] = 0x0eu;
            media.data[12u * FAUXFAT_BLOCK_SIZE + 107u] = 0u;
            media.data[12u * FAUXFAT_BLOCK_SIZE + 112u] = 42u;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
            media.data[12u * FAUXFAT_BLOCK_SIZE + 106u] = 0u;
            media.data[12u * FAUXFAT_BLOCK_SIZE + 112u] = 100u;

            /* The allowed File mutations canonicalize back to the seal. */
            memcpy(saved_public, root + 4u * 32u, sizeof(saved_public));
            store16(root + 4u * 32u + 4u, 0x0020u);
            root[4u * 32u + 12u] ^= 0x01u;
            root[4u * 32u + 21u] = 17u;
            root[4u * 32u + 23u] = 0u;
            root[4u * 32u + 24u] = 0x80u;
            store16(root + 4u * 32u + 2u,
                    entry_set_checksum(root + 4u * 32u, 160u));
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
            assert(fauxfat_reopen(&dev, NULL, NULL, NULL, &vc, NULL) ==
                   FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
            memcpy(root + 4u * 32u, saved_public, sizeof(saved_public));

            /* A structurally valid but unauthorized creation-time change is
             * still a changed fauxFAT image, even with a repaired native set checksum. */
            root[4u * 32u + 8u] ^= 0x01u;
            store16(root + 4u * 32u + 2u,
                    entry_set_checksum(root + 4u * 32u, 160u));
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            assert(fauxfat_reopen(&dev, NULL, NULL, NULL, &vc, NULL) ==
                   FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            memcpy(root + 4u * 32u, saved_public, sizeof(saved_public));

            /* Each separately sealed structural component is checked directly. */
            saved = media.data[fat_block * FAUXFAT_BLOCK_SIZE + fat_off];
            media.data[fat_block * FAUXFAT_BLOCK_SIZE + fat_off] ^= 0x01u;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            assert(fauxfat_reopen(&dev, NULL, NULL, NULL, &vc, NULL) ==
                   FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            media.data[fat_block * FAUXFAT_BLOCK_SIZE + fat_off] = saved;

            saved = media.data[fv.cluster_heap_block * FAUXFAT_BLOCK_SIZE];
            media.data[fv.cluster_heap_block * FAUXFAT_BLOCK_SIZE] ^= 0x01u;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            media.data[fv.cluster_heap_block * FAUXFAT_BLOCK_SIZE] = saved;

            saved = media.data[upcase_first * FAUXFAT_BLOCK_SIZE];
            media.data[upcase_first * FAUXFAT_BLOCK_SIZE] ^= 0x01u;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            media.data[upcase_first * FAUXFAT_BLOCK_SIZE] = saved;

            /* Main PercentInUse does not get the Backup's stale-state waiver. */
            saved            = media.data[112u];
            media.data[112u] = 99u;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            media.data[112u] = saved;

            /* Losing one OEM identity copy is still recognizable fauxFAT via
             * the other boot region, so schema-free reopen reports CHANGED.
             * Losing both identities falls back to bounded exFAT salvage. */
            saved = media.data[9u * FAUXFAT_BLOCK_SIZE];
            media.data[9u * FAUXFAT_BLOCK_SIZE] ^= 0x80u;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_INVALID);
            {
                emit_test got;
                size_t reopen_count = 0u;
                uint8_t saved_backup =
                    media.data[21u * FAUXFAT_BLOCK_SIZE];

                memset(&got, 0, sizeof(got));
                assert(fauxfat_reopen(&dev, collect_file, &got,
                                      &reopen_count, &vc, NULL) ==
                       FAUXFAT_OK);
                assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
                assert(reopen_count == 2u && got.count == 2u);

                media.data[21u * FAUXFAT_BLOCK_SIZE] ^= 0x80u;
                memset(&got, 0, sizeof(got));
                reopen_count = 0u;
                assert(fauxfat_reopen(&dev, collect_file, &got,
                                      &reopen_count, &vc, NULL) ==
                       FAUXFAT_OK);
                assert(vc == FAUXFAT_VOLUME_EXFAT_BEST_EFFORT);
                assert(reopen_count == 2u && got.count == 2u);
                media.data[21u * FAUXFAT_BLOCK_SIZE] = saved_backup;
            }
            media.data[9u * FAUXFAT_BLOCK_SIZE] = saved;

            /* Storage errors remain storage errors, not structural classifications. */
            media.fail_read_block = 128u;
            media.fail_read_code  = -333;
            assert(fauxfat_validate_strict(&fv, &dev, &vc) == -333);
            media.fail_read_code = 0;

            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
        }

        /*
         * A changed-but-understood façade can be discarded and regenerated
         * without sacrificing payload ranges recovered by the loose scan.
         * This is the actual repair path rather than merely testing the
         * formatter and parser in isolation.
         */
        {
            emit_test recovered;
            recovered_preserve_test rp;
            fauxfat_volume_class vc;
            size_t count        = 0u;
            uint64_t root_block = fv.cluster_heap_block +
                                  (uint64_t)(fv.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
            uint8_t *root      = media.data + root_block * FAUXFAT_BLOCK_SIZE;
            uint8_t *pub       = media.data + pubd.first_block * FAUXFAT_BLOCK_SIZE;
            uint8_t *opq       = media.data + opqd.first_block * FAUXFAT_BLOCK_SIZE;
            uint8_t *undefined = media.data + 24u * FAUXFAT_BLOCK_SIZE;
            uint64_t pub_bytes = pubd.allocation_blocks * FAUXFAT_BLOCK_SIZE;
            uint64_t opq_bytes = opqd.allocation_blocks * FAUXFAT_BLOCK_SIZE;
            unsigned j;

            assert(pub_bytes <= SIZE_MAX && opq_bytes <= SIZE_MAX);
            memset(pub, 0x42, (size_t)pub_bytes);
            memset(opq, 0x99, (size_t)opq_bytes);
            memset(undefined, 0x5a, FAUXFAT_BLOCK_SIZE);

            /* Case-only host rename: native exFAT remains coherent, fauxFAT
             * identity is recovered from the vendor original-name records. */
            for (j = 0u; j < 9u; ++j) {
                uint8_t *lo = root + 6u * 32u + 2u + 2u * j;
                if (*lo >= 'A' && *lo <= 'Z')
                    *lo = (uint8_t)(*lo - 'A' + 'a');
            }
            store16(root + 4u * 32u + 2u,
                    entry_set_checksum(root + 4u * 32u, 160u));

            memset(&recovered, 0, sizeof(recovered));
            assert(fauxfat_reopen(&dev, collect_file, &recovered, &count,
                                  &vc, NULL) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_CHANGED);
            assert(count == 2u && recovered.count == 2u);
            assert(strcmp(recovered.file[0].name, "SHORT.BIN") == 0);
            assert((recovered.file[0].flags &
                    FAUXFAT_DISK_FILE_NAME_CHANGED) != 0u);
            assert(recovered.file[0].first_block == pubd.first_block);
            assert(recovered.file[1].kind == FAUXFAT_DISK_FILE_OPAQUE);
            assert(recovered.file[1].first_block == opqd.first_block);

            memset(&rp, 0, sizeof(rp));
            rp.recovered = &recovered;
            assert(fauxfat_format(&fv, &dev, preserve_recovered, &rp,
                                  FAUXFAT_FORMAT_ZERO_UNDEFINED) ==
                   FAUXFAT_OK);
            assert(rp.calls == 1u);

            /* Both understood public bytes and opaque/private bytes survive;
             * unrelated undefined dirt does not acquire preservation merely
             * because it happened to be present on the old volume. */
            for (j = 0u; j < (unsigned)pub_bytes; ++j)
                assert(pub[j] == 0x42u);
            for (j = 0u; j < (unsigned)opq_bytes; ++j)
                assert(opq[j] == 0x99u);
            for (j = 0u; j < FAUXFAT_BLOCK_SIZE; ++j)
                assert(undefined[j] == 0u);

            assert(fauxfat_validate_strict(&fv, &dev, &vc) == FAUXFAT_OK);
            assert(vc == FAUXFAT_VOLUME_FAUXFAT_VALID);
            memset(&recovered, 0, sizeof(recovered));
            assert(fauxfat_parse_root_strict(&fv, &dev, collect_file,
                                             &recovered, &count) ==
                   FAUXFAT_OK);
            assert(strcmp(recovered.file[0].name, "SHORT.BIN") == 0);
            assert(recovered.file[0].flags == 0u);

            /* Merely having bytes in a public extent is not sufficient to
             * preserve them. With no matching recovered descriptor, a repair
             * performs the normal required-zero initialization. */
            memset(pub, 0x7bu, (size_t)pub_bytes);
            memset(&recovered, 0, sizeof(recovered));
            memset(&rp, 0, sizeof(rp));
            rp.recovered = &recovered;
            assert(fauxfat_format(&fv, &dev, preserve_recovered, &rp, 0u) ==
                   FAUXFAT_OK);
            assert(rp.calls == 1u);
            for (j = 0u;
                 j < (unsigned)((pubd.data_length + FAUXFAT_BLOCK_SIZE - 1u) /
                                FAUXFAT_BLOCK_SIZE * FAUXFAT_BLOCK_SIZE);
                 ++j)
                assert(pub[j] == 0u);
            /* Allocation slack remains undefined under the normal format. */
            for (j = (unsigned)(((pubd.data_length + FAUXFAT_BLOCK_SIZE - 1u) /
                                 FAUXFAT_BLOCK_SIZE) *
                                FAUXFAT_BLOCK_SIZE);
                 j < (unsigned)pub_bytes; ++j)
                assert(pub[j] == 0x7bu);
            for (j = 0u; j < (unsigned)opq_bytes; ++j)
                assert(opq[j] == 0x99u);
        }

        assert(fauxfat_format(&fv, &dev, NULL, NULL, 2u) == FAUXFAT_EINVAL);
        free(media.data);
    }

    /* Maximum-length logical opaque names spill their last two bytes into
     * Vendor Allocation.VendorDefined; no second name-extension entry needed. */
    {
        fauxfat_opaque_file opaque = {
            "ABCDEFGHIJKLMNO", 13, sizeof(opaque_data), (time_t)1735787045
        };

        fauxfat_config ocfg = cfg;
        fauxfat_view ov;
        uint64_t oroot;

        ocfg.files             = NULL;
        ocfg.file_count        = 0u;
        ocfg.write             = NULL; /* opaque-only view is read-only */
        ocfg.opaque_files      = &opaque;
        ocfg.opaque_file_count = 1u;
        assert(fauxfat_init(&ov, &ocfg) == FAUXFAT_OK);
        oroot = ov.cluster_heap_block +
                (uint64_t)(ov.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        assert(fauxfat_read_block(&ov, oroot, b) == FAUXFAT_OK);
        assert(b[224u] == 0xe0u); /* entry 7: Vendor Extension */
        assert(b[242u] == 15u);
        assert(memcmp(b + 243u, "ABCDEFGHIJKLM", 13u) == 0);
        assert(b[256u] == 0xe1u); /* entry 8: Vendor Allocation */
        assert(b[274u] == 'N');
        assert(b[275u] == 'O');

        {
            fauxfat_device parse_dev;
            emit_test got;
            size_t count = 0u;

            memset(&parse_dev, 0, sizeof(parse_dev));
            parse_dev.read    = view_dev_read;
            parse_dev.context = &ov;
            memset(&got, 0, sizeof(got));
            assert(fauxfat_parse_root_strict(&ov, &parse_dev, collect_file,
                                             &got, &count) == FAUXFAT_OK);
            assert(count == 1u && got.count == 1u);
            assert(got.file[0].kind == FAUXFAT_DISK_FILE_OPAQUE);
            assert(strcmp(got.file[0].name, "ABCDEFGHIJKLMNO") == 0);
        }
    }

    /* Public logical names use the same 15-byte ceiling, split across the two
     * benign original-name Vendor Extension records. */
    {
        fauxfat_file long_file = {
            "ABCDEFGHIJKLMNO", 11, sizeof(config_data), (time_t)1735787045
        };

        fauxfat_config lcfg = cfg;
        fauxfat_view lv;
        uint64_t lroot;
        fauxfat_device parse_dev;
        emit_test got;
        size_t count = 0u;

        lcfg.files             = &long_file;
        lcfg.file_count        = 1u;
        lcfg.opaque_files      = NULL;
        lcfg.opaque_file_count = 0u;
        assert(fauxfat_init(&lv, &lcfg) == FAUXFAT_OK);
        lroot = lv.cluster_heap_block +
                (uint64_t)(lv.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
        assert(fauxfat_read_block(&lv, lroot, b) == FAUXFAT_OK);
        assert(b[224u] == 0xe0u);
        assert(b[242u] == 15u);
        assert(memcmp(b + 243u, "ABCDEFGHIJKLM", 13u) == 0);
        assert(b[256u] == 0xe0u);
        assert(b[274u] == 'N');
        assert(b[275u] == 'O');
        assert(b[276u] == 15u);

        memset(&parse_dev, 0, sizeof(parse_dev));
        parse_dev.read    = view_dev_read;
        parse_dev.context = &lv;
        memset(&got, 0, sizeof(got));
        assert(fauxfat_parse_root_strict(&lv, &parse_dev, collect_file,
                                         &got, &count) == FAUXFAT_OK);
        assert(count == 1u && got.count == 1u);
        assert(strcmp(got.file[0].name, "ABCDEFGHIJKLMNO") == 0);
        assert(got.file[0].flags == 0u);
    }

    /* exFAT cannot encode dates before 1980 or after 2107. */
    {
        fauxfat_file bad_time = {
            "BADTIME.BIN", 11, sizeof(config_data), (time_t)315532799
        };

        fauxfat_config badcfg = cfg;
        badcfg.files          = &bad_time;
        badcfg.file_count     = 1u;
        assert(fauxfat_init(&view, &badcfg) == FAUXFAT_EINVAL);

        bad_time.mtime = (time_t)4354819200LL;
        assert(fauxfat_init(&view, &badcfg) == FAUXFAT_EINVAL);
    }

    puts("fauxfat tests: ok");
    return 0;
}
