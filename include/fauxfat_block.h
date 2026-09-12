#ifndef FAUXFAT_BLOCK_H
#define FAUXFAT_BLOCK_H

#include "fauxfat.h"
#include "fauxgpt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Physical 512-byte block device/image presented to the integration layer. */
typedef struct fauxfat_block_device {
    uint64_t block_count;
    fauxfat_device io;
    /* Required for GPT formatting; optional for read/open and bare formatting. */
    fauxgpt_dev_flush_fn flush;
} fauxfat_block_device;

typedef enum fauxfat_block_layout {
    /* Open only: detect bare fauxFAT versus our bounded GPT profile. */
    FAUXFAT_BLOCK_AUTO = 0,
    FAUXFAT_BLOCK_BARE = 1,
    FAUXFAT_BLOCK_GPT  = 2
} fauxfat_block_layout;

/*
 * Result of opening a block device. This object owns a shallow copy of the
 * caller's raw callback table and backs volume_device.context, so do not move
 * or copy it after a successful open while volume_device is in use.
 */
typedef struct fauxfat_block_opened {
    fauxfat_block_device raw;
    fauxfat_device volume_device;
    fauxfat_block_layout layout;
    uint64_t volume_first_block;
    uint64_t volume_blocks;
    fauxfat_volume_class classification;
    fauxfat_reopen_info fauxfat;
    fauxgpt_info gpt; /* zero for a bare volume */
} fauxfat_block_opened;

enum {
    FAUXFAT_BLOCK_OK     = 0,
    FAUXFAT_BLOCK_EINVAL = -32,
    FAUXFAT_BLOCK_ERANGE = -33,
    /* Wrapper metadata exists but is malformed or not the requested kind. */
    FAUXFAT_BLOCK_EWRAPPER = -34,
    /* GPT is valid enough to inspect, but its partitioning is not acceptable. */
    FAUXFAT_BLOCK_EPARTITION = -35,
    /* Candidate volume is not recognizably fauxFAT. */
    FAUXFAT_BLOCK_ENOTFAUXFAT = -36
};

enum {
    /* Passed through to fauxfat_format(). */
    FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED = 1u << 0,

    /*
     * Authorize overwriting a target which does not already open as the
     * explicitly requested bare/GPT layout. This includes foreign GPT maps,
     * a 1-2 partition GPT whose first partition lacks the fauxFAT identity,
     * bare/GPT wrapper conversion, and unknown/unformatted media.
     */
    FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA = 1u << 1
};

/*
 * Open a raw block device as fauxFAT.
 *
 * AUTO accepts either a bare fauxFAT volume at LBA 0 or a GPT containing one
 * or two active partitions, with fauxFAT in partition entry 0. GPT does not
 * silently fall back to bare parsing if GPT markers are corrupt or if there
 * are extra partitions.
 *
 * expected_gpt is optional for AUTO/GPT. When set, safety-relevant GPT
 * partitioning (disk size, count, type, ranges, attributes) must match it.
 * GUIDs and names are not part of this geometry check. Without an expected
 * layout, GPT partitions must be zero-attribute Microsoft Basic Data.
 *
 * GPT partition 1 must carry a recognizable fauxFAT OEM identity. A generic
 * exFAT volume is deliberately rejected even if the bounded exFAT scanner
 * could otherwise read it.
 */
int fauxfat_block_open(fauxfat_block_opened *opened,
                       const fauxfat_block_device *device,
                       fauxfat_block_layout expectation,
                       const fauxgpt_layout *expected_gpt,
                       fauxfat_file_emit_fn emit,
                       void *emit_context,
                       size_t *descriptor_count);

/*
 * Format a complete physical target as either BARE or GPT. AUTO is invalid:
 * destructive operations do not get to guess what wrapper the programmer
 * meant.
 *
 * Without DESTROY_USER_DATA, the existing target must already open as the
 * requested wrapper. For GPT it must also match gpt->layout and partition 1
 * must already be recognizably fauxFAT. Unknown/blank media therefore needs
 * explicit destructive authorization too; absence of recognizable metadata is
 * not proof that the device contains nothing valuable.
 *
 * GPT formatting requires gpt, requires partition entry 0 to exactly cover
 * view, materializes fauxFAT first, flushes it, then publishes GPT metadata.
 * Partition entry 1, when present, is never touched by this function.
 */
int fauxfat_block_format(const fauxfat_view *view,
                         const fauxfat_block_device *device,
                         fauxfat_block_layout layout,
                         const fauxgpt_view *gpt,
                         fauxfat_preserve_fn preserve,
                         void *preserve_context,
                         unsigned flags);

#ifdef __cplusplus
}
#endif

#endif
