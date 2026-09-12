#ifndef FAUXGPT_H
#define FAUXGPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAUXGPT_BLOCK_SIZE          512u
#define FAUXGPT_ENTRY_SIZE          128u
#define FAUXGPT_ENTRY_COUNT         128u
#define FAUXGPT_ENTRY_ARRAY_BLOCKS  32u
#define FAUXGPT_PRIMARY_HEADER_LBA  1u
#define FAUXGPT_PRIMARY_ARRAY_LBA   2u
#define FAUXGPT_FIRST_USABLE_LBA    34u
#define FAUXGPT_NAME_MAX            36u
#define FAUXGPT_OPEN_MAX_PARTITIONS 2u

/* Microsoft Basic Data, encoded in GPT on-disk GUID byte order. */
extern const uint8_t fauxgpt_type_microsoft_basic_data[16];

/*
 * Pass GPT GUIDs as their exact 16 on-disk bytes. This keeps the
 * renderer out of UUID string parsing and byte-order policy.
 */
typedef struct fauxgpt_partition {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t block_count;
    uint64_t attributes;
    const char *name; /* NULL or at most 36 printable ASCII bytes. */
} fauxgpt_partition;

typedef struct fauxgpt_layout {
    uint64_t disk_blocks; /* 512-byte logical blocks. */
    uint8_t disk_guid[16];
    const fauxgpt_partition *partitions;
    size_t partition_count;
} fauxgpt_layout;

typedef struct fauxgpt_view {
    const fauxgpt_layout *layout;
    uint32_t partition_array_crc32;
    uint64_t backup_array_lba;
    uint64_t backup_header_lba;
    uint64_t last_usable_lba;
} fauxgpt_view;

typedef int (*fauxgpt_dev_read_fn)(void *context,
                                   uint64_t first_block,
                                   size_t block_count,
                                   void *data);

typedef int (*fauxgpt_dev_write_fn)(void *context,
                                    uint64_t first_block,
                                    size_t block_count,
                                    const void *data);

typedef int (*fauxgpt_dev_flush_fn)(void *context);

typedef struct fauxgpt_device {
    fauxgpt_dev_write_fn write;
    /* Required by fauxgpt_format(); may be a no-op for synchronous media. */
    fauxgpt_dev_flush_fn flush;
    void *context;
    /* Required by fauxgpt_open(). Appended to preserve old positional init. */
    fauxgpt_dev_read_fn read;
} fauxgpt_device;

/* Parsed active GPT entry. Names are deliberately omitted from safety policy. */
typedef struct fauxgpt_partition_info {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t block_count;
    uint64_t attributes;
} fauxgpt_partition_info;

typedef struct fauxgpt_info {
    uint64_t disk_blocks;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    fauxgpt_partition_info partitions[FAUXGPT_OPEN_MAX_PARTITIONS];
    size_t partition_count;
    unsigned flags;
} fauxgpt_info;

enum {
    FAUXGPT_INFO_PRIMARY_VALID = 1u << 0,
    FAUXGPT_INFO_BACKUP_VALID  = 1u << 1,
    FAUXGPT_INFO_PMBR_VALID    = 1u << 2
};

/* One independently inspected GPT header/entry-array copy. */
typedef struct fauxgpt_copy_info {
    fauxgpt_info info;
    uint32_t partition_array_crc32;
    unsigned flags;
} fauxgpt_copy_info;

enum {
    FAUXGPT_COPY_MARKER   = 1u << 0,
    FAUXGPT_COPY_VALID    = 1u << 1,
    FAUXGPT_COPY_TOO_MANY = 1u << 2
};

/* Raw observation of the bounded GPT wrapper before open policy is applied. */
typedef struct fauxgpt_probe_info {
    fauxgpt_copy_info primary;
    fauxgpt_copy_info backup;
    unsigned flags;
} fauxgpt_probe_info;

enum {
    FAUXGPT_PROBE_PMBR_MARKER = 1u << 0,
    FAUXGPT_PROBE_PMBR_VALID  = 1u << 1
};

enum {
    FAUXGPT_OK        = 0,
    FAUXGPT_EINVAL    = -1,
    FAUXGPT_ERANGE    = -2,
    FAUXGPT_EGEOMETRY = -3,
    /* LBA is valid disk space, but is not GPT metadata rendered here. */
    FAUXGPT_EUNMAPPED = -4,
    /* No protective-MBR/GPT signature was found. */
    FAUXGPT_ENOTGPT = -5,
    /* GPT markers exist, but the bounded primary/backup structure is invalid. */
    FAUXGPT_ESTRUCTURE = -6,
    /* Valid GPT, but it has active entries outside our <=2 partition profile. */
    FAUXGPT_EPARTITIONS = -7
};

/* Validate a fixed GPT layout and precompute its entry-array CRC32. */
int fauxgpt_init(fauxgpt_view *view, const fauxgpt_layout *layout);

/*
 * Inspect primary GPT, backup GPT, and the protective MBR independently.
 * Structural damage is reported in the returned probe state, not as an error.
 * The only failures are bad arguments/geometry or device callback failures.
 * Device callback failures are propagated unchanged by this low-level API.
 */
int fauxgpt_probe(fauxgpt_probe_info *probe,
                  const fauxgpt_device *device,
                  uint64_t disk_blocks);

/*
 * Read and validate the narrow on-disk GPT profile in constant memory.
 *
 * Either the primary or backup copy is sufficient to open a degraded GPT;
 * flags report which copies and whether the protective MBR are valid. If both
 * GPT copies validate they must agree. Active entries must occupy entry 0 and
 * optionally entry 1; later active entries return FAUXGPT_EPARTITIONS.
 */
int fauxgpt_open(fauxgpt_info *info,
                 const fauxgpt_device *device,
                 uint64_t disk_blocks);

/*
 * Compare safety-relevant partition geometry against a desired layout. Disk
 * and partition GUIDs and names are identity/presentation, not geometry, and
 * are intentionally ignored. Type GUID, range, attributes, count, and disk
 * size must match exactly.
 */
int fauxgpt_geometry_matches(const fauxgpt_info *info,
                             const fauxgpt_layout *expected);

/*
 * Compare stable GPT identity only: disk GUID plus every active partition's
 * unique GUID. Geometry/type/name fields are deliberately outside this test;
 * callers which need both invariants should call geometry_matches() first.
 */
int fauxgpt_identity_matches(const fauxgpt_info *info,
                             const fauxgpt_layout *expected);

/* Compatibility spelling retained while callers migrate to the clearer name. */
int fauxgpt_partitioning_matches(const fauxgpt_info *info,
                                 const fauxgpt_layout *expected);

/*
 * Render one whole-disk GPT metadata sector. Partition bodies are deliberately
 * not rendered. A non-metadata LBA returns FAUXGPT_EUNMAPPED.
 */
int fauxgpt_render_block(const fauxgpt_view *view,
                         uint64_t block_address,
                         uint8_t out[FAUXGPT_BLOCK_SIZE]);

/*
 * Materialize only GPT metadata, backup copy first:
 *
 *   backup array, backup header, flush,
 *   primary array, primary header, protective MBR, flush.
 *
 * device->flush is required so the ordering is not decorative fiction.
 *
 * Partition bodies are never touched. Callers should materialize fauxFAT (or
 * any other owned partition content) before publishing a new GPT layout.
 */
int fauxgpt_format(const fauxgpt_view *view, const fauxgpt_device *device);

#ifdef __cplusplus
}
#endif

#endif
