# fauxFAT overview and programmer's guide

Status: current design and implemented API; explicit physical placement and recovered-fd binding remain planned.

`fauxFAT` is a deliberately restricted exFAT 1.00 volume generator and block translator. It presents a normal-looking removable filesystem to a consumer OS, but the storage layout is fixed in advance. The host is allowed to overwrite the contents of predefined files. It is not allowed to allocate files, resize them, move them, extend the root directory, or otherwise redesign the volume behind our back.

The point is to use exFAT as a transport-shaped view over storage we control, not to implement a general exFAT filesystem.

The normative byte-level format is in `fauxfat-disk-format.md`. Remaining placement/fd-binding work and qualification tasks are tracked in `fauxfat-format-verify-plan.md`.

## 1. Mental model

A fauxFAT volume contains three kinds of physical space:

| Kind | Host view | fauxFAT behavior |
| --- | --- | --- |
| Public file | Ordinary root file | Host may overwrite existing bytes. Allocation and length are fixed. |
| Opaque range | Not useful as a normal host file | Reserved for private/versioned/raw data and never writable through the public block-write translator. |
| Filesystem structure / padding | exFAT metadata or unused defined space | Generated and validated by fauxFAT. Some padding is explicitly undefined and may be skipped during formatting. |

Every data allocation is contiguous. Public files use exFAT `NoFatChain`; their physical block address is therefore arithmetic from `FirstCluster`. Opaque ranges are also contiguous, but their FAT entries are marked `0xFFFFFFF7` so ordinary allocation logic treats those clusters as unavailable.

The allocation bitmap is saturated: every cluster is already allocated from the moment the volume is created. The one-cluster root directory is also full. Unused root slots contain canonical benign padding entries rather than free entries. In normal operation there is nowhere for the host to create extra files or allocate extra clusters.

This gives the volume a simple contract:

```text
host may change:       bytes inside predefined public files
host may not change:   allocation, file length, file placement, names,
                       root layout, FAT ownership, bitmap, geometry,
                       opaque descriptors, or fauxFAT structural metadata
```

Normal exFAT timestamp/archive changes are tolerated where the format explicitly allows them.

## 2. On-disk structure in one page

fauxFAT v1 fixes the important geometry:

```text
logical block          512 bytes
cluster                64 KiB = 128 blocks
FATs                   1
root directory         exactly 1 cluster
filesystem revision    exFAT 1.00
file names             1..15 ISO-8859-1 bytes
upcase table           128-byte fauxFAT table
```

The volume is laid out as:

```text
main exFAT boot region
backup exFAT boot region
FAT alignment
FAT
cluster heap:
    allocation bitmap
    upcase table
    root directory
    public file extents
    opaque/private extents
    reserve / tail space
```

Public file clusters have FAT value `0` because `NoFatChain` files do not use those entries as a chain. Opaque ranges use `0xFFFFFFF7`. Filesystem metadata clusters use normal fixed EOC/chain values.

A public file uses the ordinary exFAT namespace entries plus two benign fauxFAT identity records:

```text
File
Stream Extension, NoFatChain=1
File Name
Vendor Extension, original-name bytes 0..12
Vendor Extension, original-name bytes 13..14 + length check
```

The two vendor records preserve the manufactured logical name. If a host performs a legal rename, parsers still return the original name and set `FAUXFAT_DISK_FILE_NAME_CHANGED`; whole-volume strict validation still reports the façade as changed. This keeps application-facing identity stable without pretending namespace mutation was authorized.

An opaque range which must be rediscoverable uses a hidden inert file set plus fauxFAT vendor records:

```text
File, hidden/system/read-only
Stream Extension, zero length
File Name, internal $FFxxxxxxxx name
Vendor Extension, carries the logical fauxFAT name
Vendor Allocation, carries the real contiguous private extent
```

The logical opaque name is not the exFAT namespace name. This allows a visible staging file and one or more private A/B or versioned copies to share the same logical name without creating duplicate host-visible file names.

## 3. Structural seal

fauxFAT stores a structural fingerprint in exFAT OEM Parameters.

The main fingerprint is one XXH32 over canonical structural data:

- fixed boot geometry fields;
- meaningful FAT entries;
- meaningful allocation bitmap bytes;
- the exact fauxFAT upcase table;
- the root directory after masking only host-mutable timestamp/archive fields.

Payload bytes are not hashed. A 2 GiB solver file does not cause 2 GiB of hashing. With 64 KiB clusters, the structural stream is roughly 66 KiB per GiB of volume plus the fixed root/geometry overhead.

Additional XXH32 values fingerprint the FAT, bitmap, root, and upcase table separately for diagnostics.

These hashes detect accidental corruption and writers which do not understand fauxFAT. They are not authentication. A hostile writer which can rewrite the disk can also rewrite an unkeyed OEM hash; if authentication is ever needed, it belongs in a keyed MAC or signature outside this mechanism.

## 4. Two interfaces: file backend and block device

The library deliberately separates file contents from the synthetic disk view.

At the top, the caller gives fauxFAT logical files backed by integer descriptors and range callbacks:

```c
typedef struct fauxfat_file {
    const char *name;
    int fd;
    uint64_t size;
    time_t mtime;
} fauxfat_file;
```

The descriptor is opaque to fauxFAT. It can mean an SD extent, NOR object, host file, test object, RPC-backed stream, or anything else the caller can address with `{fd, offset, length}`.

Payload I/O is performed through:

```c
int read(void *ctx, int fd, uint64_t offset, void *dst, size_t length);
int write(void *ctx, int fd, uint64_t offset, const void *src, size_t length);
```

fauxFAT never requires a whole file to be buffered in RAM.

At the bottom, fauxFAT exposes or materializes a 512-byte block device. Filesystem metadata is synthesized internally. A block landing inside a file is translated into the corresponding descriptor/range callback.

This is the important mapping:

```text
host block read
    -> fauxFAT classifies the block
       -> metadata: synthesize 512 bytes
       -> public/opaque payload: read(fd, offset, length)

host block write
    -> fauxFAT classifies the block
       -> public payload: write(fd, offset, length)
       -> anything else: reject as unmapped
```

Opaque payload may be read through the raw block view, but is never writable through the host-facing block-write translator.

## 5. Creating a synthetic view

A minimal caller supplies the public file table, optional opaque range table, and backend callbacks:

```c
static int storage_read(void *ctx, int fd, uint64_t off,
                        void *dst, size_t len)
{
    /* map fd + off to the real backing store */
    return 0;
}

static int storage_write(void *ctx, int fd, uint64_t off,
                         const void *src, size_t len)
{
    /* write only the requested bounded range */
    return 0;
}

static const fauxfat_file files[] = {
    { "SOLVER.DB",  10, 2ULL * 1024 * 1024 * 1024, 1789161600 },
    { "CONFIG.BIN", 11, 64 * 1024,                  1789161600 },
};

static const fauxfat_opaque_file private_files[] = {
    { "SOLVER.DB", 20, 2ULL * 1024 * 1024 * 1024, 1789161600 },
};

fauxfat_config cfg = {
    .files = files,
    .file_count = sizeof(files) / sizeof(files[0]),
    .opaque_files = private_files,
    .opaque_file_count = sizeof(private_files) / sizeof(private_files[0]),
    .read = storage_read,
    .write = storage_write,
    .io_context = storage_context,
    .partition_lba = 2048,
    .volume_serial = 0x12345678,
    .structural_epoch = 1,
    .volume_guid = { /* stable 16-byte on-disk GUID */ },
    .volume_label = "RP UPDATE",
};

fauxfat_view view;
int rc = fauxfat_init(&view, &cfg);
```

`fauxfat_init()` allocates nothing. The configuration and file arrays must outlive the view.

The current implementation packs public files first, then opaque files, in array order. Explicit physical placement is part of the planned layout/parser refactor; it is not implemented yet.

## 6. Serving the view as a block device

The volume size is:

```c
uint64_t blocks = fauxfat_block_count(&view);
```

Read one or more volume-relative sectors with:

```c
fauxfat_read_block(&view, block, sector);
fauxfat_read_blocks(&view, first_block, count, buffer);
```

`partition_lba` is written into the exFAT boot metadata; it is not included in the block address passed to these functions. A whole-disk adapter subtracts the partition start before dispatching to fauxFAT.

Writes use:

```c
fauxfat_write_block(&view, block, sector);
fauxfat_write_blocks(&view, first_block, count, buffer);
```

Only public file payload maps writable. Metadata, opaque/private ranges, and file-allocation slack return `FAUXFAT_EUNMAPPED`.

For a final sector whose `DataLength` is not sector-aligned, fauxFAT writes only the valid prefix. The remainder is outside the file and is not passed to the backend.

`fauxfat_write_blocks()` preflights the full block mapping before issuing the first backend call. This prevents a bad block in the request from causing a partially applied mapping operation. It does not make backend storage transactional: if callback 2 fails after callback 1 succeeded, callback 1 has already happened.

## 7. Direct physical descriptors

The public interchange type for a recognized contiguous allocation is:

```c
typedef struct fauxfat_disk_file {
    char name[FAUXFAT_NAME_MAX + 1];
    fauxfat_disk_file_kind kind;
    uint32_t flags;
    uint64_t first_block;
    uint64_t data_length;
    uint64_t allocation_blocks;
    time_t mtime;
} fauxfat_disk_file;
```

For recognized fauxFAT public files, `name` is the persisted manufactured logical name rather than blindly trusting the mutable namespace entry. `FAUXFAT_DISK_FILE_NAME_CHANGED` reports that the visible exFAT name no longer matches it.

For the current synthetic view:

```c
size_t n = fauxfat_disk_file_count(&view);
fauxfat_describe_disk_file(&view, i, &desc);
```

These descriptors are intentionally physical and small. They contain enough information to:

- bind a recovered file to a caller-owned descriptor;
- map byte offsets directly onto a contiguous block range;
- decide whether an existing payload can be preserved in place during a reformat;
- represent both public and opaque logical files without a general filesystem object model.

The bounded root parser emits this exact type rather than inventing a second parser-only representation.

## 8. Sparse / in-place formatting

`fauxfat_format()` writes a manufactured view to an arbitrary block device without constructing an image in RAM.

The destination callback set is:

```c
read(first_block, block_count, dst)   /* verifier/reopen */
write(first_block, block_count, src)
zero(first_block, block_count)
skip(first_block, block_count, kind)
```

Formatting distinguishes four physical meanings:

| Operation | Meaning |
| --- | --- |
| generated/write | Exact filesystem bytes must be written. |
| zero | Range must read back as zero. |
| skip undefined | fauxFAT does not care what bytes already exist there. |
| skip preserve | Known payload/private data exists there and must not be touched. |

`zero` and `undefined` are not synonyms. On a sparse host file, `zero` may be implemented by a hole only if holes are guaranteed to read as zero. On reused media, stale bytes in a required-zero range must actually be cleared. Undefined ranges may simply be skipped.

Opaque ranges are always preserve-only.

Public payload is normally zeroed on a fresh format. The caller may preserve an exact public allocation in place with the optional callback:

```c
int preserve(void *ctx, const fauxfat_disk_file *wanted);
```

Return greater than zero to preserve the complete allocation, zero to initialize it normally, or a negative value to abort formatting.

This is the intended repair path; strict verification, bounded loose salvage, and schema-free reopen are implemented:

```text
parse / validate existing volume
    -> emit fauxfat_disk_file descriptors
    -> compare desired descriptor with recovered descriptor
    -> preserve exact matching payload ranges
    -> regenerate filesystem metadata around them
```

No payload relocation is implied by reformatting. If a desired object moved or changed size, a higher layer must explicitly copy or rebuild it.

## 9. Validation and reopen model

Strict whole-volume validation against a trusted `fauxfat_view`, the bounded loose scanner, and schema-free block-device reopen are implemented.

With trusted geometry already in a `fauxfat_view`, the implemented bounded pass is:

```c
fauxfat_parse_root_strict(&view, &device, emit_file, ctx, &count);
```

It reads the fixed root a block at a time, emits `fauxfat_disk_file` descriptors directly, accepts only the documented Archive/modify/access metadata churn, and rejects malformed checksums, duplicate public names, opaque-stub collisions, noncanonical entry ordering, overlaps, and out-of-range extents. It does not inspect or follow file FAT chains. This is a root-grammar pass, not yet proof that the boot/FAT/bitmap/OEM seal agrees with it.

Whole-volume strict validation is:

```c
fauxfat_volume_class cls;
fauxfat_validate_strict(&view, &device, &cls);
```

It validates both boot regions and their native exFAT checksums, exact fixed geometry, the fauxFAT OEM records, exact upcase bytes, meaningful saturated bitmap bytes, the strict root grammar, and the complete meaningful FAT map. It recomputes the XXH32 component/map fingerprints from the device and compares them with the OEM seal and the expected view. Payload bytes and explicitly undefined alignment/slack are never read.

Main `VolumeDirty`, stale Backup Boot volatile fields, and the documented File Archive/modify/access fields are canonicalized away. Everything else remains structural. A recognizable fauxFAT OEM identity with a strict mismatch reports `FAUXFAT_CHANGED`; loss of the identity reports `INVALID`. Device read errors are still ordinary errors, not classifications.

Strict validation asks whether the volume is still the fauxFAT structure we manufactured, allowing only metadata changes expected from a normal compliant mount and in-place write cycle. It verifies fixed geometry, boot checksums, the exact upcase table, the saturated bitmap, root layout, public and opaque descriptor sets, FAT classification, exFAT entry checksums, and the OEM XXH32 seals.

Loose validation is a bounded salvage mode. It may return simple root files whose Stream Extension proves they are contiguous `NoFatChain` files, and exact fauxFAT opaque descriptors. It may skip valid objects it does not support. It aborts rather than guessing when the structure is malformed or ambiguous.

The trusted-view entry point is:

```c
fauxfat_scan_loose(&view, &device, emit_file, ctx, &count, &cls);
```

When no schema/view is available, use:

```c
fauxfat_reopen(&device, emit_file, ctx, &count, &cls, &info);
```

`fauxfat_reopen()` derives the supported fixed fauxFAT geometry from the boot sector, validates native boot envelopes/checksums, reads identity from either OEM copy, reconstructs label/GUID/epoch information, recomputes the structural XXH32 seals directly from disk, and emits the same `fauxfat_disk_file` physical descriptors. No file table or manufactured view is needed. One damaged OEM copy remains recognizable `FAUXFAT_CHANGED`; with both identities gone, a sane bounded root can still be returned as `EXFAT_BEST_EFFORT`.

`fauxfat_scan_loose()` uses the trusted view for geometry, then performs the same bounded root scan. Exact fauxFAT public files recover their persisted manufactured name from the two benign vendor records; a live namespace rename sets `FAUXFAT_DISK_FILE_NAME_CHANGED` while leaving `desc.name` stable. Ordinary three-entry contiguous root files are also emitted. Directories, fragmented files, and otherwise well-bounded unsupported File sets are skipped. Bad set checksums, orphan secondaries, impossible counts, overlap/order ambiguity, or unknown critical primaries abort the scan.

The intended classifications are:

```text
INVALID             malformed or ambiguous; descriptors are not trustworthy
EXFAT_BEST_EFFORT   bounded scan found usable simple exFAT files, but no valid fauxFAT identity
FAUXFAT_CHANGED     recognizable fauxFAT, but the strict structural contract was violated
FAUXFAT_VALID       strict fauxFAT structure and seal are intact
```

Even loose mode reports an intact fauxFAT volume as `FAUXFAT_VALID`. Conversely, recovering some simple files from a damaged fauxFAT volume does not turn it back into trusted state; it remains `FAUXFAT_CHANGED`.

The parser is deliberately bounded. It will not grow general exFAT behavior merely because a disk happens to contain it.

Specifically excluded:

- arbitrary FAT-chain walking;
- directory recursion;
- root-directory growth;
- allocator reconstruction;
- orphan recovery;
- filesystem repair;
- TexFAT;
- intent-log or journal replay;
- fragmented-file support in the normal fauxFAT path.

A loose scanner may ignore unsupported entries when their boundaries are unambiguous. If understanding an object requires unbounded filesystem interpretation, fauxFAT ignores it or rejects the volume.

## 10. Recovery policy

The structural seal tells the caller whether the exFAT façade is still structurally trustworthy. It does not decide which application payload generation is authoritative.

That distinction is deliberate.

For example, a product may simultaneously have:

```text
SOLVER.DB public      host-visible staging slot
SOLVER.DB opaque      current private A/B slot
CONFIG.BIN public     host-editable configuration
CONFIG.BIN opaque     private version store
```

If the host changes filesystem structure unexpectedly, strict validation fails. Higher-level authoritative state decides which payloads are still meaningful. Any payload whose exact range can still be positively described may be preserved while fauxFAT regenerates the presentation metadata.

The façade is disposable. The application data is not.

## 11. Current implementation boundary

Implemented now:

- deterministic exFAT v1 geometry and metadata generation;
- public contiguous files;
- persisted original/logical names for public files using two benign Vendor Extension records;
- opaque/private contiguous descriptors and ranges;
- 128-byte ISO-8859-1-oriented exFAT upcase table;
- callback-backed payload reads and writes;
- synthetic 512-byte block view;
- bounded public-file write translation;
- physical descriptor enumeration from a synthetic view;
- strict bounded parsing of the fixed one-cluster root into the same descriptors;
- whole-volume strict validation against a trusted view, including both boot checksums, FAT/bitmap/upcase/root checks, OEM identity, and recomputed XXH32 seals;
- bounded loose root scanning with original-name recovery and rename flagging;
- single structural XXH32 seal plus component XXH32 fingerprints;
- sparse/in-place formatter with generated/zero/undefined/preserve range semantics;
- Unix `time_t` input for file timestamps.

Planned, not implemented yet:

- binding recovered descriptors to caller-backed bounded block/range fds;
- explicit physical placement instead of the current packed layout;
- optional dual-view A/B presentation optimization.

That boundary is important. The library can now manufacture and serve fauxFAT, prove that a volume matches a trusted manufactured view, or reopen the supported fauxFAT geometry directly from a block device with no schema in hand. It still does not grow arbitrary exFAT geometry/traversal, and it does not yet bind recovered descriptors to application fds or let the caller place extents explicitly.

## 12. Design rules worth preserving

The project should keep these rules even as the API evolves:

1. File payload storage is callback-backed; fauxFAT does not own file buffers.
2. Every normal fauxFAT file is contiguous and directly addressable.
3. Metadata parsing remains bounded by fixed structures, especially the one-cluster root.
4. Strict validation accepts only known compliant host mutations.
5. Loose validation may salvage understood objects but never upgrades damaged fauxFAT into trusted state.
6. Reformatting distinguishes generated, zero, undefined, and preserve ranges.
7. The parser and formatter exchange the same `fauxfat_disk_file` descriptor.
8. Structural hashes cover structure, not payload.
9. fauxFAT does not become a general exFAT implementation to accommodate unexpected host behavior.
10. When the host does something structurally incompatible, rebuild the façade from authoritative state rather than trying to repair arbitrary exFAT.
