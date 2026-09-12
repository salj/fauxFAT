#include "fauxfat_block.h"

#include <string.h>

static int fb_range_ok(uint64_t limit, uint64_t first, uint64_t count)
{
    return first <= limit && count <= limit - first;
}

static int fb_volume_read(void *context, uint64_t first_block,
                          size_t block_count, void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;

    if (!opened->raw.io.read ||
        !fb_range_ok(opened->volume_blocks, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    return opened->raw.io.read(opened->raw.io.context,
                               opened->volume_first_block + first_block,
                               block_count, data);
}

static int fb_volume_write(void *context, uint64_t first_block,
                           size_t block_count, const void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;

    if (!opened->raw.io.write ||
        !fb_range_ok(opened->volume_blocks, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    return opened->raw.io.write(opened->raw.io.context,
                                opened->volume_first_block + first_block,
                                block_count, data);
}

static int fb_volume_zero(void *context, uint64_t first_block,
                          uint64_t block_count)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;

    if (!opened->raw.io.zero ||
        !fb_range_ok(opened->volume_blocks, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    return opened->raw.io.zero(opened->raw.io.context,
                               opened->volume_first_block + first_block,
                               block_count);
}

static int fb_volume_skip(void *context, uint64_t first_block,
                          uint64_t block_count, fauxfat_skip_kind kind)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;

    if (!fb_range_ok(opened->volume_blocks, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    if (!opened->raw.io.skip)
        return 0;
    return opened->raw.io.skip(opened->raw.io.context,
                               opened->volume_first_block + first_block,
                               block_count, kind);
}

static void fb_init_volume(fauxfat_block_opened *opened,
                           const fauxfat_block_device *raw,
                           fauxfat_block_layout layout,
                           uint64_t first_block, uint64_t block_count)
{
    memset(opened, 0, sizeof(*opened));
    opened->raw                   = *raw;
    opened->layout                = layout;
    opened->volume_first_block    = first_block;
    opened->volume_blocks         = block_count;
    opened->volume_device.read    = fb_volume_read;
    opened->volume_device.write   = fb_volume_write;
    opened->volume_device.zero    = fb_volume_zero;
    opened->volume_device.skip    = fb_volume_skip;
    opened->volume_device.context = opened;
}

static int fb_basic_data_profile(const fauxgpt_info *info)
{
    size_t i;

    if (info->partition_count < 1u ||
        info->partition_count > FAUXGPT_OPEN_MAX_PARTITIONS)
        return 0;
    for (i = 0u; i < info->partition_count; ++i) {
        if (memcmp(info->partitions[i].type_guid,
                   fauxgpt_type_microsoft_basic_data, 16u) != 0 ||
            info->partitions[i].attributes != 0u)
            return 0;
    }
    return 1;
}

static int fb_open_fauxfat(fauxfat_block_opened *opened,
                           fauxfat_file_emit_fn emit,
                           void *emit_context,
                           size_t *descriptor_count)
{
    fauxfat_volume_class classification;
    fauxfat_reopen_info info;
    size_t ignored_count = 0u;
    int rc;

    rc = fauxfat_reopen(&opened->volume_device, NULL, NULL, &ignored_count,
                        &classification, &info);
    if (rc != FAUXFAT_OK)
        return rc;
    if (classification != FAUXFAT_VOLUME_FAUXFAT_VALID &&
        classification != FAUXFAT_VOLUME_FAUXFAT_CHANGED)
        return FAUXFAT_BLOCK_ENOTFAUXFAT;
    if (info.partition_lba != opened->volume_first_block ||
        info.volume_blocks != opened->volume_blocks)
        return FAUXFAT_BLOCK_EPARTITION;

    opened->classification = classification;
    opened->fauxfat        = info;
    if (!emit) {
        if (descriptor_count)
            *descriptor_count = ignored_count;
        return FAUXFAT_BLOCK_OK;
    }

    rc = fauxfat_reopen(&opened->volume_device, emit, emit_context,
                        descriptor_count, &classification, NULL);
    if (rc != FAUXFAT_OK)
        return rc;
    return FAUXFAT_BLOCK_OK;
}

static int fb_probe_gpt(const fauxfat_block_device *device, fauxgpt_info *info)
{
    fauxgpt_device gd;

    memset(&gd, 0, sizeof(gd));
    gd.read    = device->io.read;
    gd.context = device->io.context;
    return fauxgpt_open(info, &gd, device->block_count);
}

int fauxfat_block_open(fauxfat_block_opened *opened,
                       const fauxfat_block_device *device,
                       fauxfat_block_layout expectation,
                       const fauxgpt_layout *expected_gpt,
                       fauxfat_file_emit_fn emit,
                       void *emit_context,
                       size_t *descriptor_count)
{
    fauxgpt_info gpt;
    int grc;
    int use_gpt = 0;
    int rc;

    if (!opened || !device || !device->io.read || device->block_count == 0u ||
        expectation < FAUXFAT_BLOCK_AUTO || expectation > FAUXFAT_BLOCK_GPT ||
        (expectation == FAUXFAT_BLOCK_BARE && expected_gpt))
        return FAUXFAT_BLOCK_EINVAL;
    if (descriptor_count)
        *descriptor_count = 0u;
    memset(opened, 0, sizeof(*opened));

    if (expected_gpt && expected_gpt->disk_blocks != device->block_count)
        return FAUXFAT_BLOCK_EPARTITION;

    grc = fb_probe_gpt(device, &gpt);
    if (grc == FAUXGPT_OK) {
        use_gpt = 1;
    } else if (grc == FAUXGPT_EPARTITIONS) {
        return FAUXFAT_BLOCK_EPARTITION;
    } else if (grc == FAUXGPT_ESTRUCTURE) {
        return FAUXFAT_BLOCK_EWRAPPER;
    } else if (grc == FAUXGPT_ENOTGPT || grc == FAUXGPT_EGEOMETRY) {
        use_gpt = 0;
    } else if (grc != FAUXGPT_OK) {
        return grc;
    }

    if (expectation == FAUXFAT_BLOCK_GPT && !use_gpt)
        return FAUXFAT_BLOCK_EWRAPPER;
    if (expectation == FAUXFAT_BLOCK_BARE && use_gpt)
        return FAUXFAT_BLOCK_EWRAPPER;

    if (use_gpt) {
        if (!fb_basic_data_profile(&gpt))
            return FAUXFAT_BLOCK_EPARTITION;
        if (expected_gpt &&
            !fauxgpt_partitioning_matches(&gpt, expected_gpt))
            return FAUXFAT_BLOCK_EPARTITION;
        fb_init_volume(opened, device, FAUXFAT_BLOCK_GPT,
                       gpt.partitions[0].first_lba,
                       gpt.partitions[0].block_count);
        opened->gpt = gpt;
    } else {
        fb_init_volume(opened, device, FAUXFAT_BLOCK_BARE,
                       0u, device->block_count);
    }

    rc = fb_open_fauxfat(opened, emit, emit_context, descriptor_count);
    if (rc != FAUXFAT_BLOCK_OK)
        memset(opened, 0, sizeof(*opened));
    return rc;
}

static int fb_format_profile_valid(const fauxfat_view *view,
                                   const fauxfat_block_device *device,
                                   fauxfat_block_layout layout,
                                   const fauxgpt_view *gpt)
{
    uint64_t volume_blocks;
    size_t i;

    if (!view || !view->config || !device || !device->io.write ||
        !device->io.zero || device->block_count == 0u)
        return 0;
    volume_blocks = fauxfat_block_count(view);

    if (layout == FAUXFAT_BLOCK_BARE)
        return !gpt && view->config->partition_lba == 0u &&
               volume_blocks == device->block_count;
    if (layout != FAUXFAT_BLOCK_GPT || !gpt || !gpt->layout ||
        !device->flush || gpt->layout->disk_blocks != device->block_count ||
        gpt->layout->partition_count < 1u ||
        gpt->layout->partition_count > FAUXGPT_OPEN_MAX_PARTITIONS)
        return 0;

    for (i = 0u; i < gpt->layout->partition_count; ++i) {
        const fauxgpt_partition *p = &gpt->layout->partitions[i];
        if (memcmp(p->type_guid, fauxgpt_type_microsoft_basic_data, 16u) != 0 ||
            p->attributes != 0u)
            return 0;
    }
    return gpt->layout->partitions[0].first_lba ==
               view->config->partition_lba &&
           gpt->layout->partitions[0].block_count == volume_blocks;
}

static int fb_format_preflight(const fauxfat_block_device *device,
                               fauxfat_block_layout desired,
                               const fauxgpt_layout *expected_gpt,
                               unsigned flags)
{
    fauxfat_block_opened existing;
    int rc;

    if ((flags & FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) != 0u)
        return FAUXFAT_BLOCK_OK;
    rc = fauxfat_block_open(&existing, device, FAUXFAT_BLOCK_AUTO,
                            expected_gpt, NULL, NULL, NULL);
    if (rc == FAUXFAT_BLOCK_OK && existing.layout == desired)
        return FAUXFAT_BLOCK_OK;
    if (rc == FAUXFAT_BLOCK_OK)
        return FAUXFAT_BLOCK_EWRAPPER;
    return rc;
}

int fauxfat_block_format(const fauxfat_view *view,
                         const fauxfat_block_device *device,
                         fauxfat_block_layout layout,
                         const fauxgpt_view *gpt,
                         fauxfat_preserve_fn preserve,
                         void *preserve_context,
                         unsigned flags)
{
    fauxfat_block_opened target;
    fauxgpt_device gd;
    const fauxgpt_layout *expected_gpt = NULL;
    unsigned fauxfat_flags             = 0u;
    int rc;

    if ((flags & ~(FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED |
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA)) != 0u ||
        (layout != FAUXFAT_BLOCK_BARE && layout != FAUXFAT_BLOCK_GPT) ||
        !fb_format_profile_valid(view, device, layout, gpt))
        return FAUXFAT_BLOCK_EINVAL;

    if (layout == FAUXFAT_BLOCK_GPT)
        expected_gpt = gpt->layout;
    rc = fb_format_preflight(device, layout, expected_gpt, flags);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;

    if ((flags & FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED) != 0u)
        fauxfat_flags |= FAUXFAT_FORMAT_ZERO_UNDEFINED;

    if (layout == FAUXFAT_BLOCK_GPT) {
        const fauxgpt_partition *p = &gpt->layout->partitions[0];
        fb_init_volume(&target, device, layout, p->first_lba, p->block_count);
    } else {
        fb_init_volume(&target, device, layout, 0u, device->block_count);
    }

    rc = fauxfat_format(view, &target.volume_device, preserve,
                        preserve_context, fauxfat_flags);
    if (rc != FAUXFAT_OK)
        return rc;

    if (layout == FAUXFAT_BLOCK_BARE) {
        if (device->flush)
            return device->flush(device->io.context);
        return FAUXFAT_BLOCK_OK;
    }

    rc = device->flush(device->io.context);
    if (rc != 0)
        return rc;
    memset(&gd, 0, sizeof(gd));
    gd.read    = device->io.read;
    gd.write   = device->io.write;
    gd.flush   = device->flush;
    gd.context = device->io.context;
    return fauxgpt_format(gpt, &gd);
}
