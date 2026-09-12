#ifndef FAUXFAT_H
#define FAUXFAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAUXFAT_BLOCK_SIZE         512u
#define FAUXFAT_CLUSTER_SIZE       65536u
#define FAUXFAT_BLOCKS_PER_CLUSTER 128u
#define FAUXFAT_MAX_FILES          681u
#define FAUXFAT_NAME_MAX           15u

/*
 * One host-visible file. v1 deliberately requires the backing range to be a
 * contiguous writable memory range. The disk extent is ceil(size / 64 KiB)
 * clusters; bytes in the final cluster beyond EOF are rendered as zero.
 */
typedef struct fauxfat_file {
    const char *name;
    uint8_t *data;
    uint64_t size;
} fauxfat_file;

typedef struct fauxfat_config {
    /* Files are packed contiguously in this exact array order. */
    const fauxfat_file *files;
    size_t file_count;

    /* Media LBA of this exFAT partition, written to PartitionOffset. */
    uint64_t partition_lba;

    uint32_t volume_serial;
    uint64_t structural_epoch;

    /* Stable raw/on-disk GUID bytes for the exFAT Volume GUID entry. */
    uint8_t volume_guid[16];

    /* ASCII, at most 11 characters. NULL means "FAUXFAT". */
    const char *volume_label;
} fauxfat_config;

typedef struct fauxfat_view {
    const fauxfat_config *config;

    uint32_t bitmap_clusters;
    uint32_t upcase_cluster;
    uint32_t root_cluster;
    uint32_t data_first_cluster;
    uint32_t cluster_count;

    uint32_t fat_length_blocks;
    uint32_t cluster_heap_block;
    uint64_t volume_blocks;

    uint32_t fat_crc32c;
    uint32_t bitmap_crc32c;
    uint32_t root_crc32c;
    uint8_t map_sha256[32];
    uint32_t boot_checksum;
} fauxfat_view;

/*
 * Translation of one volume-relative disk block into caller-owned file data.
 * length is in 1..512. It can be shorter than a disk block only for the final
 * sector of a file whose DataLength is not sector aligned. Bytes after length
 * are outside the file and must not be written to the backing range.
 */
typedef struct fauxfat_write_mapping {
    size_t file_index;
    uint64_t file_offset;
    uint8_t *data;
    size_t length;
} fauxfat_write_mapping;

enum {
    FAUXFAT_OK        = 0,
    FAUXFAT_EINVAL    = -1,
    FAUXFAT_ERANGE    = -2,
    FAUXFAT_EGEOMETRY = -3,
    /* The disk block is valid fauxFAT, but is not writable file payload. */
    FAUXFAT_EUNMAPPED = -4
};

/* No allocation. The config and file backing ranges must outlive the view. */
int fauxfat_init(fauxfat_view *view, const fauxfat_config *config);

/* Number of 512-byte blocks in the manufactured exFAT volume. */
uint64_t fauxfat_block_count(const fauxfat_view *view);

/*
 * Render one volume-relative 512-byte block. partition_lba is metadata only;
 * callers presenting a whole disk subtract the partition start before calling.
 * The function is deterministic:
 * the same initialized view and backing bytes always produce the same block.
 */
int fauxfat_read_block(const fauxfat_view *view,
                       uint64_t block_address,
                       uint8_t out[FAUXFAT_BLOCK_SIZE]);

/* Convenience wrapper for adjacent blocks. */
int fauxfat_read_blocks(const fauxfat_view *view,
                        uint64_t first_block,
                        size_t block_count,
                        uint8_t *out);

/*
 * Translate one volume-relative block write into a bounded write to a public
 * file backing range. Metadata blocks and cluster slack beyond a file's
 * DataLength return FAUXFAT_EUNMAPPED. No backing data is modified.
 */
int fauxfat_translate_write(const fauxfat_view *view,
                            uint64_t block_address,
                            fauxfat_write_mapping *mapping);

/*
 * Apply one translated block write. For a partial final sector only the bytes
 * inside DataLength are copied; the sector tail remains synthetic zero data.
 */
int fauxfat_write_block(const fauxfat_view *view,
                        uint64_t block_address,
                        const uint8_t in[FAUXFAT_BLOCK_SIZE]);

/*
 * Apply adjacent block writes. The complete range is preflighted before any
 * backing bytes are changed, so an unmapped block cannot cause a partial
 * in-memory update. This is validation atomicity, not durable transactionality.
 */
int fauxfat_write_blocks(const fauxfat_view *view,
                         uint64_t first_block,
                         size_t block_count,
                         const uint8_t *in);

#ifdef __cplusplus
}
#endif

#endif
