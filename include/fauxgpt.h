#ifndef FAUXGPT_H
#define FAUXGPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAUXGPT_BLOCK_SIZE         512u
#define FAUXGPT_ENTRY_SIZE         128u
#define FAUXGPT_ENTRY_COUNT        128u
#define FAUXGPT_ENTRY_ARRAY_BLOCKS 32u
#define FAUXGPT_PRIMARY_HEADER_LBA 1u
#define FAUXGPT_PRIMARY_ARRAY_LBA  2u
#define FAUXGPT_FIRST_USABLE_LBA   34u
#define FAUXGPT_NAME_MAX           36u

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

typedef int (*fauxgpt_dev_write_fn)(void *context,
                                    uint64_t first_block,
                                    size_t block_count,
                                    const void *data);

typedef int (*fauxgpt_dev_flush_fn)(void *context);

typedef struct fauxgpt_device {
    fauxgpt_dev_write_fn write;
    /* Required durability barrier; may be a no-op for synchronous media. */
    fauxgpt_dev_flush_fn flush;
    void *context;
} fauxgpt_device;

enum {
    FAUXGPT_OK        = 0,
    FAUXGPT_EINVAL    = -1,
    FAUXGPT_ERANGE    = -2,
    FAUXGPT_EGEOMETRY = -3,
    /* LBA is valid disk space, but is not GPT metadata rendered here. */
    FAUXGPT_EUNMAPPED = -4
};

/* Validate a fixed GPT layout and precompute its entry-array CRC32. */
int fauxgpt_init(fauxgpt_view *view, const fauxgpt_layout *layout);

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
