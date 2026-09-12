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
    size_t remaining = 5836u;
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

int main(void)
{
    static uint8_t solver[2u * FAUXFAT_CLUSTER_SIZE];
    static uint8_t config_data[FAUXFAT_CLUSTER_SIZE];
    static const uint8_t guid[16] = {
        0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a, 0xf0, 0xde,
        0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    };
    fauxfat_file files[] = {
        { "SOLVER.DB", solver, sizeof(solver) },
        { "CONFIG.BIN", config_data, sizeof(config_data) }
    };

    fauxfat_config cfg;
    fauxfat_view view;
    uint8_t b[512];
    uint64_t root_block;
    uint64_t solver_block;
    uint64_t config_block;
    unsigned i;

    fill_pattern(solver, sizeof(solver), 0x31u);
    fill_pattern(config_data, sizeof(config_data), 0xa7u);

    memset(&cfg, 0, sizeof(cfg));
    cfg.files            = files;
    cfg.file_count       = sizeof(files) / sizeof(files[0]);
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

    assert(upcase_checksum(&view) == 0xe619d30du);

    root_block = view.cluster_heap_block +
                 (uint64_t)(view.root_cluster - 2u) * FAUXFAT_BLOCKS_PER_CLUSTER;
    assert(fauxfat_read_block(&view, root_block, b) == FAUXFAT_OK);
    assert(b[0] == 0x81u);
    assert(b[32] == 0x82u);
    assert(b[64] == 0x83u);
    assert(b[96] == 0xa0u);

    /* First file set starts at root entry 4, i.e. byte 128. */
    assert(b[128] == 0x85u);
    assert(b[160] == 0xc0u);
    assert(b[192] == 0xc1u);
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
        assert(fauxfat_read_blocks(&view, solver_block, 2u, pair) == FAUXFAT_OK);
        assert(memcmp(pair, solver, sizeof(pair)) == 0);
    }
    assert(fauxfat_read_block(&view, solver_block + 128u, b) == FAUXFAT_OK);
    assert(memcmp(b, solver + FAUXFAT_CLUSTER_SIZE, sizeof(b)) == 0);
    assert(fauxfat_read_block(&view, config_block + 17u, b) == FAUXFAT_OK);
    assert(memcmp(b, config_data + 17u * 512u, sizeof(b)) == 0);

    assert(fauxfat_read_block(&view, view.volume_blocks, b) == FAUXFAT_ERANGE);

    /* v1 refuses names or capacities which would mutate the manufactured map. */
    {
        fauxfat_file bad      = { "lower.bin", config_data, sizeof(config_data) };
        fauxfat_config badcfg = cfg;
        badcfg.files          = &bad;
        badcfg.file_count     = 1;
        assert(fauxfat_init(&view, &badcfg) == FAUXFAT_EINVAL);
    }
    {
        uint8_t short_data[1234];
        fauxfat_file short_file  = { "SHORT.BIN", short_data, sizeof(short_data) };
        fauxfat_config short_cfg = cfg;
        fauxfat_view short_view;
        uint64_t data_block;

        fill_pattern(short_data, sizeof(short_data), 0x42u);
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
    }

    puts("fauxfat tests: ok");
    return 0;
}
