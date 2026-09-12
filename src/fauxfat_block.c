#include "fauxfat_block.h"

#include <string.h>

static int fb_range_ok(uint64_t limit, uint64_t first, uint64_t count)
{
    return first <= limit && count <= limit - first;
}

static int fb_align_up(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t remainder;
    uint64_t add;

    if (!out || alignment == 0u)
        return 0;
    remainder = value % alignment;
    if (remainder == 0u) {
        *out = value;
        return 1;
    }
    add = alignment - remainder;
    if (add > UINT64_MAX - value)
        return 0;
    *out = value + add;
    return 1;
}

int fauxfat_block_plan_gpt(fauxfat_block_gpt_plan *plan,
                           uint64_t disk_blocks,
                           uint64_t fauxfat_block_count,
                           uint64_t card_au_blocks,
                           int include_user_partition)
{
    uint64_t alignment = FAUXFAT_BLOCK_MIN_ALIGNMENT_BLOCKS;
    uint64_t last_usable;
    uint64_t p1_first;
    uint64_t p1_end;
    uint64_t p2_first;

    if (!plan || fauxfat_block_count == 0u)
        return FAUXFAT_BLOCK_EINVAL;
    memset(plan, 0, sizeof(*plan));

    if (card_au_blocks > alignment)
        alignment = card_au_blocks;
    if (disk_blocks < 68u)
        return FAUXFAT_BLOCK_ERANGE;
    last_usable = disk_blocks - FAUXGPT_ENTRY_ARRAY_BLOCKS - 2u;

    if (!fb_align_up(FAUXGPT_FIRST_USABLE_LBA, alignment, &p1_first))
        return FAUXFAT_BLOCK_ERANGE;
    if (p1_first > last_usable ||
        fauxfat_block_count - 1u > last_usable - p1_first)
        return FAUXFAT_BLOCK_ERANGE;
    p1_end = p1_first + fauxfat_block_count - 1u;

    plan->partition_count     = 1u;
    plan->alignment_blocks    = alignment;
    plan->fauxfat_first_lba   = p1_first;
    plan->fauxfat_block_count = fauxfat_block_count;

    if (!include_user_partition)
        return FAUXFAT_BLOCK_OK;
    if (p1_end == UINT64_MAX ||
        !fb_align_up(p1_end + 1u, alignment, &p2_first) ||
        p2_first > last_usable) {
        memset(plan, 0, sizeof(*plan));
        return FAUXFAT_BLOCK_ERANGE;
    }
    plan->partition_count  = 2u;
    plan->user_first_lba   = p2_first;
    plan->user_block_count = last_usable - p2_first + 1u;
    return FAUXFAT_BLOCK_OK;
}

static int fb_translate_backend_error(int *backend_error, int rc)
{
    if (rc == 0)
        return 0;
    if (backend_error)
        *backend_error = rc;
    return FAUXFAT_BLOCK_EIO;
}

static uint64_t fb_capture_generation(const fauxfat_block_device *device)
{
    if (!device->generation)
        return 0u;
    return device->generation(device->io.context);
}

static int fb_check_generation(const fauxfat_block_device *device,
                               uint64_t expected)
{
    if (!device->generation)
        return FAUXFAT_BLOCK_OK;
    return device->generation(device->io.context) == expected ? FAUXFAT_BLOCK_OK : FAUXFAT_BLOCK_ESTALE;
}

static int fb_finish_raw_call(fauxfat_block_opened *opened, int backend_rc)
{
    int stale = fb_check_generation(&opened->raw, opened->media_generation);

    if (backend_rc != 0) {
        opened->backend_error = backend_rc;
        if (stale != FAUXFAT_BLOCK_OK)
            return stale;
        return FAUXFAT_BLOCK_EIO;
    }
    return stale;
}

static int fb_raw_read(void *context, uint64_t first_block,
                       size_t block_count, void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;
    int rc;

    rc = fb_check_generation(&opened->raw, opened->media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (!opened->raw.io.read ||
        !fb_range_ok(opened->raw.block_count, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = opened->raw.io.read(opened->raw.io.context, first_block,
                             block_count, data);
    return fb_finish_raw_call(opened, rc);
}

static int fb_raw_write(void *context, uint64_t first_block,
                        size_t block_count, const void *data)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    uint64_t count               = (uint64_t)block_count;
    int rc;

    rc = fb_check_generation(&opened->raw, opened->media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (!opened->raw.io.write ||
        !fb_range_ok(opened->raw.block_count, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = opened->raw.io.write(opened->raw.io.context, first_block,
                              block_count, data);
    return fb_finish_raw_call(opened, rc);
}

static int fb_raw_zero(fauxfat_block_opened *opened, uint64_t first_block,
                       uint64_t block_count)
{
    int rc;

    rc = fb_check_generation(&opened->raw, opened->media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (!opened->raw.io.zero ||
        !fb_range_ok(opened->raw.block_count, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = opened->raw.io.zero(opened->raw.io.context, first_block, block_count);
    return fb_finish_raw_call(opened, rc);
}

static int fb_raw_skip(fauxfat_block_opened *opened, uint64_t first_block,
                       uint64_t block_count, fauxfat_skip_kind kind)
{
    int rc;

    rc = fb_check_generation(&opened->raw, opened->media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (!fb_range_ok(opened->raw.block_count, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    if (!opened->raw.io.skip)
        return FAUXFAT_BLOCK_OK;
    rc = opened->raw.io.skip(opened->raw.io.context, first_block, block_count,
                             kind);
    return fb_finish_raw_call(opened, rc);
}

static int fb_raw_flush(void *context)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;
    int rc;

    rc = fb_check_generation(&opened->raw, opened->media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (!opened->raw.flush)
        return FAUXFAT_BLOCK_EINVAL;
    rc = opened->raw.flush(opened->raw.io.context);
    return fb_finish_raw_call(opened, rc);
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

    if (!fb_range_ok(opened->volume_blocks, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    return fb_raw_zero(opened, opened->volume_first_block + first_block,
                       block_count);
}

static int fb_volume_skip(void *context, uint64_t first_block,
                          uint64_t block_count, fauxfat_skip_kind kind)
{
    fauxfat_block_opened *opened = (fauxfat_block_opened *)context;

    if (!fb_range_ok(opened->volume_blocks, first_block, block_count))
        return FAUXFAT_BLOCK_ERANGE;
    return fb_raw_skip(opened, opened->volume_first_block + first_block,
                       block_count, kind);
}

static void fb_init_volume(fauxfat_block_opened *opened,
                           const fauxfat_block_device *raw,
                           fauxfat_block_wrapper wrapper,
                           uint64_t first_block, uint64_t block_count,
                           uint64_t media_generation)
{
    memset(opened, 0, sizeof(*opened));
    opened->raw                   = *raw;
    opened->wrapper               = wrapper;
    opened->volume_first_block    = first_block;
    opened->volume_blocks         = block_count;
    opened->media_generation      = media_generation;
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
    uint64_t media_generation;
    int backend_error;
} fb_probe_window;

static int fb_probe_read(void *context, uint64_t first_block,
                         size_t block_count, void *data)
{
    fb_probe_window *window = (fb_probe_window *)context;
    uint64_t count          = (uint64_t)block_count;
    int rc;

    rc = fb_check_generation(window->device, window->media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (!window->device->io.read ||
        !fb_range_ok(window->block_count, first_block, count))
        return FAUXFAT_BLOCK_ERANGE;
    rc = window->device->io.read(window->device->io.context,
                                 window->first_block + first_block,
                                 block_count, data);
    if (fb_check_generation(window->device, window->media_generation) !=
        FAUXFAT_BLOCK_OK) {
        if (rc != 0)
            window->backend_error = rc;
        return FAUXFAT_BLOCK_ESTALE;
    }
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
    if (rc == FAUXFAT_BLOCK_EIO || rc == FAUXFAT_BLOCK_ESTALE)
        return rc;
    if (rc == FAUXFAT_BLOCK_ERANGE || rc == FAUXFAT_EGEOMETRY ||
        rc == FAUXFAT_ESTRUCTURE) {
        probe->classification = FAUXFAT_VOLUME_INVALID;
        memset(&probe->fauxfat, 0, sizeof(probe->fauxfat));
        return fb_check_generation(window->device, window->media_generation);
    }
    if (rc == FAUXFAT_OK)
        return fb_check_generation(window->device, window->media_generation);
    return FAUXFAT_BLOCK_EINVAL;
}

static int fb_block_probe_at_generation(fauxfat_block_probe_info *probe,
                                        const fauxfat_block_device *device,
                                        uint64_t media_generation)
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
    probe->media_generation = media_generation;
    memset(&window, 0, sizeof(window));
    window.device           = device;
    window.block_count      = device->block_count;
    window.media_generation = media_generation;
    if (fb_check_generation(device, media_generation) != FAUXFAT_BLOCK_OK)
        return FAUXFAT_BLOCK_ESTALE;

    if (device->block_count >= 68u) {
        memset(&gd, 0, sizeof(gd));
        gd.read              = fb_probe_read;
        gd.context           = &window;
        rc                   = fauxgpt_probe(&probe->gpt_probe, &gd, device->block_count);
        probe->backend_error = window.backend_error;
        if (rc == FAUXFAT_BLOCK_EIO || rc == FAUXFAT_BLOCK_ESTALE)
            return rc;
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
            return fb_check_generation(device, media_generation);
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
                return fb_check_generation(device, media_generation);
            rc = fb_probe_volume(probe,
                                 chosen->info.partitions[0].first_lba,
                                 chosen->info.partitions[0].block_count,
                                 &window);
            if (rc != FAUXFAT_BLOCK_OK)
                return rc;
            return fb_check_generation(device, media_generation);
        }
        if ((probe->gpt_probe.flags & FAUXGPT_PROBE_PMBR_MARKER) != 0u ||
            (probe->gpt_probe.primary.flags & FAUXGPT_COPY_MARKER) != 0u ||
            (probe->gpt_probe.backup.flags & FAUXGPT_COPY_MARKER) != 0u) {
            probe->kind = FAUXFAT_BLOCK_MEDIA_GPT_DAMAGED;
            return fb_check_generation(device, media_generation);
        }
    }

    rc = fb_probe_volume(probe, 0u, device->block_count, &window);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if ((probe->fauxfat.flags & FAUXFAT_REOPEN_GEOMETRY_VALID) != 0u)
        probe->kind = FAUXFAT_BLOCK_MEDIA_BARE;
    else
        probe->kind = FAUXFAT_BLOCK_MEDIA_UNKNOWN;
    return fb_check_generation(device, media_generation);
}

int fauxfat_block_probe(fauxfat_block_probe_info *probe,
                        const fauxfat_block_device *device)
{
    uint64_t media_generation;

    if (!probe || !device || !device->io.read || device->block_count == 0u)
        return FAUXFAT_BLOCK_EINVAL;
    media_generation = fb_capture_generation(device);
    return fb_block_probe_at_generation(probe, device, media_generation);
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

static int fb_block_open_at_generation(fauxfat_block_opened *opened,
                                       const fauxfat_block_device *device,
                                       fauxfat_block_wrapper expectation,
                                       const fauxgpt_layout *expected_gpt,
                                       fauxfat_file_emit_fn emit,
                                       void *emit_context,
                                       size_t *descriptor_count,
                                       uint64_t media_generation)
{
    fauxfat_block_probe_info probe;
    fauxfat_block_wrapper wrapper;
    uint64_t first_block;
    uint64_t block_count;
    int rc;

    if (!opened || !device || !device->io.read || device->block_count == 0u ||
        expectation < FAUXFAT_BLOCK_WRAPPER_AUTO || expectation > FAUXFAT_BLOCK_WRAPPER_GPT ||
        (expectation == FAUXFAT_BLOCK_WRAPPER_BARE && expected_gpt))
        return FAUXFAT_BLOCK_EINVAL;
    if (descriptor_count)
        *descriptor_count = 0u;
    memset(opened, 0, sizeof(*opened));

    if (expected_gpt && expected_gpt->disk_blocks != device->block_count)
        return FAUXFAT_BLOCK_EPARTITION;

    rc = fb_block_probe_at_generation(&probe, device, media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return fb_open_fail(opened, rc, probe.backend_error);

    if (probe.kind == FAUXFAT_BLOCK_MEDIA_GPT_DAMAGED ||
        probe.kind == FAUXFAT_BLOCK_MEDIA_GPT_CONFLICT)
        return fb_open_fail(opened, FAUXFAT_BLOCK_EWRAPPER, 0);

    if (probe.kind == FAUXFAT_BLOCK_MEDIA_GPT) {
        if (expectation == FAUXFAT_BLOCK_WRAPPER_BARE)
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
            !fauxgpt_geometry_matches(&probe.gpt, expected_gpt))
            return fb_open_fail(opened, FAUXFAT_BLOCK_EPARTITION, 0);
        wrapper     = FAUXFAT_BLOCK_WRAPPER_GPT;
        first_block = probe.gpt.partitions[0].first_lba;
        block_count = probe.gpt.partitions[0].block_count;
    } else {
        if (expectation == FAUXFAT_BLOCK_WRAPPER_GPT)
            return fb_open_fail(opened, FAUXFAT_BLOCK_EWRAPPER, 0);
        wrapper     = FAUXFAT_BLOCK_WRAPPER_BARE;
        first_block = 0u;
        block_count = device->block_count;
    }

    if (probe.classification != FAUXFAT_VOLUME_FAUXFAT_VALID &&
        probe.classification != FAUXFAT_VOLUME_FAUXFAT_CHANGED)
        return fb_open_fail(opened, FAUXFAT_BLOCK_ENOTFAUXFAT, 0);
    if (probe.fauxfat.partition_lba != first_block ||
        probe.fauxfat.volume_blocks != block_count)
        return fb_open_fail(opened, FAUXFAT_BLOCK_EPARTITION, 0);

    fb_init_volume(opened, device, wrapper, first_block, block_count,
                   media_generation);
    opened->classification = probe.classification;
    opened->fauxfat        = probe.fauxfat;
    if (wrapper == FAUXFAT_BLOCK_WRAPPER_GPT)
        opened->gpt = probe.gpt;

    /* Open semantics do not depend on whether the caller asked for output.
     * Run the bounded loose scan once to prove descriptor recoverability; the
     * split reopen path avoids repeating the much heavier validation pass. */
    rc = fauxfat_reopen_scan(&opened->volume_device, &opened->fauxfat,
                             emit, emit_context, descriptor_count);
    if (rc != FAUXFAT_OK)
        return fb_open_fail(opened, rc, opened->backend_error);
    rc = fb_check_generation(device, media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return fb_open_fail(opened, rc, opened->backend_error);
    return FAUXFAT_BLOCK_OK;
}

int fauxfat_block_open(fauxfat_block_opened *opened,
                       const fauxfat_block_device *device,
                       fauxfat_block_wrapper expectation,
                       const fauxgpt_layout *expected_gpt,
                       fauxfat_file_emit_fn emit,
                       void *emit_context,
                       size_t *descriptor_count)
{
    uint64_t media_generation;

    if (!opened || !device || !device->io.read || device->block_count == 0u)
        return FAUXFAT_BLOCK_EINVAL;
    media_generation = fb_capture_generation(device);
    return fb_block_open_at_generation(opened, device, expectation,
                                       expected_gpt, emit, emit_context,
                                       descriptor_count, media_generation);
}

static int fb_gpt_profile_valid(const fauxfat_view *view,
                                const fauxfat_block_device *device,
                                const fauxgpt_view *gpt)
{
    uint64_t volume_blocks;
    size_t i;

    if (!view || !view->config || !device || !gpt || !gpt->layout ||
        device->block_count == 0u ||
        gpt->layout->disk_blocks != device->block_count ||
        gpt->layout->partition_count < 1u ||
        gpt->layout->partition_count > FAUXGPT_OPEN_MAX_PARTITIONS)
        return 0;
    volume_blocks = fauxfat_block_count(view);

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

static int fb_format_profile_valid(const fauxfat_view *view,
                                   const fauxfat_block_device *device,
                                   fauxfat_block_wrapper wrapper,
                                   const fauxgpt_view *gpt)
{
    uint64_t volume_blocks;

    if (!view || !view->config || !device || !device->io.read ||
        !device->io.write || !device->io.zero || !device->flush ||
        device->block_count == 0u)
        return 0;
    volume_blocks = fauxfat_block_count(view);

    if (wrapper == FAUXFAT_BLOCK_WRAPPER_BARE)
        return !gpt && view->config->partition_lba == 0u &&
               volume_blocks == device->block_count;
    if (wrapper != FAUXFAT_BLOCK_WRAPPER_GPT || !device->flush)
        return 0;
    return fb_gpt_profile_valid(view, device, gpt);
}

static int fb_fauxfat_identity_matches(const fauxfat_reopen_info *actual,
                                       const fauxfat_view *expected)
{
    return actual && expected && expected->config &&
           actual->volume_serial == expected->config->volume_serial &&
           memcmp(actual->volume_guid,
                  expected->config->volume_guid, 16u) == 0;
}

static int fb_verify_fauxfat_materialization(const fauxfat_view *view,
                                             fauxfat_block_opened *target)
{
    fauxfat_volume_class classification = FAUXFAT_VOLUME_INVALID;
    int rc;

    rc = fauxfat_validate_strict(view, &target->volume_device, &classification);
    if (rc == FAUXFAT_BLOCK_EIO || rc == FAUXFAT_BLOCK_ESTALE)
        return rc;
    if (rc != FAUXFAT_OK || classification != FAUXFAT_VOLUME_FAUXFAT_VALID)
        return FAUXFAT_BLOCK_EVERIFY;
    return fb_check_generation(&target->raw, target->media_generation);
}

static int fb_verify_gpt_materialization(const fauxgpt_view *gpt,
                                         fauxfat_block_opened *target)
{
    fauxgpt_device gd;
    fauxgpt_info actual;
    unsigned required = FAUXGPT_INFO_PRIMARY_VALID |
                        FAUXGPT_INFO_BACKUP_VALID |
                        FAUXGPT_INFO_PMBR_VALID;
    int rc;

    memset(&gd, 0, sizeof(gd));
    gd.read    = fb_raw_read;
    gd.context = target;

    rc = fauxgpt_verify(gpt, &gd);
    if (rc == FAUXFAT_BLOCK_EIO || rc == FAUXFAT_BLOCK_ESTALE)
        return rc;
    if (rc != FAUXGPT_OK)
        return FAUXFAT_BLOCK_EVERIFY;

    memset(&actual, 0, sizeof(actual));
    rc = fauxgpt_open(&actual, &gd, target->raw.block_count);
    if (rc == FAUXFAT_BLOCK_EIO || rc == FAUXFAT_BLOCK_ESTALE)
        return rc;
    if (rc != FAUXGPT_OK || (actual.flags & required) != required ||
        !fauxgpt_geometry_matches(&actual, gpt->layout) ||
        !fauxgpt_identity_matches(&actual, gpt->layout))
        return FAUXFAT_BLOCK_EVERIFY;

    return fb_check_generation(&target->raw, target->media_generation);
}

static int fb_format_preflight(const fauxfat_view *view,
                               const fauxfat_block_device *device,
                               fauxfat_block_wrapper desired,
                               const fauxgpt_view *gpt,
                               unsigned flags,
                               uint64_t media_generation)
{
    fauxfat_block_opened existing;
    const fauxgpt_layout *expected_gpt = NULL;
    int rc;

    if ((flags & FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA) != 0u)
        return FAUXFAT_BLOCK_OK;
    if (desired == FAUXFAT_BLOCK_WRAPPER_GPT)
        expected_gpt = gpt->layout;
    rc = fb_block_open_at_generation(&existing, device, FAUXFAT_BLOCK_WRAPPER_AUTO,
                                     expected_gpt, NULL, NULL, NULL,
                                     media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (existing.wrapper != desired)
        return FAUXFAT_BLOCK_EWRAPPER;
    if (!fb_fauxfat_identity_matches(&existing.fauxfat, view))
        return FAUXFAT_BLOCK_EIDENTITY;
    if (desired == FAUXFAT_BLOCK_WRAPPER_GPT &&
        !fauxgpt_identity_matches(&existing.gpt, expected_gpt))
        return FAUXFAT_BLOCK_EIDENTITY;
    return FAUXFAT_BLOCK_OK;
}

int fauxfat_block_format(const fauxfat_view *view,
                         const fauxfat_block_device *device,
                         fauxfat_block_wrapper wrapper,
                         const fauxgpt_view *gpt,
                         fauxfat_preserve_fn preserve,
                         void *preserve_context,
                         unsigned flags)
{
    fauxfat_block_opened target;
    fauxgpt_device gd;
    unsigned fauxfat_flags = 0u;
    uint64_t media_generation;
    int rc;

    if ((flags & ~(FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED |
                   FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA)) != 0u ||
        (wrapper != FAUXFAT_BLOCK_WRAPPER_BARE &&
         wrapper != FAUXFAT_BLOCK_WRAPPER_GPT) ||
        !fb_format_profile_valid(view, device, wrapper, gpt))
        return FAUXFAT_BLOCK_EINVAL;

    media_generation = fb_capture_generation(device);
    rc               = fb_check_generation(device, media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;

    rc = fb_format_preflight(view, device, wrapper, gpt, flags,
                             media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;

    if ((flags & FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED) != 0u)
        fauxfat_flags |= FAUXFAT_FORMAT_ZERO_UNDEFINED;

    if (wrapper == FAUXFAT_BLOCK_WRAPPER_GPT) {
        const fauxgpt_partition *p = &gpt->layout->partitions[0];
        fb_init_volume(&target, device, wrapper, p->first_lba, p->block_count,
                       media_generation);
    } else {
        fb_init_volume(&target, device, wrapper, 0u, device->block_count,
                       media_generation);
    }

    rc = fauxfat_format(view, &target.volume_device, preserve,
                        preserve_context, fauxfat_flags);
    if (rc != FAUXFAT_OK)
        return rc;

    if (wrapper == FAUXFAT_BLOCK_WRAPPER_BARE) {
        rc = fb_raw_flush(&target);
        if (rc != FAUXFAT_BLOCK_OK)
            return rc;
        return fb_verify_fauxfat_materialization(view, &target);
    }

    rc = fb_raw_flush(&target);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    memset(&gd, 0, sizeof(gd));
    gd.read    = fb_raw_read;
    gd.write   = fb_raw_write;
    gd.flush   = fb_raw_flush;
    gd.context = &target;
    rc         = fauxgpt_format(gpt, &gd);
    if (rc != FAUXGPT_OK)
        return rc;
    rc = fb_verify_gpt_materialization(gpt, &target);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    return fb_verify_fauxfat_materialization(view, &target);
}

int fauxfat_block_repair_gpt(const fauxfat_view *expected_volume,
                             const fauxfat_block_device *device,
                             const fauxgpt_view *expected_gpt)
{
    const fauxgpt_layout *layout;
    const fauxgpt_partition *p1;
    fauxfat_block_probe_info volume_probe;
    fb_probe_window window;
    fauxfat_block_opened target;
    fauxgpt_device gd;
    fauxgpt_probe_info gpt_probe;
    const fauxgpt_copy_info *copies[2];
    size_t i;
    uint64_t media_generation;
    int primary_valid;
    int backup_valid;
    int rc;

    if (!expected_volume || !expected_volume->config || !device ||
        !device->io.read || !device->io.write || !device->flush ||
        !fb_gpt_profile_valid(expected_volume, device, expected_gpt))
        return FAUXFAT_BLOCK_EINVAL;

    media_generation = fb_capture_generation(device);
    rc               = fb_check_generation(device, media_generation);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;

    layout = expected_gpt->layout;
    p1     = &layout->partitions[0];

    /* Do not trust GPT to locate the authority we are about to repair. */
    memset(&volume_probe, 0, sizeof(volume_probe));
    memset(&window, 0, sizeof(window));
    window.device           = device;
    window.block_count      = device->block_count;
    window.media_generation = media_generation;
    rc                      = fb_probe_volume(&volume_probe, p1->first_lba, p1->block_count,
                                              &window);
    if (rc != FAUXFAT_BLOCK_OK)
        return rc;
    if (volume_probe.classification != FAUXFAT_VOLUME_FAUXFAT_VALID &&
        volume_probe.classification != FAUXFAT_VOLUME_FAUXFAT_CHANGED)
        return FAUXFAT_BLOCK_ENOTFAUXFAT;
    if (volume_probe.fauxfat.partition_lba != p1->first_lba ||
        volume_probe.fauxfat.volume_blocks != p1->block_count)
        return FAUXFAT_BLOCK_EPARTITION;
    if (!fb_fauxfat_identity_matches(&volume_probe.fauxfat, expected_volume))
        return FAUXFAT_BLOCK_EIDENTITY;

    fb_init_volume(&target, device, FAUXFAT_BLOCK_WRAPPER_GPT,
                   p1->first_lba, p1->block_count, media_generation);
    memset(&gd, 0, sizeof(gd));
    gd.read    = fb_raw_read;
    gd.write   = fb_raw_write;
    gd.flush   = fb_raw_flush;
    gd.context = &target;

    memset(&gpt_probe, 0, sizeof(gpt_probe));
    rc = fauxgpt_probe(&gpt_probe, &gd, device->block_count);
    if (rc != FAUXGPT_OK)
        return rc;

    primary_valid = (gpt_probe.primary.flags & FAUXGPT_COPY_VALID) != 0u;
    backup_valid  = (gpt_probe.backup.flags & FAUXGPT_COPY_VALID) != 0u;
    if (primary_valid && backup_valid &&
        !fb_gpt_copies_equal(&gpt_probe.primary, &gpt_probe.backup))
        return FAUXFAT_BLOCK_EWRAPPER;

    copies[0] = &gpt_probe.primary;
    copies[1] = &gpt_probe.backup;
    for (i = 0u; i < 2u; ++i) {
        const fauxgpt_copy_info *copy = copies[i];

        if ((copy->flags & FAUXGPT_COPY_VALID) == 0u)
            continue;
        if ((copy->flags & FAUXGPT_COPY_TOO_MANY) != 0u ||
            copy->info.partition_count > FAUXGPT_OPEN_MAX_PARTITIONS ||
            !fauxgpt_geometry_matches(&copy->info, layout))
            return FAUXFAT_BLOCK_EPARTITION;
        if (!fauxgpt_identity_matches(&copy->info, layout))
            return FAUXFAT_BLOCK_EIDENTITY;
    }

    /* Both copies may be unusable: partition 1 already proved authority. */
    rc = fauxgpt_format(expected_gpt, &gd);
    if (rc != FAUXGPT_OK)
        return rc;
    return fb_verify_gpt_materialization(expected_gpt, &target);
}
