#include "fauxfat_block.h"

#include <string.h>

static int fb_range_ok(uint64_t limit, uint64_t first, uint64_t count)
{
    return first <= limit && count <= limit - first;
}

static int fb_translate_backend_error(int *backend_error, int rc)
{
    if (rc == 0)
        return 0;
    if (backend_error)
        *backend_error = rc;
    return FAUXFAT_BLOCK_EIO;
}

static int fb_raw_read(void *context, uint64_t first_block,
                       size_t block_count, void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;
    int rc;

    if (!opened->raw.io.read ||
        !fb_range_ok(opened->raw.block_count, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = opened->raw.io.read(opened->raw.io.context, first_block,
                             block_count, data);
    return fb_translate_backend_error(&opened->backend_error, rc);
}

static int fb_raw_write(void *context, uint64_t first_block,
                        size_t block_count, const void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;
    int rc;

    if (!opened->raw.io.write ||
        !fb_range_ok(opened->raw.block_count, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = opened->raw.io.write(opened->raw.io.context, first_block,
                              block_count, data);
    return fb_translate_backend_error(&opened->backend_error, rc);
}

static int fb_raw_flush(void *context)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    int rc;

    if (!opened->raw.flush)
        return FAUXFAT_BLOCK_EINVAL;
    rc = opened->raw.flush(opened->raw.io.context);
    return fb_translate_backend_error(&opened->backend_error, rc);
}

static int fb_volume_read(void *context, uint64_t first_block,
                          size_t block_count, void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;

    if (!fb_range_ok(opened->volume_blocks, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    return fb_raw_read(opened, opened->volume_first_block + first_block,
                       block_count, data);
}

static int fb_volume_write(void *context, uint64_t first_block,
                           size_t block_count, const void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;

    if (!fb_range_ok(opened->volume_blocks, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    return fb_raw_write(opened, opened->volume_first_block + first_block,
                        block_count, data);
}

static int fb_volume_zero(void *context, uint64_t first_block,
                          uint64_t block_count)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    int rc;

    if (!opened->raw.io.zero ||
        !fb_range_ok(opened->volume_blocks, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = opened->raw.io.zero(opened->raw.io.context,
                             opened->volume_first_block + first_block,
                             block_count);
    return fb_translate_backend_error(&opened->backend_error, rc);
}

static int fb_volume_skip(void *context, uint64_t first_block,
                          uint64_t block_count, fauxfat_skip_kind kind)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    int rc;

    if (!fb_range_ok(opened->volume_blocks, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    if (!opened->raw.io.skip)
        return 0;
    rc = opened->raw.io.skip(opened->raw.io.context,
                             opened->volume_first_block + first_block,
                             block_count, kind);
    return fb_translate_backend_error(&opened->backend_error, rc);
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

typedef struct fb_probe_window {
    const fauxfat_block_device *device;
    uint64_t first_block;
    uint64_t block_count;
    int backend_error;
} fb_probe_window;

static int fb_probe_read(void *context, uint64_t first_block,
                         size_t block_count, void *data)
{
    fb_probe_window *window = (fb_probe_window *)context;
    uint64_t count          = (uint64_t)block_count;
    int rc;

    if (!window->device->io.read ||
        !fb_range_ok(window->block_count, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = window->device->io.read(window->device->io.context,
                                 window->first_block + first_block,
                                 block_count, data);
    return fb_translate_backend_error(&window->backend_error, rc);
}

static int fb_partition_equal(const fauxgpt_partition_info *a,
                              const fauxgpt_partition_info *b)
{
    return memcmp(a->type_guid, b->type_guid, 16u) == 0 &&
           memcmp(a->unique_guid, b->unique_guid, 16u) == 0 &&
           a->first_lba == b->first_lba &&
           a->block_count == b->block_count &&
           a->attributes == b->attributes;
}

static int fb_gpt_copies_equal(const fauxgpt_copy_info *a,
                               const fauxgpt_copy_info *b)
{
    size_t i;
    size_t compare_count;

    if (a->partition_array_crc32 != b->partition_array_crc32 ||
        a->info.disk_blocks != b->info.disk_blocks ||
        a->info.first_usable_lba != b->info.first_usable_lba ||
        a->info.last_usable_lba != b->info.last_usable_lba ||
        memcmp(a->info.disk_guid, b->info.disk_guid, 16u) != 0 ||
        a->info.partition_count != b->info.partition_count)
        return 0;

    compare_count = a->info.partition_count;
    if (compare_count > FAUXGPT_OPEN_MAX_PARTITIONS)
        compare_count = FAUXGPT_OPEN_MAX_PARTITIONS;
    for (i = 0u; i < compare_count; ++i) {
        if (!fb_partition_equal(&a->info.partitions[i],
                                &b->info.partitions[i]))
            return 0;
    }
    return 1;
}

static int fb_probe_volume(fauxfat_block_probe_info *probe,
                           uint64_t first_block, uint64_t block_count,
                           fb_probe_window *window)
{
    fauxfat_device volume;
    int rc;

    probe->volume_first_block = first_block;
    probe->volume_blocks      = block_count;
    window->first_block       = first_block;
    window->block_count       = block_count;

    memset(&volume, 0, sizeof(volume));
    volume.read          = fb_probe_read;
    volume.context       = window;
    rc                   = fauxfat_reopen_probe(&volume, &probe->classification,
                                                &probe->fauxfat);
    probe->backend_error = window->backend_error;
    if (rc == FAUXFAT_BLOCK_EIO)
        return FAUXFAT_BLOCK_EIO;
    if (rc == FAUXFAT_BLOCK_ERANGE || rc == FAUXFAT_EGEOMETRY ||
        rc == FAUXFAT_ESTRUCTURE) {
        probe->classification = FAUXFAT_VOLUME_INVALID;
        memset(&probe->fauxfat, 0, sizeof(probe->fauxfat));
        return FAUXFAT_BLOCK_OK;
    }
    if (rc == FAUXFAT_OK)
        return FAUXFAT_BLOCK_OK;
    return FAUXFAT_BLOCK_EINVAL;
}

int fauxfat_block_probe(fauxfat_block_probe_info *probe,
                        const fauxfat_block_device *device)
{
    fb_probe_window window;
    fauxgpt_device gd;
    const fauxgpt_copy_info *chosen = NULL;
    int primary_valid;
    int backup_valid;
    int rc;

    if (!probe || !device || !device->io.read || device->block_count == 0u)
        return FAUXFAT_BLOCK_EINVAL;
    memset(probe, 0, sizeof(*probe));
    memset(&window, 0, sizeof(window));
    window.device      = device;
    window.block_count = device->block_count;

    if (device->block_count >= 68u) {
        memset(&gd, 0, sizeof(gd));
        gd.read              = fb_probe_read;
        gd.context           = &window;
        rc                   = fauxgpt_probe(&probe->gpt_probe, &gd, device->block_count);
        probe->backend_error = window.backend_error;
        if (rc == FAUXFAT_BLOCK_EIO)
            return FAUXFAT_BLOCK_EIO;
        if (rc != FAUXGPT_OK)
            return FAUXFAT_BLOCK_EINVAL;

        primary_valid = (probe->gpt_probe.primary.flags &
                         FAUXGPT_COPY_VALID) != 0u;
        backup_valid  = (probe->gpt_probe.backup.flags &
                        FAUXGPT_COPY_VALID) != 0u;
        if (primary_valid && backup_valid &&
            !fb_gpt_copies_equal(&probe->gpt_probe.primary,
                                 &probe->gpt_probe.backup)) {
            probe->kind = FAUXFAT_BLOCK_MEDIA_GPT_CONFLICT;
            return FAUXFAT_BLOCK_OK;
        }

        if (primary_valid || backup_valid) {
            chosen      = primary_valid ? &probe->gpt_probe.primary : &probe->gpt_probe.backup;
            probe->kind = FAUXFAT_BLOCK_MEDIA_GPT;
            probe->gpt  = chosen->info;
            if (primary_valid)
                probe->gpt.flags |= FAUXGPT_INFO_PRIMARY_VALID;
            if (backup_valid)
                probe->gpt.flags |= FAUXGPT_INFO_BACKUP_VALID;
            if ((probe->gpt_probe.flags & FAUXGPT_PROBE_PMBR_VALID) != 0u)
                probe->gpt.flags |= FAUXGPT_INFO_PMBR_VALID;

            if (chosen->info.partition_count == 0u)
                return FAUXFAT_BLOCK_OK;
            return fb_probe_volume(probe,
                                   chosen->info.partitions[0].first_lba,
                                   chosen->info.partitions[0].block_count,
                                   &window);
        }
        if ((probe->gpt_probe.flags & FAUXGPT_PROBE_PMBR_MARKER) != 0u ||
            (probe->gpt_probe.primary.flags & FAUXGPT_COPY_MARKER) != 0u ||
            (probe->gpt_probe.backup.flags & FAUXGPT_COPY_MARKER) != 0u) {
            probe->kind = FAUXFAT_BLOCK_MEDIA_GPT_DAMAGED;
            return FAUXFAT_BLOCK_OK;
        }
    }

    rc = fb_probe_volume(probe, 0u, device->block_count, &window);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if ((probe->fauxfat.flags & FAUXFAT_REOPEN_GEOMETRY_VALID) != 0u)
        probe->kind = FAUXFAT_BLOCK_MEDIA_BARE;
    else
        probe->kind = FAUXFAT_BLOCK_MEDIA_UNKNOWN;
    return FAUXFAT_BLOCK_OK;
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

static int fb_open_fail(fauxfat_block_opened *opened, int rc,
                        int backend_error)
{
    memset(opened, 0, sizeof(*opened));
    opened->backend_error = backend_error;
    return rc;
}

int fauxfat_block_open(fauxfat_block_opened *opened,
                       const fauxfat_block_device *device,
                       fauxfat_block_layout expectation,
                       const fauxgpt_layout *expected_gpt,
                       fauxfat_file_emit_fn emit,
                       void *emit_context,
                       size_t *descriptor_count)
{
    fauxfat_block_probe_info probe;
    fauxfat_block_layout layout;
    uint64_t first_block;
    uint64_t block_count;
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

    rc = fauxfat_block_probe(&probe, device);
    if (rc != FAUXFAT_BLOCK_OK)
        return fb_open_fail(opened, rc, probe.backend_error);

    if (probe.kind == FAUXFAT_BLOCK_MEDIA_GPT_DAMAGED ||
        probe.kind == FAUXFAT_BLOCK_MEDIA_GPT_CONFLICT)
        return fb_open_fail(opened, FAUXFAT_BLOCK_EWRAPPER, 0);

    if (probe.kind == FAUXFAT_BLOCK_MEDIA_GPT) {
        if (expectation == FAUXFAT_BLOCK_BARE)
            return fb_open_fail(opened, FAUXFAT_BLOCK_EWRAPPER, 0);
        if ((probe.gpt_probe.primary.flags &
             (FAUXGPT_COPY_VALID | FAUXGPT_COPY_TOO_MANY)) ==
                (FAUXGPT_COPY_VALID | FAUXGPT_COPY_TOO_MANY) ||
            (probe.gpt_probe.backup.flags &
             (FAUXGPT_COPY_VALID | FAUXGPT_COPY_TOO_MANY)) ==
                (FAUXGPT_COPY_VALID | FAUXGPT_COPY_TOO_MANY))
            return fb_open_fail(opened, FAUXFAT_BLOCK_EPARTITION, 0);
        if (!fb_basic_data_profile(&probe.gpt))
            return fb_open_fail(opened, FAUXFAT_BLOCK_EPARTITION, 0);
        if (expected_gpt &&
            !fauxgpt_partitioning_matches(&probe.gpt, expected_gpt))
            return fb_open_fail(opened, FAUXFAT_BLOCK_EPARTITION, 0);
        layout      = FAUXFAT_BLOCK_GPT;
        first_block = probe.gpt.partitions[0].first_lba;
        block_count = probe.gpt.partitions[0].block_count;
    } else {
        if (expectation == FAUXFAT_BLOCK_GPT)
            return fb_open_fail(opened, FAUXFAT_BLOCK_EWRAPPER, 0);
        layout      = FAUXFAT_BLOCK_BARE;
        first_block = 0u;
        block_count = device->block_count;
    }

    if (probe.classification != FAUXFAT_VOLUME_FAUXFAT_VALID &&
        probe.classification != FAUXFAT_VOLUME_FAUXFAT_CHANGED)
        return fb_open_fail(opened, FAUXFAT_BLOCK_ENOTFAUXFAT, 0);
    if (probe.fauxfat.partition_lba != first_block ||
        probe.fauxfat.volume_blocks != block_count)
        return fb_open_fail(opened, FAUXFAT_BLOCK_EPARTITION, 0);

    fb_init_volume(opened, device, layout, first_block, block_count);
    opened->classification = probe.classification;
    opened->fauxfat        = probe.fauxfat;
    if (layout == FAUXFAT_BLOCK_GPT)
        opened->gpt = probe.gpt;

    /* Open semantics do not depend on whether the caller asked for output.
     * Run the bounded loose scan once to prove descriptor recoverability; the
     * split reopen path avoids repeating the much heavier validation pass. */
    rc = fauxfat_reopen_scan(&opened->volume_device, &opened->fauxfat,
                             emit, emit_context, descriptor_count);
    if (rc != FAUXFAT_OK)
        return fb_open_fail(opened, rc, opened->backend_error);
    return FAUXFAT_BLOCK_OK;
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
            return fb_raw_flush(&target);
        return FAUXFAT_BLOCK_OK;
    }

    rc = fb_raw_flush(&target);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    memset(&gd, 0, sizeof(gd));
    gd.read    = fb_raw_read;
    gd.write   = fb_raw_write;
    gd.flush   = fb_raw_flush;
    gd.context = &target;
    return fauxgpt_format(gpt, &gd);
}
