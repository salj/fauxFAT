# fauxFAT overview

Formatting, validation, recovery, GPT, and whole-device support are implemented. Windows VHDX qualification has been exercised; Linux/macOS and real-media durability still need work.

fauxFAT exposes a fixed exFAT volume over storage whose physical layout stays under application control. Hosts can overwrite predefined files; the application owns the backing data and reserved ranges.

See `fauxfat-disk-format.md` for the disk layout, `fauxfat-usage.md` for the API and recovery flow, and `fauxfat-partition-layout.md` for GPT layout.

## 1. Purpose

fauxFAT gives the host a few ordinary files while firmware keeps each file at a known physical range. By default the host can overwrite file data, but cannot allocate clusters or grow the root directory. A file configured with an explicit reserved allocation may change its directory length within that fixed extent; strict validation accepts that length change.

## 2. Physical model

A fauxFAT volume has four relevant kinds of space:

| Kind | Host-visible meaning | Firmware rule |
| --- | --- | --- |
| Public file | Ordinary root file | Host may overwrite bytes inside `DataLength`; explicitly reserved files may change `DataLength` within their fixed extent. |
| Named opaque range | Hidden descriptor plus private allocation | Not writable through host block translation; recoverable by logical name and physical range. |
| Anonymous opaque reserve | Canonically no useful directory object; qualification mode may add a non-file benign owner | Preserved physical capacity for application use or later layout changes. |
| Filesystem structure / undefined padding | exFAT metadata or semantically irrelevant bytes | Generated, verified, zeroed, or skipped according to the format contract. |

Every useful data allocation is contiguous. Public files without explicit capacity are exFAT `NoFatChain` streams. Public files with reserved growth capacity use a contiguous FAT chain so the chain can exceed the current `DataLength`. Opaque and anonymous reserved clusters are marked allocated in the exFAT allocation bitmap and carry `0xFFFFFFF7` in the FAT, which fauxFAT uses as its deterministic private/reserved marker.

Qualification builds may additionally describe each otherwise-ownerless
anonymous contiguous run with an unrecognized benign Generic Primary entry.
That redundant owner is never used instead of the bad-cluster marker: the
canonical blocker remains present so a host which ignores the benign primary
still sees the range as unavailable. This mode is experimental and exists only
to learn what desktop stacks preserve.

The allocation bitmap is saturated. The fixed one-cluster root directory is also saturated with defined entries or benign padding. There is no free namespace or free cluster pool for normal host allocation.

The resulting rule is intentionally narrow:

```text
host may change:
    payload bytes inside predefined public files
    documented exFAT timestamp/archive fields
    Main VolumeDirty

host may not change:
    geometry
    allocation
    file lengths or physical placement
    fauxFAT logical identity records
    opaque descriptors
    FAT ownership map
    allocation bitmap
    structural OEM seal
```

## 3. Fixed exFAT profile

The current format fixes the major geometry and parser bounds:

```text
logical sector          512 bytes
cluster                 64 KiB = 128 sectors
FAT count               1
root directory          exactly one cluster
filesystem revision     exFAT 1.00
logical names           1..15 ISO-8859-1 bytes
upcase table            128-byte generated table
```

The volume is laid out as:

```text
main boot region
backup boot region
FAT alignment
FAT
cluster heap:
    allocation bitmap
    upcase table
    root directory
    payload arena:
        public extents
        opaque descriptors and extents
        anonymous reserved gaps/tail
```

A public file uses five root entries:

```text
File
Stream Extension, NoFatChain=1
File Name
Vendor Extension, persisted logical-name part 0
Vendor Extension, persisted logical-name part 1
```

The vendor records preserve the manufactured logical name independently of the mutable visible namespace name. If a host renames the file, bounded parsing can still recover its original identity and sets `FAUXFAT_DISK_FILE_NAME_CHANGED`. Strict whole-volume validation still treats the rename as a structural change.

A named opaque allocation also uses a five-entry set:

```text
File, hidden/system/read-only
Stream Extension, zero-length inert namespace stream
File Name, deterministic internal stub
Vendor Extension, logical name
Vendor Allocation, contiguous private range
```

The namespace stub is not the application-visible identity. This allows a public file and one or more private generations to use the same logical name without creating duplicate visible exFAT names.

## 4. Structural fingerprints

fauxFAT stores one XXH32 structural fingerprint in exFAT OEM Parameters and separate XXH32 component fingerprints for FAT, bitmap, root, and upcase data.

The structural fingerprint covers canonical metadata, not payload bytes. Large application files therefore do not make validation proportional to payload size. With 64 KiB clusters, metadata hashing grows roughly with FAT/bitmap size plus the fixed root cluster.

The hash is corruption/change detection, not authentication. A writer with arbitrary block access can rewrite both metadata and an unkeyed hash. If hostile-writer authenticity is ever required, it needs a key or signature anchored outside the writable fauxFAT presentation.

Native exFAT checksums remain in use where the exFAT format requires them: boot checksum, entry-set checksum, NameHash, and upcase-table checksum.

## 5. Two I/O sides

fauxFAT separates application payload storage from the synthetic block device.

Application payloads are identified by an integer descriptor and are accessed through bounded callbacks:

```c
int read(void *ctx, int fd, uint64_t offset, void *dst, size_t length);
int write(void *ctx, int fd, uint64_t offset, const void *src, size_t length);
```

The descriptor is opaque to fauxFAT. It may represent a raw extent, another filesystem object, a host file in tests, or any other backing store which can service `{fd, offset, length}`.

The block-facing side works in volume-relative 512-byte sectors:

```text
block read
    metadata / required-zero / undefined / anonymous reserve
        -> synthesize deterministic bytes
    public payload
        -> read(fd, offset, length)
    named opaque payload
        -> read(fd, offset, 512)

block write
    public payload
        -> write(fd, offset, length)
    everything else
        -> FAUXFAT_EUNMAPPED
```

This path requires no whole-file buffer and no extent index proportional to file count or volume size.

## 6. Direct physical descriptors

The common interchange type between view generation, parsing, preservation decisions, and recovered-range I/O is `fauxfat_disk_file`:

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

It intentionally describes only a recognized contiguous physical object. It is not a filesystem object graph.

Descriptors can be obtained from a manufactured view, from the strict root
parser, from the loose scanner, or from schema-free `fauxfat_reopen()`. The
split `fauxfat_reopen_probe()` / `fauxfat_reopen_scan()` form is used when a
caller needs to apply policy after identifying the volume but before exposing
recovered descriptors. The same descriptor can then be used for bounded byte
I/O directly against a block device with `fauxfat_disk_file_read()` and
`fauxfat_disk_file_write()`.

The library does not allocate or retain a file-descriptor table. A caller which wants ordinary integer handles stores one recovered descriptor in each of its own open-handle slots.

## 7. Sparse formatting and in-place regeneration

`fauxfat_format()` materializes a manufactured view onto a block device without building an image in RAM. The destination interface distinguishes four operations:

| Operation | Required meaning |
| --- | --- |
| generated write | Exact structural bytes must be written. |
| zero | The range must read back as zero. |
| skip undefined | fauxFAT places no condition on existing bytes. |
| skip preserve | Existing payload/private bytes must remain untouched. |

`zero` and `undefined` are deliberately different. A sparse-file backend may implement zero with a hole if holes read as zero. A reused physical device must actually clear a required-zero range. Undefined space may simply be skipped.

Named opaque ranges and anonymous reservations are preserve ranges. Public payload is zero-initialized by default, but the caller may preserve an exact existing public allocation through the formatter's preservation callback.

That enables the core recovery loop:

```text
validate / reopen existing volume
    -> emit understood fauxfat_disk_file descriptors
    -> application decides which exact ranges remain useful
    -> build desired fauxfat_view
    -> format canonical metadata while preserving accepted ranges
    -> strict-validate result
```

fauxFAT never moves payload as part of reformatting. If an object must move or resize, the caller performs that transaction separately.

## 8. Validation modes

There are two acceptance levels.

Strict validation answers: "is this still the fauxFAT structure we manufactured, allowing only the small set of metadata changes expected from a compliant exFAT mount and in-place overwrite cycle?"

It verifies fixed geometry, both boot regions and native checksums, the upcase table, saturated bitmap, bounded root grammar, deterministic FAT classification, OEM identity, and the XXH32 structural/component fingerprints. Payload bytes and explicitly undefined padding are not inspected.

Loose scanning is bounded salvage. It can return:

- exact fauxFAT public descriptors;
- exact fauxFAT opaque descriptors;
- simple foreign root files which directly prove a contiguous `NoFatChain` allocation.

It may skip unsupported but well-bounded file sets. It aborts on malformed or ambiguous structure. It does not walk arbitrary FAT chains, recurse directories, repair allocation state, or interpret TexFAT transactions.

The classification is separate from descriptor recovery:

```text
FAUXFAT_VOLUME_INVALID
    structure is malformed or outside the bounded parser contract

FAUXFAT_VOLUME_EXFAT_BEST_EFFORT
    bounded exFAT parsing recovered simple files, but fauxFAT identity is absent

FAUXFAT_VOLUME_FAUXFAT_CHANGED
    fauxFAT is recognizable but its strict structural contract was violated

FAUXFAT_VOLUME_FAUXFAT_VALID
    strict fauxFAT structure and seal are intact
```

Recovering a useful file from a changed volume never promotes that volume back to trusted fauxFAT state.

## 9. Schema-free reopen

`fauxfat_reopen()` needs only a block-device reader. It derives the supported
fauxFAT geometry from the Main Boot Sector, checks the bounded
boot/OEM/root/FAT/bitmap/upcase structures, emits direct physical descriptors,
and reports the classification above. The whole-device layer uses the split
probe/scan form so policy can reject a candidate before a final descriptor scan
without performing the full reopen twice.

Reopen needs no file table from the caller. One surviving recognizable OEM identity copy plus a coherent bounded fauxFAT root profile is enough to classify a damaged presentation as fauxFAT-changed. OEM parameter bytes alone are not ownership: host quick-format operations may replace the exFAT filesystem while leaving stale OEM sectors untouched. If fauxFAT identity is gone but the supported bounded exFAT geometry/root remains sane, reopen may return `FAUXFAT_VOLUME_EXFAT_BEST_EFFORT` descriptors.

This is intentionally not a general exFAT mount operation.

## 10. Higher-level payload policy is outside fauxFAT

fauxFAT says which physical ranges a host may overwrite and which ranges must remain opaque. It does not decide application commit semantics.

A caller may use the same mechanisms for, for example:

- a staged replacement object with one public ingress extent and one private committed extent;
- a continuously edited public configuration file whose accepted generations are copied into a private version store;
- private raw objects which never appear to the host;
- anonymous reserve space which is carved into named objects only while the device is private.

The structural epoch is a presentation-generation number. Change it when the manufactured fauxFAT structure changes materially. Ordinary payload writes do not change it.

A second prebuilt metadata image is not part of the core format. It may be useful as an optional quick-regeneration/checkpoint cache or comparison copy, but the same resilience can also be obtained by regenerating canonical metadata from authoritative state. Application generation-selection policy does not depend on such a mirror.

## 11. Explicit non-goals

fauxFAT deliberately does not implement:

- arbitrary path lookup;
- general directory traversal;
- FAT-chain traversal for fragmented files;
- allocation or free-space reconstruction;
- root-directory growth;
- orphan recovery;
- filesystem repair;
- TexFAT or journal/intent-log replay;
- authentication of hostile writers;
- transactional payload movement.

If a future host behavior requires one of those features merely to accept a volume as strict-valid, the preferred response is to reject and regenerate the façade rather than turn fauxFAT into another filesystem stack.

## 12. Current implementation and remaining qualification

Implemented now:

- deterministic exFAT 1.00 metadata generation;
- callback-backed public and opaque payloads;
- synthetic sector reads and bounded public-sector writes;
- explicit payload-arena placement and anonymous reserved gaps/tail;
- persisted public logical names and opaque vendor descriptors;
- direct physical descriptor enumeration;
- sparse/in-place formatting with generated/zero/undefined/preserve semantics;
- strict bounded root parsing;
- strict whole-volume validation against a trusted view;
- bounded loose scanning;
- schema-free reopen;
- direct bounded byte I/O using recovered descriptors;
- structural XXH32 and component fingerprints;
- Unix `time_t` input for manufactured file timestamps.
- bounded GPT render/probe/open/verify with primary/backup reconciliation;
- canonical GPT placement planning using `max(1 MiB, SD AU)` alignment;
- bare/GPT whole-device probe/open/format policy with explicit destructive
  authorization for foreign/unknown media;
- stable fauxFAT + GPT identity checks for non-destructive formatting;
- GPT-only non-destructive repair which never writes partition bodies;
- optional removable-media generation fencing for stale handles/hotplug;
- immediate readback verification after whole-device format/repair;
- exhaustive callback-failure and torn-GPT-write tests in `make test-faults`;
- native Windows VHDX/raw qualification harness cross-built with Zig.

Windows qualification has already established several useful boundaries:

- the canonical VHDX/GPT/fauxFAT image mounts and reopens through a real
  `PhysicalDrive`;
- manufactured logical identity survives a namespace rename through the vendor
  name records;
- delete/recreate is not a supported update primitive because the freed extent
  becomes ordinary allocatable space and Windows may consume it immediately;
- DiskPart quick-format can leave stale OEM parameter bytes, so OEM identity is
  never trusted without a coherent bounded root profile;
- anonymous bitmap-only reservation is not viable: Windows `chkdsk` diagnoses
  it as corruption and `/F` releases it;
- the canonical bad-cluster blocker survives `chkdsk`, at the cosmetic cost of
  being reported as bad media capacity.

Still required before treating the host contract as production-qualified:

- broaden Windows coverage across the exact versions/filter stacks we intend to
  support;
- exercise normal Linux kernel exFAT and current macOS exFAT implementations;
- verify preservation of the benign vendor records, opaque Vendor Allocation descriptors, `0xFFFFFFF7` blockers, saturated bitmap, and full padded root;
- qualify the optional redundant benign-primary reservation mode, or remove it
  if a normal desktop path objects;
- record any real host mutations which need to be added to the strict canonicalization allowlist;
- verify the real SD backend's flush/durability semantics; host-side fault
  injection proves ordering logic, not what a particular card does with its
  internal cache.

The core rule remains simple: payload storage is valuable; the exFAT façade is reproducible metadata.
