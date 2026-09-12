#ifndef FAUXFAT_H
#define FAUXFAT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAUXFAT_BLOCK_SIZE         512u
#define FAUXFAT_CLUSTER_SIZE       65536u
#define FAUXFAT_BLOCKS_PER_CLUSTER 128u
#define FAUXFAT_MAX_FILES          681u
#define FAUXFAT_NAME_MAX           15u

/* One host-visible file. File data itself is owned by the callback backend. */
typedef struct fauxfat_file {
    /* 1..15 ISO-8859-1 bytes, excluding exFAT-forbidden characters. */
    const char *name;
    int fd;
    uint64_t size;

    /*
     * UTC Unix epoch seconds, using the target libc's native time_t.
     * fauxFAT encodes this as the exFAT create/modify/access timestamp.
     * Valid range is 1980-01-01 through 2107-12-31 inclusive.
     */
    time_t mtime;
} fauxfat_file;

/*
 * File payload I/O. offset and length are always bounded by the corresponding
 * fauxfat_file.size. A callback receives the descriptor from fauxfat_file.fd.
 *
 * Return 0 on success. Any non-zero callback return value is propagated by
 * fauxfat_read_* / fauxfat_write_* unchanged. Callbacks should therefore use
 * their own negative error range if the caller needs to distinguish backend
 * failures from FAUXFAT_E* errors.
 */
typedef int (*fauxfat_read_fn)(void *context,
                               int fd,
                               uint64_t offset,
                               void *data,
                               size_t length);

typedef int (*fauxfat_write_fn)(void *context,
                                int fd,
                                uint64_t offset,
                                const void *data,
                                size_t length);

typedef struct fauxfat_config {
    /* Files are packed contiguously in this exact array order. */
    const fauxfat_file *files;
    size_t file_count;

    /* Payload storage backend. Required when file_count != 0. */
    fauxfat_read_fn read;
    fauxfat_write_fn write;
    void *io_context;

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
    uint32_t upcase_checksum;
    uint8_t map_sha256[32];
    uint32_t boot_checksum;
} fauxfat_view;

/*
 * Translation of one volume-relative disk block into a file access range.
 * length is in 1..512. It can be shorter than a disk block only for the final
 * sector of a file whose DataLength is not sector aligned. Bytes after length
 * are outside the file and must not be passed to the backend.
 */
typedef struct fauxfat_write_mapping {
    size_t file_index;
    int fd;
    uint64_t file_offset;
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

/* No allocation. The config and file table must outlive the view. */
int fauxfat_init(fauxfat_view *view, const fauxfat_config *config);

/* Number of 512-byte blocks in the manufactured exFAT volume. */
uint64_t fauxfat_block_count(const fauxfat_view *view);

/*
 * Render one volume-relative 512-byte block. partition_lba is metadata only;
 * callers presenting a whole disk subtract the partition start before calling.
 * File payload sectors are fetched through config.read(). Metadata sectors are
 * synthesized internally and never touch the backend.
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
 * Translate one volume-relative block write into a bounded public-file range.
 * Metadata blocks and cluster slack beyond a file's DataLength return
 * FAUXFAT_EUNMAPPED. No backend callback is made.
 */
int fauxfat_translate_write(const fauxfat_view *view,
                            uint64_t block_address,
                            fauxfat_write_mapping *mapping);

/*
 * Apply one translated block write through config.write(). For a partial final
 * sector only the bytes inside DataLength are passed to the backend; the
 * sector tail remains synthetic zero data.
 */
int fauxfat_write_block(const fauxfat_view *view,
                        uint64_t block_address,
                        const uint8_t in[FAUXFAT_BLOCK_SIZE]);

/*
 * Apply adjacent block writes. The complete disk mapping is preflighted before
 * the first callback, so an unmapped block causes no backend I/O. Adjacent
 * blocks within one file are coalesced into one callback range. A backend
 * failure after an earlier callback can of course leave earlier writes applied;
 * this is mapping-validation atomicity, not durable transactionality.
 */
int fauxfat_write_blocks(const fauxfat_view *view,
                         uint64_t first_block,
                         size_t block_count,
                         const uint8_t *in);

#ifdef __cplusplus
}
#endif

#endif
