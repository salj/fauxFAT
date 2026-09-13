#ifndef FAUXFAT_BLOCK_H
#define FAUXFAT_BLOCK_H

#include "fauxfat.h"
#include "fauxgpt.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional media-generation fence. Return a stable token for the currently
 * inserted/opened physical medium and change it whenever removal, insertion,
 * reinitialization, or another event invalidates previously opened handles.
 * Equality is the only required property; monotonicity is not required.
 */
typedef uint64_t (*fauxfat_block_generation_fn)(void *context);

/* Physical 512-byte block device/image presented to the integration layer. */
typedef struct fauxfat_block_device {
    uint64_t block_count;
    fauxfat_device io;
    /* Required for whole-device formatting and GPT repair. */
    fauxgpt_dev_flush_fn flush;
    /* Optional. Called with io.context before/after raw I/O to detect swaps. */
    fauxfat_block_generation_fn generation;
} fauxfat_block_device;

typedef enum fauxfat_block_wrapper {
    /* Open only: detect bare fauxFAT versus our bounded GPT profile. */
    FAUXFAT_BLOCK_WRAPPER_AUTO = 0,
    FAUXFAT_BLOCK_WRAPPER_BARE = 1,
    FAUXFAT_BLOCK_WRAPPER_GPT  = 2
} fauxfat_block_wrapper;

#define FAUXFAT_BLOCK_MIN_ALIGNMENT_BLOCKS 2048u

/*
 * Canonical whole-disk GPT geometry for fauxFAT media. GUIDs, names, and
 * filesystem construction remain separate concerns; this is only placement.
 * A zero card_au_blocks input means the SD allocation unit is unavailable.
 */
typedef struct fauxfat_block_gpt_plan {
    unsigned partition_count;
    uint64_t alignment_blocks;
    uint64_t fauxfat_first_lba;
    uint64_t fauxfat_block_count;
    uint64_t user_first_lba;
    uint64_t user_block_count;
} fauxfat_block_gpt_plan;

/*
 * Plan canonical GPT partition placement for a fixed-size fauxFAT partition.
 * Alignment is max(1 MiB, card_au_blocks). Partition 1 starts at the first
 * aligned LBA at or after GPT's first usable LBA. When include_user_partition
 * is non-zero, partition 2 starts at the next aligned LBA after partition 1
 * and consumes the remainder through GPT's last usable LBA.
 *
 * The planner does not grow fauxFAT to consume an alignment gap. Callers that
 * want zero gap should size the fauxFAT anonymous tail before calling this
 * function, then pass the final fauxfat_block_count here.
 */
int fauxfat_block_plan_gpt(fauxfat_block_gpt_plan *plan,
                           uint64_t disk_blocks,
                           uint64_t fauxfat_block_count,
                           uint64_t card_au_blocks,
                           int include_user_partition);

typedef enum fauxfat_block_media_kind {
    FAUXFAT_BLOCK_MEDIA_UNKNOWN = 0,
    FAUXFAT_BLOCK_MEDIA_BARE,
    FAUXFAT_BLOCK_MEDIA_GPT,
    FAUXFAT_BLOCK_MEDIA_GPT_DAMAGED,
    FAUXFAT_BLOCK_MEDIA_GPT_CONFLICT
} fauxfat_block_media_kind;

/*
 * Non-writing observation of a physical target. Structural/policy problems
 * live in this result rather than being collapsed into open() errors.
 * backend_error is meaningful only when fauxfat_block_probe() returns EIO.
 */
typedef struct fauxfat_block_probe_info {
    fauxfat_block_media_kind kind;
    int backend_error;
    /* Captured media-generation token when device.generation is available. */
    uint64_t media_generation;
    fauxgpt_probe_info gpt_probe;
    fauxgpt_info gpt; /* coherent copy when kind == GPT */
    uint64_t volume_first_block;
    uint64_t volume_blocks;
    fauxfat_volume_class classification;
    fauxfat_reopen_info fauxfat;
} fauxfat_block_probe_info;

/*
 * Result of opening a block device. This object owns a shallow copy of the
 * caller's raw callback table and backs volume_device.context, so do not move
 * or copy it after a successful open while volume_device is in use.
 */
typedef struct fauxfat_block_opened {
    fauxfat_block_device raw;
    fauxfat_device volume_device;
    fauxfat_block_wrapper wrapper;
    uint64_t volume_first_block;
    uint64_t volume_blocks;
    fauxfat_volume_class classification;
    fauxfat_reopen_info fauxfat;
    fauxgpt_info gpt; /* zero for a bare volume */
    /* Last raw device callback error translated to FAUXFAT_BLOCK_EIO. */
    int backend_error;
    /* Captured generation; volume callbacks fail ESTALE after replacement. */
    uint64_t media_generation;
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
    FAUXFAT_BLOCK_ENOTFAUXFAT = -36,
    /* A raw read/write/zero/skip/flush callback failed. */
    FAUXFAT_BLOCK_EIO = -37,
    /* Geometry is acceptable, but stable fauxFAT/GPT identity differs. */
    FAUXFAT_BLOCK_EIDENTITY = -38,
    /* Optional media generation changed during or after opening an operation. */
    FAUXFAT_BLOCK_ESTALE = -39,
    /* Post-mutation readback did not match the requested materialization. */
    FAUXFAT_BLOCK_EVERIFY = -40
};

enum {
    /* Passed through to fauxfat_format(). */
    FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED = 1u << 0,

    /*
     * Authorize overwriting a target which does not already open as the
     * explicitly requested bare/GPT layout. This includes foreign GPT maps,
     * a 1-2 partition GPT whose first partition lacks the fauxFAT identity,
     * stable fauxFAT/GPT identity mismatch, bare/GPT wrapper conversion, and
     * unknown/unformatted media.
     */
    FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA = 1u << 1
};

/*
 * Inspect a physical target without applying product layout policy and without
 * writing it. GPT primary/backup copies remain independently visible, and a
 * coherent GPT may still report >2 active partitions or a non-fauxFAT p1.
 *
 * Structural damage is observation, not failure. Only bad API arguments,
 * impossible device geometry, actual I/O failures, or a generation change
 * during the probe return non-zero.
 */
int fauxfat_block_probe(fauxfat_block_probe_info *probe,
                        const fauxfat_block_device *device);

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
 * GPT partition 1 must classify as recognizable, scan-ready fauxFAT. OEM
 * identity bytes alone are deliberately insufficient because a host quick
 * format can replace the filesystem while leaving stale OEM sectors behind.
 * A generic exFAT volume is rejected even if the bounded exFAT scanner could
 * otherwise read it.
 *
 * When device.generation is available, open captures it before probing and the
 * returned volume adapter checks the same token before and after every raw
 * operation. A replacement/reinitialized medium therefore yields ESTALE
 * rather than addressing the new medium through old partition geometry.
 */
int fauxfat_block_open(fauxfat_block_opened *opened,
                       const fauxfat_block_device *device,
                       fauxfat_block_wrapper expectation,
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
 * requested wrapper, match the requested fauxFAT Volume GUID + serial, and,
 * for GPT, match both requested partition geometry and the stable GPT disk /
 * partition GUIDs. Structural epoch, volume label, and GPT names are mutable
 * presentation state and do not participate in ownership checks.
 * Unknown/blank media therefore needs explicit destructive authorization too;
 * absence of recognizable metadata is not proof that the device contains
 * nothing valuable.
 *
 * Formatting always requires device.io.read/write/zero plus device.flush.
 * After the final durability barrier, the whole-device layer reads the result
 * back: the fauxFAT presentation must pass strict validation against `view`,
 * and GPT
 * metadata (when requested) must exactly match the rendered GPT plus reopen as
 * the requested geometry/identity. A successful backend write followed by a
 * mismatching readback returns FAUXFAT_BLOCK_EVERIFY.
 *
 * GPT formatting requires gpt, requires partition entry 0 to exactly cover
 * view, materializes fauxFAT first, flushes it, then publishes GPT metadata.
 * Partition entry 1, when present, is never touched by this function.
 */
int fauxfat_block_format(const fauxfat_view *view,
                         const fauxfat_block_device *device,
                         fauxfat_block_wrapper wrapper,
                         const fauxgpt_view *gpt,
                         fauxfat_preserve_fn preserve,
                         void *preserve_context,
                         unsigned flags);

/*
 * Rebuild only GPT wrapper metadata from an authoritative expected layout.
 * This operation is intrinsically non-destructive: it never writes either
 * partition body and has no DESTROY_USER_DATA escape hatch.
 *
 * Before any write, partition entry 0 is probed directly at the expected LBA
 * (without trusting on-disk GPT) and must prove the requested fauxFAT stable
 * identity and exact PartitionOffset/VolumeLength. Every individually valid
 * GPT copy must also match the requested geometry and stable GPT identity.
 * Two valid but disagreeing GPT copies are treated as ambiguous and refused.
 * If both GPT copies are unusable, a proven partition-1 fauxFAT identity is
 * sufficient authority to reconstruct the wrapper.
 */
int fauxfat_block_repair_gpt(const fauxfat_view *expected_volume,
                             const fauxfat_block_device *device,
                             const fauxgpt_view *expected_gpt);

#ifdef __cplusplus
}
#endif

#endif
