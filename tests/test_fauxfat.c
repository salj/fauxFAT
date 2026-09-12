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
} test_device;

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

static int preserve_named(void *context, const fauxfat_disk_file *wanted)
{
    preserve_test *p = (preserve_test *)context;

    ++p->calls;
    p->last = *wanted;
    return strcmp(wanted->name, p->name) == 0;
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
    assert(memcmp(b + 64, "FFV1", 4) == 0);
    assert(load16(b + 68) == 1u);
    assert(load64(b + 72) == 0x1122334455667788ull);
    assert(load32(b + 80) == view.fat_crc32c);
    assert(load32(b + 84) == view.bitmap_crc32c);
    assert(load32(b + 88) == view.root_crc32c);

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
    assert(b[160] == 0xc0u);
    assert(b[192] == 0xc1u);
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

    /* Second file begins at entry 7, and entry 10 is the next padded slot. */
    assert(b[224] == 0x85u);
    assert(load32(b + 256 + 20) == 7u);
    assert(b[320] == 0xa1u);
    assert(load16(b + 322) == 0x0508u);
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
        assert(fauxfat_read_block(&ov, oroot, b) == FAUXFAT_OK);

        /* Two public 3-entry sets occupy entries 4..9. Opaque set is 10..14. */
        assert(b[320u] == 0x85u);
        assert(b[321u] == 4u);
        assert(load16(b + 324u) == 0x0007u); /* R|H|S */
        assert(b[352u] == 0xc0u);
        assert(b[353u] == 0x01u);
        assert(load64(b + 352u + 8u) == 0u);
        assert(load32(b + 352u + 20u) == 0u);
        assert(load64(b + 352u + 24u) == 0u);
        assert(b[384u] == 0xc1u);
        assert(load16(b + 386u) == '$');
        assert(load16(b + 388u) == 'F');
        assert(load16(b + 390u) == 'F');

        assert(b[416u] == 0xe0u);
        assert(b[417u] == 0u);
        assert(memcmp(b + 418u, meta_guid, sizeof(meta_guid)) == 0);
        assert(b[434u] == 9u);
        assert(memcmp(b + 435u, "SOLVER.DB", 9u) == 0);

        assert(b[448u] == 0xe1u);
        assert(b[449u] == 0x03u);
        assert(memcmp(b + 450u, alloc_guid, sizeof(alloc_guid)) == 0);
        assert(load32(b + 468u) == 8u);
        assert(load64(b + 472u) == sizeof(opaque_data));
        assert(b[480u] == 0xa1u); /* descriptor is followed by occupied padding */

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
