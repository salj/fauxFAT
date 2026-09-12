# fauxFAT programmer's guide

This guide covers `fauxfat`, `fauxfat_block`, and `fauxgpt`. It shows how to build, validate, recover, and format a volume. Application transaction policy belongs above this library.

## 1. Build a manufactured view

A manufactured view starts with public files, optional named opaque files, payload callbacks, and stable volume identity.

```c
#include "fauxfat.h"

static int payload_read(void *ctx, int fd, uint64_t offset,
                        void *dst, size_t length)
{
    /* Translate fd + offset into the application's backing store. */
    return 0;
}

static int payload_write(void *ctx, int fd, uint64_t offset,
                         const void *src, size_t length)
{
    /* Write exactly this bounded range. */
    return 0;
}

static const fauxfat_file public_files[] = {
    {
        .name = "PAYLOAD.BIN",
        .fd = 1,
        .size = 16ULL * 1024 * 1024,
        .mtime = (time_t)1789161600,
        .data_cluster = 0,
    },
    {
        .name = "EDIT.CFG",
        .fd = 2,
        .size = 64ULL * 1024,
        .mtime = (time_t)1789161600,
        .data_cluster = FAUXFAT_CLUSTER_AUTO,
    },
};

static const fauxfat_opaque_file private_files[] = {
    {
        .name = "PAYLOAD.BIN",
        .fd = 20,
        .size = 16ULL * 1024 * 1024,
        .mtime = (time_t)1789161600,
        .data_cluster = 300,
    },
};

static const fauxfat_config cfg = {
    .files = public_files,
    .file_count = sizeof(public_files) / sizeof(public_files[0]),
    .opaque_files = private_files,
    .opaque_file_count = sizeof(private_files) / sizeof(private_files[0]),

    /* Leave additional anonymous private capacity after the named extents. */
    .data_cluster_count = 700,

    .read = payload_read,
    .write = payload_write,
    .io_context = NULL,

    /* Written into exFAT metadata; block APIs remain volume-relative. */
    .partition_lba = 2048,
    .volume_serial = 0x12345678,
    .structural_epoch = 1,
    .volume_guid = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    },
    .volume_label = "TRANSFER",
};

fauxfat_view view;
int rc = fauxfat_init(&view, &cfg);
```

`fauxfat_init()` performs no allocation. `cfg`, both file arrays, their names, and `io_context` must remain valid for the lifetime of `view`.

Important configuration rules:

- public and opaque names are 1..15 ISO-8859-1 bytes and must obey the fauxFAT/exFAT restrictions;
- public file sizes must be non-zero;
- opaque sizes must be non-zero multiples of 64 KiB;
- `mtime` is UTC Unix time and must fit the exFAT 1980..2107 range;
- public names are case-folded for collision checks;
- explicit `data_cluster` values are offsets inside the payload arena and must be monotonically ordered/non-overlapping across the public table followed by the opaque table;
- `FAUXFAT_CLUSTER_AUTO` packs the entry immediately after the previous configured extent;
- `data_cluster_count == 0` ends the arena at the last configured extent; a larger value reserves an anonymous opaque tail;
- `read` is required when any public or opaque object exists;
- `write` is required when any public file exists;
- `volume_guid` must be non-zero;
- `volume_label` is at most 11 printable ASCII bytes; `NULL` uses the library default `FAUXFAT`.

The public and opaque tables are intentionally separate because root order is fixed: public descriptors first, opaque descriptors second.

## 2. Serve the synthetic block view

The view exposes a volume-relative 512-byte block device.

```c
uint64_t nblocks = fauxfat_block_count(&view);
uint8_t sector[FAUXFAT_BLOCK_SIZE];

rc = fauxfat_read_block(&view, block, sector);
rc = fauxfat_read_blocks(&view, first_block, count, buffer);
```

`partition_lba` is metadata only. If a whole-disk adapter presents this volume at LBA 2048, subtract 2048 before calling fauxFAT block functions. The companion `fauxgpt` API renders and writes whole-disk GPT metadata; the product-level adapter still owns LBA routing between GPT, fauxFAT partition 1, and the untouched user partition.

Metadata blocks are synthesized. Public and named opaque payload reads are forwarded to `cfg.read` with the corresponding `{fd, offset, length}`. Reads of anonymous reserved space and structurally undefined bytes synthesize zero in the view even though an in-place formatter is not required to zero those physical bytes.

### Host block writes

```c
rc = fauxfat_write_block(&view, block, sector);
rc = fauxfat_write_blocks(&view, first_block, count, buffer);
```

Only bytes inside public `DataLength` map to `cfg.write`. Metadata, opaque ranges, anonymous reserve, and public allocation slack return `FAUXFAT_EUNMAPPED`.

`fauxfat_write_blocks()` validates the full block mapping before issuing the first payload callback. This prevents an unmapped block later in the request from causing partial mapping-level application. It does not turn multiple backend callbacks into a durable transaction.

For the final sector of a non-sector-aligned public file, only the valid prefix is sent to the payload backend.

If a caller needs to inspect the translation without performing a write:

```c
fauxfat_write_mapping m;
rc = fauxfat_translate_write(&view, block, &m);
```

On success `m` gives the public file index, fd, byte offset, and valid byte count for that sector.

## 3. Enumerate physical descriptors from a view

A manufactured view can describe every named public or opaque object as a physical contiguous range:

```c
for (size_t i = 0; i < fauxfat_disk_file_count(&view); ++i) {
    fauxfat_disk_file d;
    rc = fauxfat_describe_disk_file(&view, i, &d);
    if (rc != FAUXFAT_OK)
        break;

    /* d.first_block and d.allocation_blocks are volume-relative. */
}
```

Public descriptors come first, followed by opaque descriptors.

`d.data_length` is the logical data length. `d.allocation_blocks` is the complete cluster-rounded physical allocation. They are deliberately different for public files whose size is not cluster-aligned.

## 4. Materialize or regenerate a block device

`fauxfat_format()` writes the presentation without allocating an image buffer.

```c
static int dev_write(void *ctx, uint64_t first_block,
                     size_t block_count, const void *src)
{
    return 0;
}

static int dev_zero(void *ctx, uint64_t first_block,
                    uint64_t block_count)
{
    /* Must guarantee zero-on-read after success. */
    return 0;
}

static int dev_skip(void *ctx, uint64_t first_block,
                    uint64_t block_count, fauxfat_skip_kind kind)
{
    /* Optional: seek, log, assert preservation policy, etc. */
    return 0;
}

fauxfat_device device = {
    .read = NULL,       /* formatting itself does not require read */
    .write = dev_write,
    .zero = dev_zero,
    .skip = dev_skip,   /* optional */
    .context = storage,
};

rc = fauxfat_format(&view, &device, NULL, NULL, 0);
```

`device.write` and `device.zero` are required by formatting. `device.skip` is optional.

The formatter distinguishes:

```text
generated       exact metadata bytes are written
zero            range must become zero
undefined       no condition; omitted unless ZERO_UNDEFINED is requested
preserve        must not be modified
```

Named opaque ranges and anonymous reserve are always preserve ranges. Fresh public payload is zeroed through its logical data length; allocation slack is undefined.

For a deterministic full image, including otherwise undefined bytes:

```c
rc = fauxfat_format(&view, &device, NULL, NULL,
                    FAUXFAT_FORMAT_ZERO_UNDEFINED);
```

Preserve ranges are still never zeroed by this flag.

### Sparse files

A host-file backend may implement `zero()` by creating/preserving sparse holes only if a hole is guaranteed to read as zero. `skip(UNDEFINED)` can simply seek past bytes. `skip(PRESERVE)` must retain the existing range.

On reused physical media, do not implement `zero()` as a no-op merely because no bytes need to be stored in a sparse-image representation. fauxFAT distinguishes zero from undefined precisely to avoid that ambiguity.

## 5. Preserve known payloads during regeneration

The formatter can preserve an exact public allocation:

```c
static int preserve_public(void *ctx, const fauxfat_disk_file *wanted)
{
    const recovered_table *r = ctx;

    if (have_exact_recovered_match(r, wanted))
        return 1;       /* preserve */
    return 0;           /* initialize normally */
}

rc = fauxfat_format(&view, &device,
                    preserve_public, &recovered, 0);
```

The callback receives the desired descriptor. fauxFAT does not retain a recovered-file database or guess whether an old allocation is semantically current. The application compares against descriptors it recovered or against its own authoritative state.

A preserved public object must already occupy exactly the desired physical allocation. Formatting does not relocate bytes.

Opaque ranges do not use the preserve callback; they are preserve-only by definition.

## 6. Strict root parsing with trusted geometry

When a manufactured `fauxfat_view` is already available, the fixed root can be parsed without validating the rest of the volume:

```c
static int emit_file(void *ctx, unsigned index,
                     const fauxfat_disk_file *file)
{
    /* Consume immediately, copy into an open-handle slot, or compare. */
    return 0;
}

size_t count;
rc = fauxfat_parse_root_strict(&view, &device,
                               emit_file, ctx, &count);
```

This function validates only the fixed fauxFAT root grammar and its entry-set checksums/range relationships. Use `fauxfat_validate_strict()` when the question is whether the complete disk presentation remains valid.

The parser tolerates only the documented host-mutable Archive and modify/access timestamp fields. A public namespace rename can still be identified from the persisted logical-name vendor records, but strict whole-volume validation treats the rename as a structural change.

## 7. Strict whole-volume validation

With a trusted manufactured view:

```c
fauxfat_volume_class cls;
rc = fauxfat_validate_strict(&view, &device, &cls);
```

`device.read` is required.

A structural mismatch is normally returned through `cls` while the function itself returns `FAUXFAT_OK`. Device callback failures are propagated as errors.

Interpretation:

```text
FAUXFAT_VOLUME_FAUXFAT_VALID
    complete deterministic presentation still matches the view

FAUXFAT_VOLUME_FAUXFAT_CHANGED
    fauxFAT identity is recognizable, but strict structure changed

FAUXFAT_VOLUME_INVALID
    no usable strict fauxFAT identity/structure was established
```

`fauxfat_validate_strict()` does not emit recovered descriptors. Use the root/loose/reopen paths when you also need file ranges.

## 8. Bounded loose scanning with trusted geometry

If the caller has a trusted `fauxfat_view` but wants salvage descriptors even after strict failure:

```c
size_t count;
fauxfat_volume_class cls;

rc = fauxfat_scan_loose(&view, &device,
                        emit_file, ctx, &count, &cls);
```

The scanner can emit:

- exact fauxFAT public files, using the persisted logical name;
- exact fauxFAT opaque descriptors;
- simple ordinary root files whose Stream Extension directly proves a contiguous `NoFatChain` allocation.

It skips some well-bounded unsupported file sets and directories. It rejects malformed or ambiguous sets. It never follows a fragmented file's FAT chain.

A renamed fauxFAT public file is emitted with the original logical name and `FAUXFAT_DISK_FILE_NAME_CHANGED` set in `flags`.

## 9. Reopen without a schema

When no manufactured view survives, use the block device directly:

```c
fauxfat_reopen_info info;
size_t count;
fauxfat_volume_class cls;

rc = fauxfat_reopen(&device,
                    emit_file, ctx,
                    &count, &cls, &info);
```

`fauxfat_reopen()` derives only the supported fixed fauxFAT geometry. It does not attempt arbitrary exFAT discovery.

Code which needs to apply policy before exposing recovered descriptors can use
the split form instead:

```c
rc = fauxfat_reopen_probe(&device, &cls, &info);
if (rc == FAUXFAT_OK &&
    (info.flags & FAUXFAT_REOPEN_SCAN_READY) != 0)
    rc = fauxfat_reopen_scan(&device, &info, emit_file, ctx, &count);
```

The probe performs geometry/identity/structural classification. The scan then
walks the bounded root once using the recorded geometry; it does not rerun the
full self-validation. `fauxfat_reopen()` remains the convenience wrapper which
does both.

This function is volume-relative. If the caller has a whole card/image which
may be GPT-wrapped, prefer the integration API in `fauxfat_block.h` described
below instead of manually guessing an LBA offset and handing it to
`fauxfat_reopen()`.

`info` reports the recovered stable presentation identity and geometry:

```c
info.partition_lba
info.volume_blocks
info.structural_epoch
info.volume_serial
info.fat_length_blocks
info.cluster_heap_block
info.cluster_count
info.root_cluster
info.volume_guid
info.volume_label
```

`info` and `descriptor_count` may be `NULL`. If no recognizable fauxFAT OEM identity survives, `info.structural_epoch` remains zero because there is no trusted epoch source.

Classification is:

```text
FAUXFAT_VOLUME_FAUXFAT_VALID
    self-described fauxFAT structure and OEM seals validate

FAUXFAT_VOLUME_FAUXFAT_CHANGED
    fauxFAT identity is recognizable but strict self-validation fails

FAUXFAT_VOLUME_EXFAT_BEST_EFFORT
    supported bounded exFAT root/geometry is usable, but fauxFAT identity is absent

FAUXFAT_VOLUME_INVALID
    unsupported, malformed, or too ambiguous for the bounded parser
```

The emitted descriptors are salvage information. In particular, `FAUXFAT_VOLUME_FAUXFAT_CHANGED` does not tell the application which private generation or staged object is authoritative.

## 10. Use a recovered descriptor as backing storage

A recovered descriptor can be used directly for bounded byte I/O against the same block device:

```c
rc = fauxfat_disk_file_read(&device, &desc,
                            byte_offset, dst, length);

rc = fauxfat_disk_file_write(&device, &desc,
                             byte_offset, src, length);
```

Both functions reject access beyond `desc.data_length`; allocation slack is inaccessible.

Reads require `device.read`.

Writes require `device.write`. A whole-sector-aligned write can succeed without `device.read`. An unaligned first or final sector requires read-modify-write and therefore also requires `device.read`.

These helpers accept both public and opaque descriptors. They are application-side physical range adapters. Whether a recovered private range may be modified is the caller's policy.

A common pattern is to store one `fauxfat_disk_file` in each caller-owned open-handle slot and route the original fauxFAT payload callbacks back through these helpers after reopen.

## 11. Recommended recovery sequence

A conservative in-place recovery cycle is:

1. stop host access;
2. call `fauxfat_reopen()` or a trusted-view validator/scanner;
3. consume the emitted descriptors and compare them with application authority;
4. construct the desired `fauxfat_config` / `fauxfat_view`;
5. use the formatter preservation callback for exact public ranges which should survive;
6. let opaque/anonymous ranges remain preserve-only;
7. run `fauxfat_format()` to regenerate canonical metadata;
8. call `fauxfat_validate_strict()` against the desired view;
9. expose the block device again only after strict validation succeeds.

If a payload needs relocation or resizing, do that outside fauxFAT under the application's transaction policy. Then build the new view around the resulting physical ranges.

## 12. Structural epoch

`structural_epoch` belongs to the presentation, not to every payload write.

Increment it when the manufactured layout materially changes, for example:

- a public or named opaque extent moves;
- a size/allocation changes;
- a root slot changes role;
- anonymous reserve is carved into or reclaimed from a named object;
- the public/private presentation of a range changes.

Do not increment it merely because bytes inside a fixed public file changed.

A prebuilt alternate metadata image may be used by an application as an optional cache/checkpoint for quick regeneration or comparison. That is outside the fauxFAT core API and is not required for staged replacement or private versioning.

## 13. Errors and callback returns

Library-defined errors are:

```text
FAUXFAT_OK
FAUXFAT_EINVAL
FAUXFAT_ERANGE
FAUXFAT_EGEOMETRY
FAUXFAT_EUNMAPPED
FAUXFAT_ESTRUCTURE
```

Payload and block-device callback failures are propagated unchanged. If the application needs to distinguish backend errors from fauxFAT's small negative error range, give backend failures a separate range.

The classification enum is not an error code. A validator may return `FAUXFAT_OK` while reporting `FAUXFAT_VOLUME_FAUXFAT_CHANGED` or `FAUXFAT_VOLUME_INVALID`.

## 14. Deliberate limits

Do not assume fauxFAT provides any of the following:

- allocation of new host files;
- file growth or shrink through host writes;
- fragmented-file support;
- FAT-chain traversal for application files;
- directory recursion;
- filesystem repair;
- TexFAT handling;
- authentication;
- atomic multi-callback payload transactions;
- payload relocation during format/recovery.

Those omissions are the point of the design, not pending filesystem features.

## 15. Plan canonical GPT placement

The high-level block layer owns the appliance-specific placement policy. The
low-level `fauxgpt` module deliberately does not. Use
`fauxfat_block_plan_gpt()` to compute partition LBAs before constructing the GPT
records:

```c
fauxfat_block_gpt_plan plan;

rc = fauxfat_block_plan_gpt(&plan,
                            card_blocks,
                            fauxfat_block_count(&view),
                            card_au_blocks,
                            1); /* include user partition */
```

The planner chooses `max(2048, card_au_blocks)` 512-byte blocks as the
alignment, puts fauxFAT at the first aligned LBA at or after GPT's first usable
LBA, and optionally puts the user partition at the next aligned LBA and extends
it through the final usable GPT LBA. A zero `card_au_blocks` means the card did
not report an AU.

The returned structure contains only geometry. It does not invent GUIDs, names,
or partition types, and it does not silently grow fauxFAT. If the application
wants no gap between partitions, size the anonymous fauxFAT tail first and call
the planner again with the resulting final volume size. `partition_count` is 1
or 2 and the user fields are zero when the user partition is omitted.

The physical partition offset does not affect `fauxfat_block_count()`, so it is
valid to compute the volume size first, plan the disk, assign
`config.partition_lba = plan.fauxfat_first_lba`, then initialize the final view.

## 16. Open a bare or GPT-wrapped block device

`include/fauxfat_block.h` binds the bounded GPT reader to schema-free fauxFAT
reopen. The physical device supplies its total block count plus the existing
fauxFAT block callbacks:

```c
#include "fauxfat_block.h"

fauxfat_block_device disk = {
    .block_count = card_blocks,
    .io = {
        .read = raw_read,
        .write = raw_write,
        .zero = raw_zero,
        .skip = raw_skip,
        .context = card,
    },
    .flush = raw_flush,
    .generation = card_generation, /* optional hotplug/media-replacement fence */
};

fauxfat_block_opened opened;
rc = fauxfat_block_open(&opened, &disk,
                        FAUXFAT_BLOCK_WRAPPER_AUTO,
                        &expected_layout,
                        emit_file, ctx, &count);
```

For diagnostics, repair decisions, or UI which needs to explain why open would
refuse a target, inspect it first without applying product policy:

```c
fauxfat_block_probe_info probe;
rc = fauxfat_block_probe(&probe, &disk);
```

`probe.kind` distinguishes bare media, coherent GPT, damaged GPT metadata, and
two individually valid but conflicting GPT copies. `probe.gpt_probe` retains
the primary and backup observations independently. A coherent GPT is still
reported even when it has more than two active partitions or partition 1 is
not fauxFAT; those are policy failures for `fauxfat_block_open()`, not failures
to observe the disk. The block probe deliberately stops before the final loose
descriptor scan. `fauxfat_block_open()` applies policy and then performs that
bounded scan once, whether or not an emit callback is set, so
open success does not depend on whether descriptor output happened to be
requested.

`FAUXFAT_BLOCK_WRAPPER_AUTO` accepts either:

- a bare fauxFAT volume beginning at physical LBA 0; or
- the narrow GPT profile with one or two active partitions and fauxFAT in GPT
  entry 0.

GPT markers which are present but damaged are not reinterpreted as a bare
volume. A valid GPT with more than two active partitions is also rejected
rather than being partially understood.

When `expected_layout` is non-NULL, the disk size, partition count, type GUIDs,
ranges, and attributes must match. Disk/partition GUIDs and GPT names are not
part of that geometry check. They are identity/presentation fields, not an
excuse to reject an otherwise identical partition map because somebody renamed
"USER DATA" in a partition editor.

For GPT, partition 1 must contain a recognizable fauxFAT OEM identity.
`FAUXFAT_VOLUME_EXFAT_BEST_EFFORT` is deliberately not sufficient here. A
generic exFAT filesystem in the expected-looking first partition is user data,
not an invitation to overwrite it.

The returned `opened.volume_device` is a bounds-checked volume-relative adapter
over the physical device and can be passed to `fauxfat_disk_file_read()` /
`fauxfat_disk_file_write()`. `opened` must remain at the same address while
that adapter is in use because its callback context points back to the opened
object.

Removable-media integrations should also provide `disk.generation`. The
callback returns an arbitrary 64-bit token for the currently inserted/opened
medium; change it on removal, insertion, controller reinitialization, or any
other event which invalidates previously opened handles. Equality is the only
contract, so a simple insertion-generation counter is enough. Probe/open,
format, and GPT repair capture one token for the entire operation, and every
raw read/write/zero/skip/flush through the block adapter checks it before and
after touching the backend. A changed token returns
`FAUXFAT_BLOCK_ESTALE`; an already-opened partition adapter therefore cannot
quietly apply old GPT geometry to a replacement card. `probe.media_generation`
and `opened.media_generation` expose the captured token for diagnostics.

This fence is not a locking primitive. The storage integration still has to
exclude USB MSC/host access and other writers while device-side open, repair,
format, or writable handles are active. If media physically changes inside a
backend write callback, the library can detect that only when the callback
returns; preventing such a write from reaching replacement media is the block
driver/hotplug state machine's job.

## 17. Publish or reformat a complete target

Formatting uses the same integration layer, but AUTO is forbidden. Destructive
code must say whether it intends a bare volume or GPT:

```c
rc = fauxfat_block_format(&view, &disk,
                          FAUXFAT_BLOCK_WRAPPER_GPT, &gpt,
                          preserve, preserve_ctx, 0);
```

Without `FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA`, the existing target must
already open as the requested wrapper. For GPT it must also match the requested
partitioning and partition 1 must already carry recognizable fauxFAT identity.
Safe reformatting then performs an ownership check: the existing fauxFAT
Volume GUID and volume serial must equal the requested view, and GPT media must
also retain the requested disk GUID and every active partition's unique GUID.
The structural epoch, volume label, and GPT partition names are deliberately
excluded from this identity test because ordinary regeneration is allowed to
change presentation metadata without pretending the disk became somebody
else's.
Therefore all of these are refused by default:

- a GPT whose partition map differs from the expected layout;
- a valid one- or two-partition GPT whose first partition is not fauxFAT;
- a same-shaped fauxFAT volume with a different Volume GUID or serial;
- a same-shaped GPT with a different disk or partition GUID;
- converting bare <-> GPT;
- unknown or apparently blank media.

The last case is intentionally conservative. Looking blank in the first few
metadata sectors is not proof that an arbitrary block device contains no user
data. Initial provisioning therefore passes
`FAUXFAT_BLOCK_FORMAT_DESTROY_USER_DATA` explicitly.

For GPT output, GPT entry 0 must cover the fauxFAT view exactly.
The formatter materializes fauxFAT first, flushes it, then publishes GPT via
the backup-array/header -> flush -> primary-array/header/PMBR -> flush sequence.
Partition entry 1 is never formatted or otherwise touched by this API.

Unlike the lower-level `fauxfat_format()` renderer, `fauxfat_block_format()` is
a safety boundary and therefore requires `disk.io.read` and `disk.flush` as
well as write/zero. Synchronous media can provide a no-op flush callback, but
the durability contract is explicit rather than inferred. After the last flush
it reads the result back before reporting success. The fauxFAT partition must
pass `fauxfat_validate_strict()` against the requested view. GPT output must
also exactly match every rendered GPT metadata sector and reopen through the
bounded parser with both copies plus the PMBR valid and the requested geometry
and stable GUID identity. A successful write callback followed by mismatching
readback returns `FAUXFAT_BLOCK_EVERIFY`; a real read failure is still
`FAUXFAT_BLOCK_EIO`, and media replacement is still `FAUXFAT_BLOCK_ESTALE`.

`FAUXFAT_BLOCK_FORMAT_ZERO_UNDEFINED` maps to the existing fauxFAT
reproducibility flag. `DESTROY_USER_DATA` authorizes destructive replacement;
it does not turn fauxFAT preserve ranges or the user partition into erase
requests.

The integration-specific failures are `FAUXFAT_BLOCK_EWRAPPER` for malformed
or wrong wrapper metadata, `FAUXFAT_BLOCK_EPARTITION` for a valid-enough GPT
whose partition map is unacceptable, and `FAUXFAT_BLOCK_ENOTFAUXFAT` when the
candidate volume lacks recognizable fauxFAT identity.
`FAUXFAT_BLOCK_EIDENTITY` means the wrapper/geometry is acceptable but one of
the stable ownership identifiers differs from the requested format target.
`FAUXFAT_BLOCK_EVERIFY` means a format or GPT repair completed its mutation
callbacks but immediate readback did not match the requested materialization.
`FAUXFAT_BLOCK_ESTALE` means the optional media-generation token changed
during the operation or after an opened adapter was created. Raw block callback
failures are translated to `FAUXFAT_BLOCK_EIO`, so an SD/backend errno such as
`-5`, `-6`, or `-7` cannot accidentally become GPT parser state. The original
callback value is retained as `probe.backend_error` by
`fauxfat_block_probe()` and as `opened.backend_error` for I/O through a
successfully opened volume adapter. The lower-level `fauxfat` and `fauxgpt`
APIs continue to propagate their callbacks unchanged.

## 18. Repair only the GPT wrapper

When the product already knows the intended GPT layout and partition-1
fauxFAT identity, repair the wrapper without reformatting the volume:

```c
rc = fauxfat_block_repair_gpt(&view, &disk, &gpt);
```

This call first probes the expected partition-1 range directly, without using
on-disk GPT to locate it. The candidate must be recognizable fauxFAT at exactly
the requested `PartitionOffset` and `VolumeLength`, with the requested Volume
GUID and volume serial. It then inspects both GPT copies independently. Every
valid copy must match the requested partition geometry and stable GPT disk /
partition GUIDs. Two valid copies which disagree are refused as ambiguous.

If both GPT copies are damaged or missing, exact partition-1 identity is enough
to reconstruct the protective MBR and both GPT copies. The operation writes
only GPT metadata, backup first, and never calls the fauxFAT formatter or
writes either partition body. Before returning success it reads every GPT
metadata sector back and requires an exact match to the authoritative view.
There is no `DESTROY_USER_DATA` option on this
API. Foreign geometry or identity must be handled by the explicit destructive
formatting path instead of quietly smuggling repartitioning in under the word
"repair".

## 19. Low-level whole-card GPT API

`include/fauxgpt.h` is deliberately separate from fauxFAT. It renders only the protective MBR, primary/backup GPT entry arrays, and primary/backup headers. It never reads or writes a partition body.

```c
#include "fauxgpt.h"

fauxgpt_partition parts[2] = {0};

memcpy(parts[0].type_guid, fauxgpt_type_microsoft_basic_data, 16);
memcpy(parts[0].unique_guid, app_partition_guid, 16);
parts[0].first_lba = plan.fauxfat_first_lba;
parts[0].block_count = plan.fauxfat_block_count;
parts[0].name = "INGRESS";

memcpy(parts[1].type_guid, fauxgpt_type_microsoft_basic_data, 16);
memcpy(parts[1].unique_guid, user_partition_guid, 16);
parts[1].first_lba = plan.user_first_lba;
parts[1].block_count = plan.user_block_count;
parts[1].name = "USER DATA";

fauxgpt_layout layout = {
    .disk_blocks = card_blocks,
    .partitions = parts,
    .partition_count = plan.partition_count,
};
memcpy(layout.disk_guid, card_guid, 16);

fauxgpt_view gpt;
rc = fauxgpt_init(&gpt, &layout);
```

All GUID byte arrays are already in GPT on-disk byte order. The module does not parse UUID strings or invent identifiers. Names are optional printable ASCII, at most 36 bytes.

For a synthetic whole-disk read path, call `fauxgpt_render_block()` first for GPT metadata LBAs; `FAUXGPT_EUNMAPPED` means route the request to a partition body or other storage.

The same module now has a bounded reader:

```c
fauxgpt_device gd = {
    .read = raw_read,
    .write = raw_write,
    .flush = raw_flush,
    .context = card,
};

fauxgpt_info gi;
rc = fauxgpt_open(&gi, &gd, card_blocks);
```

`fauxgpt_probe()` is the observation primitive underneath `fauxgpt_open()`.
It inspects the PMBR, primary copy, and backup copy independently and records
markers, CRC-valid copies, array CRCs, and the bounded active-entry count.
Structural damage or a primary/backup conflict remains visible in the probe
result instead of being collapsed immediately into `FAUXGPT_ESTRUCTURE`.
`fauxgpt_open()` then applies the ordinary one-coherent-map / at-most-two-entry
policy to that observation.

`fauxgpt_open()` validates the canonical header geometry, header CRCs, complete
128-entry array CRCs, and active entry ranges. `fauxgpt_verify()` is the stricter
immediate-materialization check: it reads only GPT metadata sectors and requires
them to match `fauxgpt_render_block()` byte-for-byte, including names and unused
metadata bytes. Either primary or backup GPT is
enough to open a degraded disk; `gi.flags` says which copies and whether the
protective MBR are valid. If both GPT copies validate they must agree. More
than two active partitions returns `FAUXGPT_EPARTITIONS`.

`fauxgpt_geometry_matches()` performs the safety-geometry comparison used by
the high-level block API. `fauxgpt_identity_matches()` separately compares the
disk GUID and active partition unique GUIDs. The API exposes only those two
clear operations; the earlier ambiguous geometry helper spelling is gone.

For physical provisioning, format/materialize firmware-owned partition content first, then publish GPT metadata:

```c
fauxgpt_device gd = {
    .write = raw_write,
    .flush = raw_flush,
    .context = card,
};

rc = fauxgpt_format(&gpt, &gd);
```

`fauxgpt_format()` writes backup array/header, flushes, then primary array/header and the protective MBR, then flushes again. It emits no writes between `FAUXGPT_FIRST_USABLE_LBA` and `gpt.last_usable_lba`, so the blank/user-formatted second partition is preserved by construction.

## 20. Windows VHDX qualification utility

`tools/fauxfat_qualify_win.c` is a native Windows qualification harness. Build
it from a Unix-like development host with Zig's bundled MinGW environment:

```sh
make windows-tool
# -> build/fauxfat-qualify.exe
```

The target uses `zig cc -target x86_64-windows-gnu` and links the Windows
Virtual Disk API directly. The source carries only the small Win32/Virtdisk ABI
surface it needs, so no separate Windows SDK or mingw installation is required
when Zig is present. `make check-windows-tool` performs an ordinary host C
syntax/warning check without requiring Zig.

The normal qualification path is a disposable dynamic VHDX, not a real disk:

```text
fauxfat-qualify.exe create-vhdx C:\\temp\\fauxfat-test.vhdx
fauxfat-qualify.exe verify-vhdx C:\\temp\\fauxfat-test.vhdx
fauxfat-qualify.exe detach-vhdx C:\\temp\\fauxfat-test.vhdx
```

`create-vhdx` defaults to a 512 MiB virtual disk with a 64 MiB fauxFAT payload
arena. `--size-mib N` and `--fauxfat-data-mib N` override those values. The tool
creates a 512-byte-logical-sector VHDX, attaches it without drive letters while
formatting, materializes the canonical two-partition GPT through
`fauxfat_block_format()`, detaches it, then attaches it normally with permanent
lifetime. Partition 1 is labelled `FAUXQUAL` and contains one fixed 8 MiB
`QUALIFY.BIN`; partition 2 is Microsoft Basic Data but intentionally has no
filesystem. Windows can therefore mount/mutate partition 1 and the operator can
format partition 2 with normal host tooling before running `verify-vhdx` again.

`verify-vhdx` reuses an existing attachment when possible; otherwise it makes a
temporary read-only/no-drive-letter attachment. It opens the resulting
`\\.\\PhysicalDriveN` through the real block-device API, requires the PMBR and
both GPT copies to validate, runs schema-free fauxFAT reopen/descriptor scanning,
and requires the persisted `QUALIFY.BIN` descriptor to remain recoverable at
its original logical size. The harness derives the GPT disk/partition GUIDs from
the fauxFAT Volume GUID, so verification also detects identity rewrites without
a sidecar file. It replans the canonical two-partition geometry from the reopened
partition-1 size, so deleting/moving/resizing the user partition is not mistaken
for a successful host-filesystem qualification. A namespace rename is acceptable
because the vendor records preserve the logical name; deleting/truncating the
qualification object is not.

For real-media qualification there is a deliberately inconvenient destructive
path:

```text
fauxfat-qualify.exe format-raw \\.\\PhysicalDrive7 --destroy-user-data
fauxfat-qualify.exe verify-raw \\.\\PhysicalDrive7
```

`format-raw` accepts only the exact `\\.\\PhysicalDriveN` spelling and refuses
to run without the literal `--destroy-user-data` option. It does not lock or
dismount existing Windows volumes on the target; Windows may therefore reject
writes to an in-use disk, which is preferable to this test utility quietly
becoming `diskpart` with worse judgment. After direct GPT writes it issues
`IOCTL_DISK_UPDATE_PROPERTIES` so Windows invalidates its cached partition map.
The fauxFAT library's own `DESTROY_USER_DATA` flag is also set; partition 2's
body is still never written by fauxFAT/GPT formatting.
