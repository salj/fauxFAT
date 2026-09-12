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
#define FAUXFAT_MAX_OPAQUE_FILES   408u
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
 * One opaque/private allocation which remains present in the raw block view
 * but is not exposed as ordinary file data.  fauxFAT describes it on disk
 * using a hidden zero-length File entry set with fauxFAT Vendor Extension +
 * Vendor Allocation secondaries.  The logical name is stored in the vendor
 * records, not in the host-visible namespace, so A/B alternatives may use the
 * same logical name.
 *
 * size is the complete preserved allocation and must be a non-zero multiple
 * of FAUXFAT_CLUSTER_SIZE.  Raw block reads are served through fd/read(); raw
 * writes never translate into this range.
 */
typedef struct fauxfat_opaque_file {
    /* 1..15 ISO-8859-1 bytes, same character restrictions as fauxfat_file. */
    const char *name;
    int fd;
    uint64_t size;
    time_t mtime;
} fauxfat_opaque_file;

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
    /*
     * Public files are packed first in array order.  Opaque/private ranges
     * follow, also in array order.  The upcoming formatter/parser refactor
     * will generalize this to explicit physical ranges; this packing rule is
     * retained for the synthetic view API.
     */
    const fauxfat_file *files;
    size_t file_count;
    const fauxfat_opaque_file *opaque_files;
    size_t opaque_file_count;

    /*
     * Payload storage backend. read is required when either table is nonempty;
     * write is required only for host-visible files.
     */
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

/*
 * Direct physical descriptor used by the synthetic view now and by the
 * bounded on-disk parser later.  It is intentionally not a filesystem object
 * model: it says only what contiguous range a recognized logical file owns.
 */
typedef enum fauxfat_disk_file_kind {
    FAUXFAT_DISK_FILE_PUBLIC = 0,
    FAUXFAT_DISK_FILE_OPAQUE = 1
} fauxfat_disk_file_kind;

typedef struct fauxfat_disk_file {
    char name[FAUXFAT_NAME_MAX + 1u];
    fauxfat_disk_file_kind kind;
    uint64_t first_block;       /* volume-relative first 512-byte block */
    uint64_t data_length;       /* logical bytes described by the entry */
    uint64_t allocation_blocks; /* complete contiguous physical allocation */
    time_t mtime;
} fauxfat_disk_file;

/*
 * Raw block-device side of sparse formatting and (later) validation.
 * Addresses are volume-relative 512-byte blocks, matching fauxfat_read_block.
 */
typedef int (*fauxfat_dev_read_fn)(void *context,
                                   uint64_t first_block,
                                   size_t block_count,
                                   void *data);

typedef int (*fauxfat_dev_write_fn)(void *context,
                                    uint64_t first_block,
                                    size_t block_count,
                                    const void *data);

/* After success every block in the range must read as all-zero. */
typedef int (*fauxfat_dev_zero_fn)(void *context,
                                   uint64_t first_block,
                                   uint64_t block_count);

typedef enum fauxfat_skip_kind {
    /* fauxFAT places no condition on existing bytes in this range. */
    FAUXFAT_SKIP_UNDEFINED = 0,
    /* Existing bytes are payload/private state and must remain untouched. */
    FAUXFAT_SKIP_PRESERVE = 1
} fauxfat_skip_kind;

typedef int (*fauxfat_dev_skip_fn)(void *context,
                                   uint64_t first_block,
                                   uint64_t block_count,
                                   fauxfat_skip_kind kind);

typedef struct fauxfat_device {
    fauxfat_dev_read_fn read;
    fauxfat_dev_write_fn write;
    fauxfat_dev_zero_fn zero;
    fauxfat_dev_skip_fn skip;
    void *context;
} fauxfat_device;

/*
 * Called once for each public allocation before fresh-format zeroing.
 * >0 preserves the complete exact allocation, 0 initializes it normally,
 * <0 aborts and is propagated unchanged. Opaque allocations are always
 * preserve ranges and do not need this callback.
 */
typedef int (*fauxfat_preserve_fn)(void *context,
                                   const fauxfat_disk_file *wanted);

enum {
    /* Stronger reproducibility mode: zero ranges normally left undefined. */
    FAUXFAT_FORMAT_ZERO_UNDEFINED = 1u << 0
};

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

/* Public descriptors first, then opaque descriptors, matching canonical root order. */
size_t fauxfat_disk_file_count(const fauxfat_view *view);
int fauxfat_describe_disk_file(const fauxfat_view *view,
                               size_t index,
                               fauxfat_disk_file *out);

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

/*
 * Materialize the manufactured filesystem into an arbitrary block device
 * without constructing an image in RAM. Structural blocks are generated one
 * block at a time; public payload is zeroed in ranges; undefined and opaque
 * allocations are reported as skip ranges. A preserve callback can convert
 * an exact public allocation from zeroing to preserve-in-place.
 *
 * device.write and device.zero are required. device.skip is optional; when
 * omitted, skipped ranges simply cause no callback. device.read is unused by
 * formatting and exists for the verifier/reopen API which shares this device
 * description.
 */
int fauxfat_format(const fauxfat_view *view,
                   const fauxfat_device *device,
                   fauxfat_preserve_fn preserve,
                   void *preserve_context,
                   unsigned flags);

#ifdef __cplusplus
}
#endif

#endif
