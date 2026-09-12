#include "fauxfat_block.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct memdev {
    uint8_t *data;
    uint64_t blocks;
    unsigned flushes;
    uint64_t read_calls;
    uint64_t write_calls;
    uint64_t zero_calls;
    uint64_t fail_read_call;
    int fail_read_code;
    int fail_write_code;
    int fail_zero_code;
    int fail_skip_code;
    int fail_flush_code;
} memdev;

static int mem_read(void *context, uint64_t first_block,
                    size_t block_count, void *data)
{
    memdev *m      = (memdev *)context;
    uint64_t count = (uint64_t)block_count;

    ++m->read_calls;
    if (m->fail_read_call != 0u && m->read_calls == m->fail_read_call)
        return m->fail_read_code;
    if (first_block > m->blocks || count > m->blocks - first_block)
        return -101;
    memcpy(data, m->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE),
           block_count * FAUXFAT_BLOCK_SIZE);
    return 0;
}

static int mem_write(void *context, uint64_t first_block,
                     size_t block_count, const void *data)
{
    memdev *m      = (memdev *)context;
    uint64_t count = (uint64_t)block_count;

    ++m->write_calls;
    if (m->fail_write_code != 0)
        return m->fail_write_code;
    if (first_block > m->blocks || count > m->blocks - first_block)
        return -102;
    memcpy(m->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE), data,
           block_count * FAUXFAT_BLOCK_SIZE);
    return 0;
}

static int mem_zero(void *context, uint64_t first_block, uint64_t block_count)
{
    memdev *m = (memdev *)context;

    ++m->zero_calls;
    if (m->fail_zero_code != 0)
        return m->fail_zero_code;
    if (first_block > m->blocks || block_count > m->blocks - first_block)
        return -103;
    memset(m->data + (size_t)(first_block * FAUXFAT_BLOCK_SIZE), 0,
           (size_t)(block_count * FAUXFAT_BLOCK_SIZE));
    return 0;
}

static int mem_skip(void *context, uint64_t first_block, uint64_t block_count,
                    fauxfat_skip_kind kind)
{
    memdev *m = (memdev *)context;
    (void)kind;
    if (m->fail_skip_code != 0)
        return m->fail_skip_code;
    if (first_block > m->blocks || block_count > m->blocks - first_block)
        return -104;
    return 0;
}

static int mem_flush(void *context)
{
    memdev *m = (memdev *)context;
    if (m->fail_flush_code != 0)
        return m->fail_flush_code;
    ++m->flushes;
    return 0;
}

static void fill_guid(uint8_t guid[16], uint8_t seed)
{
    unsigned i;
    for (i = 0u; i < 16u; ++i)
        guid[i] = (uint8_t)(seed + i * 11u);
}

static void init_fauxfat(fauxfat_config *cfg, fauxfat_view *view,
                         uint64_t partition_lba)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->data_cluster_count = 16u;
    cfg->partition_lba      = partition_lba;
    cfg->volume_serial      = 0x12345678u;
    cfg->structural_epoch   = 7u;
    fill_guid(cfg->volume_guid, 0x31u);
    cfg->volume_label = "INGRESS";
    assert(fauxfat_init(view, cfg) == FAUXFAT_OK);
}

static void setup_gpt(fauxgpt_partition parts[2], fauxgpt_layout *layout,
                      fauxgpt_view *gpt, uint64_t disk_blocks,
                      uint64_t p1_first, uint64_t p1_blocks)
{
    uint64_t p2_first = p1_first + p1_blocks;

    memset(parts, 0, 2u * sizeof(*parts));
    memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[0].unique_guid, 0x51u);
    parts[0].first_lba   = p1_first;
    parts[0].block_count = p1_blocks;
    parts[0].name        = "INGRESS";

    memcpy(parts[1].type_guid, fauxgpt_type_microsoft_basic_data, 16u);
    fill_guid(parts[1].unique_guid, 0x71u);
    parts[1].first_lba   = p2_first;
    parts[1].block_count = (disk_blocks - 34u) - p2_first + 1u;
    parts[1].name        = "USER DATA";

    memset(layout, 0, sizeof(*layout));
    layout->disk_blocks = disk_blocks;
    fill_guid(layout->disk_guid, 0x91u);
    layout->partitions      = parts;
    layout->partition_count = 2u;
    assert(fauxgpt_init(gpt, layout) == FAUXGPT_OK);
}

static void test_gpt_open_format_and_guards(void)
{
    fauxfat_config cfg;
    fauxfat_view view;
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view gpt;
    fauxfat_block_device dev;
    fauxfat_block_opened opened;
    fauxfat_block_probe_info probe;
    memdev media;
    uint64_t p1_first = 2048u;
    uint64_t disk_blocks;
    uint64_t user_probe;
    size_t bytes;
    int rc;

    init_fauxfat(&cfg, &view, p1_first);
    disk_blocks = p1_first + view.volume_blocks + 8192u + 34u;
    setup_gpt(parts, &layout, &gpt, disk_blocks, p1_first,
              view.volume_blocks);

    bytes = (size_t)(disk_blocks * FAUXFAT_BLOCK_SIZE);
    memset(&media, 0, sizeof(media));
    media.blocks = disk_blocks;
    media.data   = (uint8_t *)calloc(1u, bytes);
    assert(media.data != NULL);

    memset(&dev, 0, sizeof(dev));
    dev.block_count = disk_blocks;
    dev.io.read     = mem_read;
    dev.io.write    = mem_write;
    dev.io.zero     = mem_zero;
    dev.io.skip     = mem_skip;
    dev.io.context  = &media;
    dev.flush       = mem_flush;

    user_probe                                       = parts[1].first_lba + 17u;
    media.data[user_probe * FAUXFAT_BLOCK_SIZE + 3u] = 0x5au;

    /* Unknown/blank is not assumed disposable merely because it looks bored. */
    rc = fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                              NULL, NULL, 0u);
    assert(rc == FAUXFAT_BLOCK_ENOTFAUXFAT);
    rc = fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                              NULL, NULL,
                              FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA);
    assert(rc == FAUXFAT_BLOCK_OK);
    assert(media.data[user_probe * FAUXFAT_BLOCK_SIZE + 3u] == 0x5au);

    assert(fauxfat_block_probe(&probe, &dev) == FAUXFAT_BLOCK_OK);
    assert(probe.kind == FAUXFAT_BLOCK_MEDIA_GPT);
    assert(probe.classification == FAUXFAT_VOLUME_FAUXFAT_VALID);
    assert(probe.volume_first_block == p1_first);
    assert(probe.volume_blocks == view.volume_blocks);
    assert((probe.gpt_probe.primary.flags & FAUXGPT_COPY_VALID) != 0u);
    assert((probe.gpt_probe.backup.flags & FAUXGPT_COPY_VALID) != 0u);

    memset(&opened, 0, sizeof(opened));
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, &layout,
                              NULL, NULL, NULL) == FAUXFAT_BLOCK_OK);
    assert(opened.layout == FAUXFAT_BLOCK_GPT);
    assert(opened.volume_first_block == p1_first);
    assert(opened.volume_blocks == view.volume_blocks);
    assert(opened.classification == FAUXFAT_VOLUME_FAUXFAT_VALID);
    assert((opened.gpt.flags & FAUXGPT_INFO_PRIMARY_VALID) != 0u);
    assert((opened.gpt.flags & FAUXGPT_INFO_BACKUP_VALID) != 0u);
    assert((opened.gpt.flags & FAUXGPT_INFO_PMBR_VALID) != 0u);
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_BARE, NULL,
                              NULL, NULL, NULL) == FAUXFAT_BLOCK_EWRAPPER);

    /* Once the target proves it is ours, regeneration needs no destroy flag. */
    assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                NULL, NULL, 0u) == FAUXFAT_BLOCK_OK);
    assert(media.data[user_probe * FAUXFAT_BLOCK_SIZE + 3u] == 0x5au);

    /* Safe reformat proves fauxFAT stable identity, not merely shape. */
    {
        fauxfat_config foreign_cfg = cfg;
        fauxfat_view foreign_view;
        uint64_t writes_before;
        uint64_t zeros_before;

        fill_guid(foreign_cfg.volume_guid, 0xd1u);
        assert(fauxfat_init(&foreign_view, &foreign_cfg) == FAUXFAT_OK);
        assert(fauxfat_block_format(
                   &foreign_view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);

        writes_before = media.write_calls;
        zeros_before  = media.zero_calls;
        assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                    NULL, NULL, 0u) ==
               FAUXFAT_BLOCK_EIDENTITY);
        assert(media.write_calls == writes_before);
        assert(media.zero_calls == zeros_before);
        assert(fauxfat_block_format(
                   &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);

        foreign_cfg = cfg;
        foreign_cfg.volume_serial ^= 0x01020304u;
        assert(fauxfat_init(&foreign_view, &foreign_cfg) == FAUXFAT_OK);
        assert(fauxfat_block_format(
                   &foreign_view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);
        writes_before = media.write_calls;
        zeros_before  = media.zero_calls;
        assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                    NULL, NULL, 0u) ==
               FAUXFAT_BLOCK_EIDENTITY);
        assert(media.write_calls == writes_before);
        assert(media.zero_calls == zeros_before);
        assert(fauxfat_block_format(
                   &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);
    }

    /* Safe GPT format also preserves the disk and partition GUID identities. */
    {
        fauxgpt_partition alt_parts[2];
        fauxgpt_layout alt_layout;
        fauxgpt_view alt_gpt;
        fauxgpt_device gd;
        uint64_t writes_before;
        uint64_t zeros_before;

        memcpy(alt_parts, parts, sizeof(alt_parts));
        alt_layout            = layout;
        alt_layout.partitions = alt_parts;
        fill_guid(alt_layout.disk_guid, 0xc1u);
        assert(fauxgpt_init(&alt_gpt, &alt_layout) == FAUXGPT_OK);
        memset(&gd, 0, sizeof(gd));
        gd.write   = mem_write;
        gd.flush   = mem_flush;
        gd.context = &media;
        assert(fauxgpt_format(&alt_gpt, &gd) == FAUXGPT_OK);

        writes_before = media.write_calls;
        zeros_before  = media.zero_calls;
        assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                    NULL, NULL, 0u) ==
               FAUXFAT_BLOCK_EIDENTITY);
        assert(media.write_calls == writes_before);
        assert(media.zero_calls == zeros_before);
        assert(fauxfat_block_format(
                   &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);

        memcpy(alt_parts, parts, sizeof(alt_parts));
        alt_layout            = layout;
        alt_layout.partitions = alt_parts;
        fill_guid(alt_parts[0].unique_guid, 0xb1u);
        assert(fauxgpt_init(&alt_gpt, &alt_layout) == FAUXGPT_OK);
        assert(fauxgpt_format(&alt_gpt, &gd) == FAUXGPT_OK);
        writes_before = media.write_calls;
        zeros_before  = media.zero_calls;
        assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                    NULL, NULL, 0u) ==
               FAUXFAT_BLOCK_EIDENTITY);
        assert(media.write_calls == writes_before);
        assert(media.zero_calls == zeros_before);
        assert(fauxfat_block_format(
                   &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);
    }

    /* Epoch/labels/names are mutable presentation state, not ownership. */
    {
        fauxfat_config changed_cfg = cfg;
        fauxfat_view changed_view;
        fauxgpt_partition renamed_parts[2];
        fauxgpt_layout renamed_layout;
        fauxgpt_view renamed_gpt;

        changed_cfg.structural_epoch += 1u;
        changed_cfg.volume_label = "UPDATED";
        assert(fauxfat_init(&changed_view, &changed_cfg) == FAUXFAT_OK);
        memcpy(renamed_parts, parts, sizeof(renamed_parts));
        renamed_parts[0].name     = "RENAMED";
        renamed_parts[1].name     = "SCRATCH";
        renamed_layout            = layout;
        renamed_layout.partitions = renamed_parts;
        assert(fauxgpt_init(&renamed_gpt, &renamed_layout) == FAUXGPT_OK);
        assert(fauxfat_block_format(&changed_view, &dev, FAUXFAT_BLOCK_GPT,
                                    &renamed_gpt, NULL, NULL, 0u) ==
               FAUXFAT_BLOCK_OK);
        /* Returning to the old presentation is also identity-safe. */
        assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                    NULL, NULL, 0u) == FAUXFAT_BLOCK_OK);
    }

    /* Change only partition 2 geometry: valid GPT, wrong product layout. */
    {
        fauxgpt_partition alt_parts[2];
        fauxgpt_layout alt_layout;
        fauxgpt_view alt_gpt;
        fauxgpt_device gd;

        memcpy(alt_parts, parts, sizeof(alt_parts));
        alt_parts[1].first_lba += 1u;
        alt_parts[1].block_count -= 1u;
        alt_layout            = layout;
        alt_layout.partitions = alt_parts;
        assert(fauxgpt_init(&alt_gpt, &alt_layout) == FAUXGPT_OK);
        memset(&gd, 0, sizeof(gd));
        gd.write   = mem_write;
        gd.flush   = mem_flush;
        gd.context = &media;
        assert(fauxgpt_format(&alt_gpt, &gd) == FAUXGPT_OK);

        assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, &layout,
                                  NULL, NULL, NULL) ==
               FAUXFAT_BLOCK_EPARTITION);
        assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                    NULL, NULL, 0u) ==
               FAUXFAT_BLOCK_EPARTITION);
        assert(fauxfat_block_format(
                   &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);
    }

    /* Keep GPT geometry valid but remove both fauxFAT OEM identity copies. */
    memset(media.data + (size_t)((p1_first + 9u) * FAUXFAT_BLOCK_SIZE), 0,
           FAUXFAT_BLOCK_SIZE);
    memset(media.data + (size_t)((p1_first + 21u) * FAUXFAT_BLOCK_SIZE), 0,
           FAUXFAT_BLOCK_SIZE);
    assert(fauxfat_block_probe(&probe, &dev) == FAUXFAT_BLOCK_OK);
    assert(probe.kind == FAUXFAT_BLOCK_MEDIA_GPT);
    assert(probe.classification == FAUXFAT_VOLUME_EXFAT_BEST_EFFORT);
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, &layout,
                              NULL, NULL, NULL) ==
           FAUXFAT_BLOCK_ENOTFAUXFAT);
    assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_GPT, &gpt,
                                NULL, NULL, 0u) ==
           FAUXFAT_BLOCK_ENOTFAUXFAT);
    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);

    free(media.data);
}

static void test_probe_damage_conflict_and_io_isolation(void)
{
    fauxfat_config cfg;
    fauxfat_view view;
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view gpt;
    fauxfat_block_device dev;
    fauxfat_block_probe_info probe;
    fauxfat_block_opened opened;
    memdev media;
    uint64_t disk_blocks;
    uint64_t successful_reads;
    size_t bytes;
    int code;

    init_fauxfat(&cfg, &view, 2048u);
    disk_blocks = 2048u + view.volume_blocks + 8192u + 34u;
    setup_gpt(parts, &layout, &gpt, disk_blocks, 2048u,
              view.volume_blocks);

    bytes = (size_t)(disk_blocks * FAUXFAT_BLOCK_SIZE);
    memset(&media, 0, sizeof(media));
    media.blocks = disk_blocks;
    media.data   = (uint8_t *)calloc(1u, bytes);
    assert(media.data != NULL);

    memset(&dev, 0, sizeof(dev));
    dev.block_count = disk_blocks;
    dev.io.read     = mem_read;
    dev.io.write    = mem_write;
    dev.io.zero     = mem_zero;
    dev.io.skip     = mem_skip;
    dev.io.context  = &media;
    dev.flush       = mem_flush;

    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);

    media.read_calls = 0u;
    assert(fauxfat_block_probe(&probe, &dev) == FAUXFAT_BLOCK_OK);
    assert(probe.kind == FAUXFAT_BLOCK_MEDIA_GPT);
    successful_reads = media.read_calls;
    assert(successful_reads != 0u);

    /* Every read in the successful probe path translates backend errno-like
     * values instead of letting -5/-6/-7 masquerade as parser state. */
    for (code = -1; code >= -10; --code) {
        uint64_t fail_at;

        for (fail_at = 1u; fail_at <= successful_reads; ++fail_at) {
            media.read_calls     = 0u;
            media.fail_read_call = fail_at;
            media.fail_read_code = code;
            memset(&probe, 0, sizeof(probe));
            assert(fauxfat_block_probe(&probe, &dev) == FAUXFAT_BLOCK_EIO);
            assert(probe.backend_error == code);
        }
    }
    media.fail_read_call = 0u;
    media.fail_read_code = 0;

    media.read_calls     = 0u;
    media.fail_read_call = 1u;
    media.fail_read_code = FAUXGPT_ENOTGPT;
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, &layout,
                              NULL, NULL, NULL) == FAUXFAT_BLOCK_EIO);
    assert(opened.backend_error == FAUXGPT_ENOTGPT);
    media.fail_read_call = 0u;
    media.fail_read_code = 0;

    /* Observation retains two individually valid but conflicting GPT copies;
     * open policy rejects the ambiguity instead of quietly choosing one. */
    {
        fauxgpt_partition alt_parts[2];
        fauxgpt_layout alt_layout;
        fauxgpt_view alt_gpt;
        uint8_t block[FAUXGPT_BLOCK_SIZE];
        uint64_t lba;

        memcpy(alt_parts, parts, sizeof(alt_parts));
        alt_parts[1].first_lba += 1u;
        alt_parts[1].block_count -= 1u;
        alt_layout            = layout;
        alt_layout.partitions = alt_parts;
        assert(fauxgpt_init(&alt_gpt, &alt_layout) == FAUXGPT_OK);
        for (lba = alt_gpt.backup_array_lba;
             lba <= alt_gpt.backup_header_lba; ++lba) {
            assert(fauxgpt_render_block(&alt_gpt, lba, block) == FAUXGPT_OK);
            memcpy(media.data + (size_t)(lba * FAUXFAT_BLOCK_SIZE), block,
                   FAUXFAT_BLOCK_SIZE);
        }
        assert(fauxfat_block_probe(&probe, &dev) == FAUXFAT_BLOCK_OK);
        assert(probe.kind == FAUXFAT_BLOCK_MEDIA_GPT_CONFLICT);
        assert((probe.gpt_probe.primary.flags & FAUXGPT_COPY_VALID) != 0u);
        assert((probe.gpt_probe.backup.flags & FAUXGPT_COPY_VALID) != 0u);
        assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, NULL,
                                  NULL, NULL, NULL) ==
               FAUXFAT_BLOCK_EWRAPPER);

        {
            fauxgpt_device gd;
            memset(&gd, 0, sizeof(gd));
            gd.write   = mem_write;
            gd.flush   = mem_flush;
            gd.context = &media;
            assert(fauxgpt_format(&gpt, &gd) == FAUXGPT_OK);
        }
    }

    /* Probe is observation rather than policy: it reports >2 active entries;
     * the stricter open API is the thing which refuses that profile. */
    {
        fauxgpt_partition three_parts[3];
        fauxgpt_layout three_layout;
        fauxgpt_view three_gpt;
        fauxgpt_device gd;

        memset(three_parts, 0, sizeof(three_parts));
        memcpy(&three_parts[0], &parts[0], sizeof(parts[0]));
        memcpy(&three_parts[1], &parts[1], sizeof(parts[1]));
        memcpy(three_parts[2].type_guid,
               fauxgpt_type_microsoft_basic_data, 16u);
        fill_guid(three_parts[2].unique_guid, 0xb1u);
        three_parts[1].block_count -= 2048u;
        three_parts[2].first_lba = three_parts[1].first_lba +
                                   three_parts[1].block_count;
        three_parts[2].block_count   = 2048u;
        three_parts[2].name          = "THIRD";
        three_layout                 = layout;
        three_layout.partitions      = three_parts;
        three_layout.partition_count = 3u;
        assert(fauxgpt_init(&three_gpt, &three_layout) == FAUXGPT_OK);
        memset(&gd, 0, sizeof(gd));
        gd.write   = mem_write;
        gd.flush   = mem_flush;
        gd.context = &media;
        assert(fauxgpt_format(&three_gpt, &gd) == FAUXGPT_OK);
        assert(fauxfat_block_probe(&probe, &dev) == FAUXFAT_BLOCK_OK);
        assert(probe.kind == FAUXFAT_BLOCK_MEDIA_GPT);
        assert(probe.gpt.partition_count == 3u);
        assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, NULL,
                                  NULL, NULL, NULL) ==
               FAUXFAT_BLOCK_EPARTITION);
        assert(fauxgpt_format(&gpt, &gd) == FAUXGPT_OK);
    }

    /* GPT markers plus two bad headers are observable damage, not bare media. */
    media.data[FAUXGPT_PRIMARY_HEADER_LBA * FAUXFAT_BLOCK_SIZE + 17u] ^= 0x80u;
    media.data[(disk_blocks - 1u) * FAUXFAT_BLOCK_SIZE + 17u] ^= 0x80u;
    assert(fauxfat_block_probe(&probe, &dev) == FAUXFAT_BLOCK_OK);
    assert(probe.kind == FAUXFAT_BLOCK_MEDIA_GPT_DAMAGED);
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, NULL,
                              NULL, NULL, NULL) == FAUXFAT_BLOCK_EWRAPPER);

    free(media.data);
}

static void test_bare_open_and_format(void)
{
    fauxfat_config cfg;
    fauxfat_view view;
    fauxfat_config foreign_cfg;
    fauxfat_view foreign_view;
    fauxfat_block_device dev;
    fauxfat_block_opened opened;
    memdev media;
    uint64_t writes_before;
    uint64_t zeros_before;
    size_t bytes;

    init_fauxfat(&cfg, &view, 0u);
    bytes = (size_t)(view.volume_blocks * FAUXFAT_BLOCK_SIZE);
    memset(&media, 0, sizeof(media));
    media.blocks = view.volume_blocks;
    media.data   = (uint8_t *)calloc(1u, bytes);
    assert(media.data != NULL);

    memset(&dev, 0, sizeof(dev));
    dev.block_count = media.blocks;
    dev.io.read     = mem_read;
    dev.io.write    = mem_write;
    dev.io.zero     = mem_zero;
    dev.io.skip     = mem_skip;
    dev.io.context  = &media;
    dev.flush       = mem_flush;

    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_BARE, NULL, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_AUTO, NULL,
                              NULL, NULL, NULL) == FAUXFAT_BLOCK_OK);
    assert(opened.layout == FAUXFAT_BLOCK_BARE);
    assert(opened.volume_first_block == 0u);
    assert(opened.volume_blocks == view.volume_blocks);
    assert(opened.classification == FAUXFAT_VOLUME_FAUXFAT_VALID);
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_GPT, NULL,
                              NULL, NULL, NULL) == FAUXFAT_BLOCK_EWRAPPER);
    assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_BARE, NULL,
                                NULL, NULL, 0u) == FAUXFAT_BLOCK_OK);

    foreign_cfg = cfg;
    fill_guid(foreign_cfg.volume_guid, 0xe1u);
    assert(fauxfat_init(&foreign_view, &foreign_cfg) == FAUXFAT_OK);
    assert(fauxfat_block_format(
               &foreign_view, &dev, FAUXFAT_BLOCK_BARE, NULL, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);
    writes_before = media.write_calls;
    zeros_before  = media.zero_calls;
    assert(fauxfat_block_format(&view, &dev, FAUXFAT_BLOCK_BARE, NULL,
                                NULL, NULL, 0u) ==
           FAUXFAT_BLOCK_EIDENTITY);
    assert(media.write_calls == writes_before);
    assert(media.zero_calls == zeros_before);
    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_BARE, NULL, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);

    free(media.data);
}

static void test_gpt_wrapper_repair(void)
{
    fauxfat_config cfg;
    fauxfat_view view;
    fauxgpt_partition parts[2];
    fauxgpt_layout layout;
    fauxgpt_view gpt;
    fauxfat_block_device dev;
    fauxfat_block_opened opened;
    memdev media;
    uint8_t *body_snapshot;
    uint64_t p1_first = 2048u;
    uint64_t disk_blocks;
    uint64_t body_blocks;
    size_t body_bytes;
    size_t bytes;

    init_fauxfat(&cfg, &view, p1_first);
    disk_blocks = p1_first + view.volume_blocks + 8192u + 34u;
    setup_gpt(parts, &layout, &gpt, disk_blocks, p1_first,
              view.volume_blocks);

    bytes = (size_t)(disk_blocks * FAUXFAT_BLOCK_SIZE);
    memset(&media, 0, sizeof(media));
    media.blocks = disk_blocks;
    media.data   = (uint8_t *)calloc(1u, bytes);
    assert(media.data != NULL);

    memset(&dev, 0, sizeof(dev));
    dev.block_count = disk_blocks;
    dev.io.read     = mem_read;
    dev.io.write    = mem_write;
    dev.io.zero     = mem_zero;
    dev.io.skip     = mem_skip;
    dev.io.context  = &media;
    dev.flush       = mem_flush;

    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);

    /* Give the user partition some nonzero dirt and snapshot every usable
     * partition-body byte. Repair is allowed to rewrite wrapper metadata only. */
    media.data[(parts[1].first_lba + 17u) * FAUXFAT_BLOCK_SIZE + 9u] = 0xa5u;
    body_blocks                                                      = gpt.last_usable_lba - p1_first + 1u;
    body_bytes                                                       = (size_t)(body_blocks * FAUXFAT_BLOCK_SIZE);
    body_snapshot                                                    = (uint8_t *)malloc(body_bytes);
    assert(body_snapshot != NULL);
    memcpy(body_snapshot,
           media.data + (size_t)(p1_first * FAUXFAT_BLOCK_SIZE),
           body_bytes);

    /* Repair has no use for the sparse formatter's zero callback. */
    dev.io.zero = NULL;

    /* One bad copy, or only the PMBR, is ordinary wrapper repair. */
    media.data[FAUXGPT_PRIMARY_HEADER_LBA * FAUXFAT_BLOCK_SIZE] ^= 0x01u;
    assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) == FAUXFAT_BLOCK_OK);
    assert(memcmp(body_snapshot,
                  media.data + (size_t)(p1_first * FAUXFAT_BLOCK_SIZE),
                  body_bytes) == 0);

    media.data[gpt.backup_header_lba * FAUXFAT_BLOCK_SIZE] ^= 0x01u;
    assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) == FAUXFAT_BLOCK_OK);
    assert(memcmp(body_snapshot,
                  media.data + (size_t)(p1_first * FAUXFAT_BLOCK_SIZE),
                  body_bytes) == 0);

    media.data[510u] = 0u;
    assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) == FAUXFAT_BLOCK_OK);
    assert(memcmp(body_snapshot,
                  media.data + (size_t)(p1_first * FAUXFAT_BLOCK_SIZE),
                  body_bytes) == 0);

    /* Even with both GPT headers and PMBR unusable, exact p1 identity at the
     * authoritative expected LBA is enough to reconstruct only the wrapper. */
    media.data[510u] = 0u;
    media.data[FAUXGPT_PRIMARY_HEADER_LBA * FAUXFAT_BLOCK_SIZE] ^= 0x01u;
    media.data[gpt.backup_header_lba * FAUXFAT_BLOCK_SIZE] ^= 0x01u;
    assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) == FAUXFAT_BLOCK_OK);
    assert(memcmp(body_snapshot,
                  media.data + (size_t)(p1_first * FAUXFAT_BLOCK_SIZE),
                  body_bytes) == 0);
    assert(fauxfat_block_open(&opened, &dev, FAUXFAT_BLOCK_GPT, &layout,
                              NULL, NULL, NULL) == FAUXFAT_BLOCK_OK);
    assert((opened.gpt.flags & FAUXGPT_INFO_PRIMARY_VALID) != 0u);
    assert((opened.gpt.flags & FAUXGPT_INFO_BACKUP_VALID) != 0u);
    assert((opened.gpt.flags & FAUXGPT_INFO_PMBR_VALID) != 0u);

    /* A valid but different map is not "repairable" without destructive
     * authorization through the formatting API. */
    {
        fauxgpt_partition alt_parts[2];
        fauxgpt_layout alt_layout;
        fauxgpt_view alt_gpt;
        fauxgpt_device gd;
        uint64_t writes_before;

        memcpy(alt_parts, parts, sizeof(alt_parts));
        alt_parts[1].first_lba += 1u;
        alt_parts[1].block_count -= 1u;
        alt_layout            = layout;
        alt_layout.partitions = alt_parts;
        assert(fauxgpt_init(&alt_gpt, &alt_layout) == FAUXGPT_OK);
        memset(&gd, 0, sizeof(gd));
        gd.write   = mem_write;
        gd.flush   = mem_flush;
        gd.context = &media;
        assert(fauxgpt_format(&alt_gpt, &gd) == FAUXGPT_OK);

        writes_before = media.write_calls;
        assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) ==
               FAUXFAT_BLOCK_EPARTITION);
        assert(media.write_calls == writes_before);
        assert(fauxgpt_format(&gpt, &gd) == FAUXGPT_OK);
    }

    /* Stable GPT identity is just as authoritative as geometry here. */
    {
        fauxgpt_layout alt_layout = layout;
        fauxgpt_view alt_gpt;
        fauxgpt_device gd;
        uint64_t writes_before;

        fill_guid(alt_layout.disk_guid, 0xc1u);
        assert(fauxgpt_init(&alt_gpt, &alt_layout) == FAUXGPT_OK);
        memset(&gd, 0, sizeof(gd));
        gd.write   = mem_write;
        gd.flush   = mem_flush;
        gd.context = &media;
        assert(fauxgpt_format(&alt_gpt, &gd) == FAUXGPT_OK);

        writes_before = media.write_calls;
        assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) ==
               FAUXFAT_BLOCK_EIDENTITY);
        assert(media.write_calls == writes_before);
        assert(fauxgpt_format(&gpt, &gd) == FAUXGPT_OK);
    }

    /* Two CRC-valid GPT copies which disagree are ambiguity, even when one is
     * exactly the expected layout. Do not invent merge policy here. */
    {
        fauxgpt_partition alt_parts[2];
        fauxgpt_layout alt_layout;
        fauxgpt_view alt_gpt;
        fauxgpt_device gd;
        uint8_t block[FAUXGPT_BLOCK_SIZE];
        uint64_t lba;
        uint64_t writes_before;

        memcpy(alt_parts, parts, sizeof(alt_parts));
        alt_parts[1].first_lba += 1u;
        alt_parts[1].block_count -= 1u;
        alt_layout            = layout;
        alt_layout.partitions = alt_parts;
        assert(fauxgpt_init(&alt_gpt, &alt_layout) == FAUXGPT_OK);
        for (lba = alt_gpt.backup_array_lba;
             lba <= alt_gpt.backup_header_lba; ++lba) {
            assert(fauxgpt_render_block(&alt_gpt, lba, block) == FAUXGPT_OK);
            memcpy(media.data + (size_t)(lba * FAUXFAT_BLOCK_SIZE), block,
                   FAUXFAT_BLOCK_SIZE);
        }

        writes_before = media.write_calls;
        assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) ==
               FAUXFAT_BLOCK_EWRAPPER);
        assert(media.write_calls == writes_before);

        memset(&gd, 0, sizeof(gd));
        gd.write   = mem_write;
        gd.flush   = mem_flush;
        gd.context = &media;
        assert(fauxgpt_format(&gpt, &gd) == FAUXGPT_OK);
    }

    /* Wrapper reconstruction is not an ownership bypass. */
    {
        fauxfat_config foreign_cfg = cfg;
        fauxfat_view foreign_view;
        uint64_t writes_before;

        fill_guid(foreign_cfg.volume_guid, 0xe1u);
        assert(fauxfat_init(&foreign_view, &foreign_cfg) == FAUXFAT_OK);
        dev.io.zero = mem_zero;
        assert(fauxfat_block_format(
                   &foreign_view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);
        dev.io.zero = NULL;

        writes_before = media.write_calls;
        assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) ==
               FAUXFAT_BLOCK_EIDENTITY);
        assert(media.write_calls == writes_before);

        dev.io.zero = mem_zero;
        assert(fauxfat_block_format(
                   &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
               FAUXFAT_BLOCK_OK);
        dev.io.zero = NULL;
    }

    /* Generic exFAT-looking p1 is still user data, not repair authority. */
    media.data[(p1_first + 9u) * FAUXFAT_BLOCK_SIZE]  = 0u;
    media.data[(p1_first + 21u) * FAUXFAT_BLOCK_SIZE] = 0u;
    {
        uint64_t writes_before = media.write_calls;
        assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) ==
               FAUXFAT_BLOCK_ENOTFAUXFAT);
        assert(media.write_calls == writes_before);
    }

    /* Restore p1, then prove raw backend failures cannot alias policy errors. */
    dev.io.zero = mem_zero;
    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_GPT, &gpt, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_OK);
    dev.io.zero = NULL;

    media.read_calls     = 0u;
    media.fail_read_call = 1u;
    media.fail_read_code = FAUXGPT_ESTRUCTURE;
    assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) ==
           FAUXFAT_BLOCK_EIO);
    media.fail_read_call = 0u;
    media.fail_read_code = 0;

    media.fail_write_code = FAUXGPT_EPARTITIONS;
    assert(fauxfat_block_repair_gpt(&view, &dev, &gpt) ==
           FAUXFAT_BLOCK_EIO);
    media.fail_write_code = 0;

    free(body_snapshot);
    free(media.data);
}

static void test_mutating_callback_error_translation(void)
{
    fauxfat_config cfg;
    fauxfat_view view;
    fauxfat_block_device dev;
    memdev media;
    size_t bytes;

    init_fauxfat(&cfg, &view, 0u);
    bytes = (size_t)(view.volume_blocks * FAUXFAT_BLOCK_SIZE);
    memset(&media, 0, sizeof(media));
    media.blocks = view.volume_blocks;
    media.data   = (uint8_t *)calloc(1u, bytes);
    assert(media.data != NULL);

    memset(&dev, 0, sizeof(dev));
    dev.block_count = media.blocks;
    dev.io.read     = mem_read;
    dev.io.write    = mem_write;
    dev.io.zero     = mem_zero;
    dev.io.skip     = mem_skip;
    dev.io.context  = &media;
    dev.flush       = mem_flush;

    media.fail_write_code = FAUXFAT_ESTRUCTURE;
    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_BARE, NULL, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_EIO);
    media.fail_write_code = 0;

    media.fail_zero_code = FAUXGPT_ESTRUCTURE;
    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_BARE, NULL, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_EIO);
    media.fail_zero_code = 0;

    media.fail_skip_code = FAUXGPT_EPARTITIONS;
    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_BARE, NULL, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_EIO);
    media.fail_skip_code = 0;

    media.fail_flush_code = FAUXGPT_ENOTGPT;
    assert(fauxfat_block_format(
               &view, &dev, FAUXFAT_BLOCK_BARE, NULL, NULL, NULL,
               FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) ==
           FAUXFAT_BLOCK_EIO);

    free(media.data);
}

int main(void)
{
    test_gpt_open_format_and_guards();
    test_probe_damage_conflict_and_io_isolation();
    test_bare_open_and_format();
    test_gpt_wrapper_repair();
    test_mutating_callback_error_translation();
    puts("fauxfat block tests: ok");
    return 0;
}
