# fauxFAT v1 on-disk format

This is the on-disk contract for fauxFAT v1. It defines the exFAT layout, file extents, and host-allowed changes.

The profile follows Microsoft exFAT 1.00.

## 1. Required behavior

A fauxFAT volume shall have these properties:

1. Every cluster is unavailable for allocation from the instant the volume is manufactured.
2. A fixed set of host-visible files exists in fixed root-directory slots. Each file is a contiguous `NoFatChain` extent.
3. An ordinary host may overwrite bytes inside an existing file without changing allocation metadata.
4. The root directory has no free directory entries and cannot grow.
5. Project-private and reserved ranges are blocked from ordinary file allocation by canonical `0xFFFFFFF7` FAT markers. Ranges whose identity must survive a façade rebuild additionally carry a fauxFAT opaque descriptor entry set in the root.
6. An opaque descriptor is an ordinary hidden zero-length File set followed by fauxFAT Vendor Extension and Vendor Allocation secondaries; the logical private-file name is stored in vendor data while the Vendor Allocation points at the preserved contiguous range.
7. Structural state is checksummed in the OEM Parameters sectors. Any structural mutation outside the explicit host-mutable fields is incompatible.
8. On incompatible mutation the firmware does not repair arbitrary exFAT. It preserves only payload ranges it can positively describe, then rebuilds the façade from authoritative private state.

This is an exFAT-shaped block scratch pad, not a general writable filesystem.

## 2. Fixed v1 geometry

fauxFAT v1 fixes:

```
logical sector size       512 bytes
BytesPerSectorShift       9
cluster size              65536 bytes
sectors per cluster       128
SectorsPerClusterShift    7
NumberOfFats              1
FileSystemRevision        0x0100
root directory length     exactly 1 cluster
filename character set    ISO-8859-1 mapped directly to Unicode U+0000..U+00FF
up-case table             fauxFAT custom compressed table, 128 bytes
up-case checksum          0xA872CEE1
```

The containing GPT partition should use the Microsoft Basic Data GUID. GPT is not part of the fauxFAT volume format. `PartitionOffset` is the media LBA at which this exFAT partition starts.

The partition should start on a 1 MiB boundary. fauxFAT itself uses 64 KiB internal alignment.

## 3. Volume layout

All offsets below are relative to the beginning of the exFAT partition.

```
sector 0                  Main Boot Sector
sectors 1..8              Main Extended Boot Sectors
sector 9                  Main OEM Parameters
sector 10                 Main Reserved
sector 11                 Main Boot Checksum
sector 12                 Backup Boot Sector
sectors 13..20            Backup Extended Boot Sectors
sector 21                 Backup OEM Parameters
sector 22                 Backup Reserved
sector 23                 Backup Boot Checksum
sectors 24..127           FAT alignment, initialized to zero, not trusted
sector 128                FAT begins
...
next 128-sector boundary  Cluster Heap begins
...
end of full clusters      Cluster Heap ends
remaining tail sectors    exFAT Excess Space, initialized to zero, not used
```

Thus:

```
FatOffset = 128
FatLength = align_up(ceil((ClusterCount + 2) * 4 / 512), 128)
ClusterHeapOffset = FatOffset + FatLength
```

`ClusterCount` and `FatLength` depend on one another. The formatter solves them by iteration:

```
fat_len = 128
repeat:
    heap_off = 128 + fat_len
    clusters = floor((VolumeLength - heap_off) / 128)
    new_fat_len = align_up(ceil((clusters + 2) * 4 / 512), 128)
until new_fat_len == fat_len
```

The final values are written to the boot sectors. Any trailing sectors which do not form a whole cluster are Excess Space and are never used by fauxFAT.

## 4. Boot regions

### 4.1 Boot Sector

The main sector at volume sector 0 and backup sector at volume sector 12 use the normal exFAT layout. fauxFAT v1 requires:

| Byte offset | Size | Value |
|---:|---:|---|
| 0 | 3 | `EB 76 90` |
| 3 | 8 | ASCII `EXFAT   ` |
| 11 | 53 | zero |
| 64 | 8 | little-endian `PartitionOffset` |
| 72 | 8 | little-endian `VolumeLength` |
| 80 | 4 | `FatOffset = 128` |
| 84 | 4 | computed `FatLength` |
| 88 | 4 | computed `ClusterHeapOffset` |
| 92 | 4 | computed `ClusterCount` |
| 96 | 4 | `RootCluster` |
| 100 | 4 | stable volume serial |
| 104 | 2 | `0x0100` |
| 106 | 2 | `VolumeFlags`, normally zero |
| 108 | 1 | `9` |
| 109 | 1 | `7` |
| 110 | 1 | `1` |
| 111 | 1 | `0x80` |
| 112 | 1 | `100` |
| 113 | 7 | zero |
| 120 | 390 | `0xF4` |
| 510 | 2 | `55 AA` |

Only the **Main** Boot Sector carries current host-volatile state:

```
VolumeFlags.VolumeDirty
PercentInUse, which remains exactly 100 in the Main Boot Sector
```

The Backup Boot Sector copies these bytes at manufacture time, but exFAT explicitly defines its `VolumeFlags` and `PercentInUse` as stale. fauxFAT validates the backup values only for field-range validity and otherwise ignores them; they are never compared with the current Main values.

In the Main Boot Sector, `ActiveFat`, `MediaFailure`, and `ClearToZero` must remain zero. A real I/O failure may cause firmware to treat the card as failed rather than merely structurally incompatible.

### 4.2 Extended Boot Sectors

Each of sectors 1..8 and 13..20 contains:

```
bytes 0..507    zero
bytes 508..511  00 00 55 AA
```

### 4.3 Reserved sectors

Sectors 10 and 22 are all zero.

### 4.4 Boot checksums

Sectors 11 and 23 contain the normal exFAT 32-bit boot checksum repeated 128 times. The checksum covers the preceding 11 sectors of the corresponding boot region, excluding byte offsets 106, 107, and 112 in its Boot Sector.

Any fauxFAT structural rewrite which changes OEM Parameters must regenerate both boot checksums.

## 5. OEM Parameters: fauxFAT seal

The main OEM Parameters sector is sector 9 and the backup copy is sector 21. Both are byte-identical in canonical state and each is covered by its corresponding boot checksum.

There are ten 48-byte OEM parameter records. fauxFAT v1 uses the first two and writes Null Parameters to the remaining eight.

### 5.1 Parameter 0: structural map fingerprint

GUID:

```
{995BE6E7-3445-46DC-A213-74D985B30134}
on-disk bytes: E7 E6 5B 99 45 34 DC 46 A2 13 74 D9 85 B3 01 34
```

Its 32-byte `CustomDefined` payload is exactly:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | XXH32 of `fauxfat_map_bytes`, seed `0x00000000` |
| 4 | 28 | zero |

`fauxfat_map_bytes` is defined in section 11. It is derived directly from the on-disk boot geometry, FAT, Allocation Bitmap, Up-case Table, and root directory after masking the explicitly host-mutable file metadata. The XXH32 value is the fauxFAT structural fingerprint. This is an accidental-corruption/unaware-writer detector, not an authentication mechanism.

### 5.2 Parameter 1: epoch and component fingerprints

GUID:

```
{8B7283DB-B9EA-4ADB-A327-9B597363479B}
on-disk bytes: DB 83 72 8B EA B9 DB 4A A3 27 9B 59 73 63 47 9B
```

Its 32-byte `CustomDefined` payload is exactly:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `FFV1` |
| 4 | 2 | format version = 1 |
| 6 | 2 | flags = 0 |
| 8 | 8 | little-endian structural epoch |
| 16 | 4 | XXH32 of canonical FAT bytes, seed `0x46415431` |
| 20 | 4 | XXH32 of Allocation Bitmap meaningful bytes, seed `0x42495431` |
| 24 | 4 | XXH32 of canonicalized root cluster, seed `0x524F4F54` |
| 28 | 4 | XXH32 of exact Up-case Table bytes, seed `0x55504331` |

All fauxFAT-private hashes use standard XXH32. exFAT-mandated boot checksums, directory-set checksums, NameHash, and Up-case Table checksum retain their native exFAT algorithms.

The structural epoch changes only when the device intentionally changes the fauxFAT structure: file slot activation/deactivation, file extent movement/resize, public/private range reclassification, or other material layout changes. Ordinary host writes to file contents do not change it.

## 6. FAT: the actual block ownership map

This is the important part. The FAT doubles as the compact, deterministic classification map for every cluster.

```
FatEntry[0] = 0xFFFFFFF8
FatEntry[1] = 0xFFFFFFFF
```

For all real cluster entries `2 .. ClusterCount+1`, exactly one of these states is used:

### 6.1 FAT-chained filesystem metadata

Allocation Bitmap, Up-case Table, and root directory are normal FAT chains:

```
intermediate cluster  -> index of next cluster
last cluster          -> 0xFFFFFFFF
```

The root is exactly one cluster, so `FAT[RootCluster] = 0xFFFFFFFF`.

### 6.2 Host-visible `NoFatChain` file data

Every cluster owned by a host-visible file has:

```
FAT[cluster] = 0x00000000
AllocationBitmap[cluster] = 1
```

The FAT value is deliberately not meaningful for a `NoFatChain` allocation. Zero is fauxFAT's canonical value so the map is deterministic.

### 6.3 Opaque/private/reserved raw data

Every cluster which must not be visible or allocatable to the host has:

```
FAT[cluster] = 0xFFFFFFF7   // exFAT bad-cluster marker
AllocationBitmap[cluster] = 1
```

The bad-cluster marker is fauxFAT's physical blocker and structural range classifier. A range may additionally have an opaque descriptor entry set (section 10.5). The descriptor does not make the range host-writable; it gives firmware a standards-defined, bounded way to recover a logical name and the associated `FirstCluster/DataLength` after parsing a damaged-but-understandable façade.

exFAT treats `0xFFFFFFF7` as a bad cluster, while the Allocation Bitmap marks it unavailable. Firmware is knowingly lying about media health and may use those physical clusters for littlefs, A/B inactive copies, version stores, raw databases, or future reserve space.

For a fauxFAT opaque descriptor the Vendor Allocation secondary sets `NoFatChain=1`, so a generic exFAT implementation shall not interpret the corresponding FAT entries as a chain. fauxFAT nevertheless keeps `0xFFFFFFF7` there as its independent blocker/classification marker.

`VolumeFlags.MediaFailure` remains zero. In exFAT, zero is valid when known failures have already been represented as bad clusters in the FAT. Generic disk diagnostics may report absurd quantities of bad space. Such diagnostics are not part of the supported write protocol; if they rewrite the map, the OEM seal fails.

### 6.4 FAT padding

Bytes after `FatEntry[ClusterCount+1]` through the end of `FatLength` are initialized to zero and are ignored by the fauxFAT seal because exFAT defines that FAT excess space as undefined.

## 7. Allocation Bitmap

The Allocation Bitmap begins at cluster 2.

```
BitmapBytes = ceil(ClusterCount / 8)
BitmapClusters = ceil(BitmapBytes / 65536)
```

Its root entry is type `0x81`, `BitmapFlags = 0`, with `FirstCluster = 2` and `DataLength = BitmapBytes`.

The bitmap's clusters are FAT-chained contiguously from cluster 2.

Every meaningful bitmap bit is `1`. There are no free clusters in a mounted fauxFAT volume. Unused high bits in the final byte are reserved bits inside the Allocation Bitmap and are initialized to zero. Bytes after `BitmapBytes` in the final allocated bitmap cluster are outside the bitmap's `DataLength`; the synthetic view may return zero there, but fauxFAT assigns those cluster-slack bytes no structural meaning and an on-disk verifier ignores them.

The defined bitmap bytes therefore never change during ordinary host operation or during fauxFAT range reclassification. A change to a meaningful allocation bit or to the reserved high bits inside `DataLength` is structural corruption/incompatibility.

## 8. Up-case Table and root cluster placement

Immediately after the bitmap chain:

```
UpcaseCluster = 2 + BitmapClusters
RootCluster   = UpcaseCluster + 1
DataFirstCluster = RootCluster + 1
```

The Up-case Table occupies one cluster. Its root entry is type `0x82` with:

```
TableChecksum = 0xA872CEE1
FirstCluster  = UpcaseCluster
DataLength    = 128
```

fauxFAT uses a custom compressed Up-case Table rather than carrying the 5836-byte recommended Unicode table. exFAT permits a formatter-defined table so long as it covers U+0000..U+FFFF, and every reader is required to understand the compressed representation.

The fauxFAT table is generated algorithmically as 64 little-endian 16-bit words:

```
FFFF 0061                   U+0000..U+0060 identity
0041 0042 ... 005A          U+0061..U+007A -> A..Z
FFFF 0065                   U+007B..U+00DF identity
00C0 00C1 ...               U+00E0..U+00FF ISO-8859-1 folding
                             (E0..F6 and F8..FE subtract 0x20;
                              F7 and FF remain identity)
FFFF FF00                   U+0100..U+FFFF identity
```

This covers the complete 16-bit Unicode range while doing useful case folding only for the character repertoire fauxFAT permits. Characters whose Unicode uppercase form is outside ISO-8859-1, notably `U+00FF`, remain identity mappings. The result is 128 bytes and has exFAT `TableChecksum = 0xA872CEE1`. The remaining bytes in the allocated cluster are outside the Up-case Table's `DataLength`; the synthetic view emits zero there, but a sparse/on-disk formatter may leave that cluster slack undefined and the verifier ignores it. `FAT[UpcaseCluster] = 0xFFFFFFFF`.

The root occupies exactly one cluster. `FAT[RootCluster] = 0xFFFFFFFF`.

## 9. Root directory layout

The root cluster contains exactly 2048 entries of 32 bytes each. No entry is unused and there is no end-of-directory marker.

Canonical ordering:

```text
entry 0        Allocation Bitmap (0x81)
entry 1        Up-case Table (0x82)
entry 2        Volume Label (0x83)
entry 3        Volume GUID (0xA0)
then           public file sets, 3 entries each
then           opaque descriptor sets, 5 entries each
remainder      one-entry 0xA1 padding records
```

The root-entry budget is therefore:

```text
4 + 3 * public_file_count + 5 * opaque_descriptor_count <= 2048
```

With no opaque descriptors this retains the previous maximum of 681 short public files. With no public files the theoretical opaque-descriptor maximum is 408. Real products use vastly fewer of either, because sanity occasionally gets a vote.

### 9.1 Volume Label

Entry 2 is a normal `0x83` Volume Label. The product chooses one fixed label of at most 11 UTF-16 code units. `RP UPDATE` is the current suggested label.

### 9.2 Volume GUID

Entry 3 is a normal `0xA0` Volume GUID entry with `SecondaryCount = 0`, `GeneralPrimaryFlags = 0`, and a stable product-generated GUID. Its one-entry `SetChecksum` is valid.

Keeping this GUID stable across façade regeneration encourages the host to regard the rebuilt volume as the same volume.

### 9.3 Padding entry

Every directory entry not currently used by a real entry set is a one-entry TexFAT Padding record:

```text
byte 0      0xA1
byte 1      0x00        SecondaryCount
bytes 2..3  0x0508 LE   SetChecksum for the canonical all-zero body
bytes 4..31 0x00
```

The base exFAT 1.00 specification says `0xA1` TexFAT Padding must be treated like an unrecognized benign primary entry and must not be moved. fauxFAT uses it only as namespace padding, never as an allocation owner.

Firmware may replace a run of padding entries with one public or opaque-descriptor set only while the host does not own the volume.

## 10. File and opaque-descriptor entry sets

fauxFAT v1 restricts logical names to 1..15 **ISO-8859-1 bytes**. Each byte is decoded directly to the same-numbered Unicode code point. exFAT-forbidden characters (`U+0000..U+001F`, `"`, `*`, `/`, `:`, `<`, `>`, `?`, `\\`, `|`) are rejected, as are `.` and `..`.

There is deliberately no UTF-8 decoder, Unicode normalization, surrogate handling, or general Unicode case machinery.

### 10.1 Public File entry, type `0x85`

A public file uses exactly the normal three-entry set `File + Stream Extension + File Name`.

| Offset | Size | Canonical content |
|---:|---:|---|
| 0 | 1 | `0x85` |
| 1 | 1 | `2` secondary entries |
| 2 | 2 | normal exFAT EntrySetChecksum |
| 4 | 2 | file attributes |
| 6 | 2 | zero |
| 8 | 4 | create timestamp |
| 12 | 4 | last-modified timestamp |
| 16 | 4 | last-access timestamp |
| 20 | 1 | create 10ms increment |
| 21 | 1 | modify 10ms increment |
| 22 | 1 | create UTC offset |
| 23 | 1 | modify UTC offset |
| 24 | 1 | access UTC offset |
| 25 | 7 | zero |

Visible writable files have no ReadOnly, Hidden, System, or Directory bits. Archive may be set or cleared by the host and is treated as volatile.

The formatter takes one code-facing `time_t mtime` per file, interpreted as UTC Unix epoch seconds. fauxFAT accepts `1980-01-01T00:00:00Z` through `2107-12-31T23:59:59Z` and rejects values outside that interval.

At manufacture time create, last-modified, and last-access timestamps all receive this value. UTC offset bytes are `0x80`. exFAT's packed timestamp stores seconds in two-second units; create/modify `10msIncrement` is `100` for an odd Unix second and `0` for an even second.

### 10.2 Public Stream Extension, type `0xC0`

| Offset | Size | Content |
|---:|---:|---|
| 0 | 1 | `0xC0` |
| 1 | 1 | `0x03` (`AllocationPossible=1`, `NoFatChain=1`) |
| 2 | 1 | zero |
| 3 | 1 | NameLength |
| 4 | 2 | exFAT NameHash |
| 6 | 2 | zero |
| 8 | 8 | `ValidDataLength = DataLength` |
| 16 | 4 | zero |
| 20 | 4 | FirstCluster |
| 24 | 8 | DataLength |

The stream owns exactly `ceil(DataLength / 65536)` contiguous clusters. `ValidDataLength` is manufactured equal to `DataLength` so an in-place overwrite never needs to extend it. Cluster slack after `DataLength` is undefined presentation data and is not included in content preservation.

### 10.3 File Name, type `0xC1`

```text
byte 0      0xC1
byte 1      0x00
bytes 2..   UTF-16LE name, up to 15 code units
unused name positions = 0x0000
```

`NameHash` uses the fauxFAT Up-case Table. Public names must be unique under that folding.

### 10.4 Public EntrySetChecksum

`SetChecksum` uses the normal exFAT rotate/add checksum over all 96 bytes of the public three-entry set, excluding bytes 2 and 3 of the File entry.

### 10.5 Opaque range descriptor

A private range which must be recoverable by a bounded parse has a five-entry root set:

```text
File (0x85)
Stream Extension (0xC0)
File Name (0xC1)
Vendor Extension (0xE0)
Vendor Allocation (0xE1)
```

The first three entries describe an inert zero-length hidden descriptor file. The last two describe the private allocation. Generic exFAT implementations which do not recognize the fauxFAT vendor GUIDs must treat those vendor entries as unrecognized benign secondaries and must not modify their associated allocation during ordinary operation.

The descriptor File entry has:

```text
SecondaryCount = 4
FileAttributes = ReadOnly | Hidden | System = 0x0007
```

Timestamps use the same `time_t` encoding as public files.

Its Stream Extension is deliberately allocation-free:

```text
GeneralSecondaryFlags = 0x01        AllocationPossible=1, NoFatChain=0
ValidDataLength       = 0
FirstCluster          = 0
DataLength            = 0
```

The real exFAT namespace name is an internal deterministic stub:

```text
$FF00000000
$FF00000001
...
```

The eight hex digits are the descriptor ordinal in canonical root order. This name exists only to make the surrounding File entry set valid. It is not the logical private-file name.

#### 10.5.1 fauxFAT opaque-name Vendor Extension

Entry type `0xE0`, with on-disk GUID:

```text
{E2A67221-0B24-41FE-A8AD-A8303DFAA843}
on-disk bytes: 21 72 A6 E2 24 0B FE 41 A8 AD A8 30 3D FA A8 43
```

`GeneralSecondaryFlags = 0`. Its 14-byte `VendorDefined` payload is:

```text
byte 0      logical-name length, 1..15
bytes 1..13 logical-name bytes 0..12, ISO-8859-1, zero-padded
```

The GUID identifies this exact descriptor version, so the payload wastes no separate version byte.

#### 10.5.2 fauxFAT opaque Vendor Allocation

Entry type `0xE1`, with on-disk GUID:

```text
{940DBDEA-CEF9-4CAA-8555-0F60E05BA93C}
on-disk bytes: EA BD 0D 94 F9 CE AA 4C 85 55 0F 60 E0 5B A9 3C
```

Fields:

```text
GeneralSecondaryFlags = 0x03        AllocationPossible | NoFatChain
VendorDefined[0]      = logical-name byte 13, or 0
VendorDefined[1]      = logical-name byte 14, or 0
FirstCluster          = first cluster of the private contiguous range
DataLength            = complete private allocation length
```

`DataLength` is non-zero and cluster-aligned in fauxFAT v1. The corresponding Allocation Bitmap bits remain one. fauxFAT additionally keeps every FAT entry in the range at `0xFFFFFFF7`; because the Vendor Allocation is `NoFatChain`, those FAT entries are not interpreted as a chain by generic implementations.

The 15-byte logical name is therefore reconstructed as:

```text
VendorExtension.VendorDefined[1..13]
+ VendorAllocation.VendorDefined[0..1]
truncated to VendorExtension.VendorDefined[0]
```

This indirection is intentional. A visible staging file and one or more opaque A/B alternatives may all carry the same *logical* name without creating duplicate exFAT namespace names.

The five-entry `SetChecksum` covers all 160 bytes. The descriptor itself is part of the structural seal.

Deleting the hidden descriptor File set is structural damage. The exFAT specification says deletion of a File entry set containing an unrecognized benign secondary also frees that secondary's associated allocation. fauxFAT therefore does not pretend this descriptor is a security boundary. Under the supported mount/read/in-place-write cycle it should remain untouched; if it does not, strict validation fails. Recovery preserves an opaque payload only when a valid descriptor, authoritative private state, or some other explicitly trusted mapping still describes its range.

## 11. Structural seal computation

The OEM XXH32 structural and component fingerprints are computed from disk structures, not from a separate manifest serialization.

### 11.1 Canonical FAT bytes

Hash exactly:

```
(FatEntry[0] through FatEntry[ClusterCount+1])
```

Do not include FAT excess-space bytes.

### 11.2 Canonical bitmap bytes

Hash exactly `ceil(ClusterCount / 8)` bytes of the Allocation Bitmap.

### 11.3 Canonicalized root bytes

Start with a 65536-byte copy of the root cluster. For every active File entry (`0x85`) zero these host-mutable fields before hashing:

```
bytes 2..3     SetChecksum
FileAttributes.Archive bit only
bytes 12..19   LastModifiedTimestamp and LastAccessedTimestamp
byte 21         LastModified10msIncrement
bytes 23..24   LastModifiedUtcOffset and LastAccessedUtcOffset
```

Do not mask:

```
EntryType
SecondaryCount
ReadOnly/Hidden/System/Directory attribute bits
create timestamp, Create10msIncrement, and CreateUtcOffset
Stream Extension bytes
File Name bytes
padding entries
system entries
```

XXH32 the 65536 canonicalized root bytes with seed `0x524F4F54`.

### 11.4 Map XXH32 input

`fauxfat_map_bytes` is the byte concatenation:

```
ASCII "FFMAP1\0\0"                  8 bytes
Boot Sector bytes 64..105           geometry through FileSystemRevision
Boot Sector bytes 108..111          sector/cluster shifts, NumberOfFats, DriveSelect
canonical FAT bytes
meaningful Allocation Bitmap bytes
exact Up-case Table bytes through its DataLength
canonicalized root cluster
```

The current `VolumeFlags` and `PercentInUse` are intentionally absent.

For `N = ClusterCount`, the map input length is `65726 + 4*N + ceil(N/8)` bytes: 65,726 fixed bytes, four FAT bytes per cluster (plus the two reserved FAT entries), and one allocation-bitmap bit per cluster. With 64 KiB clusters this is about 66 KiB per GiB of volume plus the fixed ~64 KiB root/geometry overhead. A ~4 GiB volume therefore hashes about 328 KiB; a 32 GiB volume hashes about 2.1 MiB. Payload bytes are never included.

Compute one XXH32 over that exact byte stream with seed `0x00000000`. Store the 32-bit result in OEM Parameter 0. The seal is only a non-adversarial structural fingerprint; the verifier also checks the structural invariants and exFAT-native checksums directly, so fauxFAT deliberately does not spend another 32 bits on a second map hash.

This seal detects every change to physical extent ownership, file positions/sizes/names, FAT bad-cluster reservations, root slot use, the fauxFAT case-folding table, filesystem metadata chains, and geometry while tolerating ordinary timestamp/archive updates.

## 12. Physical allocation policy

The disk format does not require one high-level object policy, but v1 lays out the usable heap in a deliberately simple direction:

```
low cluster numbers
    Allocation Bitmap
    Up-case Table
    root
    large host-visible bulk extent(s)
    ...
    tail allocations carved downward
    opaque/private/version storage
    reserve
high cluster numbers
```

The large bulk file begins at `DataFirstCluster` and grows upward. The reserve/private tail begins at the last cluster and grows downward.

To create another small predefined file from the tail while the card is private:

1. choose a contiguous tail range currently marked `0xFFFFFFF7`;
2. change only those FAT entries to `0x00000000`;
3. keep all Allocation Bitmap bits at `1`;
4. replace one three-entry padding slot with a valid `0x85/0xC0/0xC1` file set pointing at that range;
5. increment the structural epoch;
6. recompute FAT/root/bitmap/upcase XXH32 component fingerprints and the map fingerprint;
7. update both OEM sectors and both boot checksums;
8. validate the rebuilt façade before exposing it.

To retire that file, reverse the operation: convert its extent back to `0xFFFFFFF7`, turn its three root entries back into padding, and reseal.

The same operation may shorten the high end of a large bulk extent and hand the released clusters to new tail files. No Allocation Bitmap change is required because all clusters remain unavailable throughout.

## 13. A/B and continuously versioned data on the same volume

Nothing special is required in the disk format.

A staged A/B object can use two fixed raw extents:

```
visible staging slot       FAT = 0, named file entry exists
current/private slot       FAT = 0xFFFFFFF7, opaque descriptor may name the range
```

After validation firmware may swap their roles with one structural update. The optional dual-view exFAT trick may later make this swap cheaper, but it is not part of fauxFAT v1.

A continuously versioned configuration object can simultaneously use:

```
small visible edit file    FAT = 0, named file entry exists
private version store      FAT = 0xFFFFFFF7, opaque descriptor may name the range
```

The version store format is outside fauxFAT. fauxFAT only reserves its physical range from the host.

## 14. Host mutation contract

While the volume is host-owned, the only intended data operation is overwriting bytes inside existing file extents without changing their length.

The following metadata changes are accepted after validating the File entry-set checksum:

```
Main Boot Sector `VolumeFlags.VolumeDirty` toggle
Backup Boot Sector `VolumeFlags` and `PercentInUse`: stale, ignored except for valid-range checks
FileAttributes.Archive
LastModifiedTimestamp
LastAccessedTimestamp
LastModified10msIncrement
LastModifiedUtcOffset
LastAccessedUtcOffset
corresponding File SetChecksum
```

Main Boot `PercentInUse` remains exactly `100`: the fauxFAT Allocation Bitmap is saturated and ordinary in-place file writes neither allocate nor free clusters. exFAT revision 1.00 defines valid `PercentInUse` values as 0..100; `0xff` is not part of this profile.

Everything else is incompatible, including:

```
FAT changes
Allocation Bitmap changes
file FirstCluster changes
DataLength or ValidDataLength changes
NoFatChain changes
name changes
new/deleted/moved directory entries
padding changes
opaque descriptor File/Vendor Extension/Vendor Allocation changes outside the timestamp/archive mask
root-chain growth
up-case changes
boot geometry changes
OEM seal changes not made by firmware
NumberOfFats != 1
TexFAT state
```

Failure of this strict mutation contract means the presentation is no longer accepted as intact fauxFAT. Product recovery returns to authoritative private state and regenerates the façade. An optional loose scanner may still enumerate simple contiguous root files for diagnostics or salvage, but it never upgrades a strict failure into trusted fauxFAT state.

## 15. Validation order after host use

Strict validation performs this bounded sequence:

1. validate Main and Backup Boot Checksums independently; treat Backup `VolumeFlags` and `PercentInUse` as stale;
2. require exact fauxFAT v1 geometry and one FAT;
3. validate the Allocation Bitmap and Up-case root entries;
4. verify the fauxFAT Up-case checksum/content;
5. verify the root is exactly one cluster and FAT-chained EOC;
6. verify every root entry is one of the expected system entries, public file sets, fauxFAT opaque descriptor sets, or canonical `0xA1` padding;
7. verify each public/opaque File set checksum and permit only the host-mutable timestamp/archive fields listed above;
8. recompute FAT, bitmap, root, and upcase XXH32 component fingerprints and the map fingerprint;
9. compare those values to OEM Parameters;
10. only then inspect candidate file payloads.

The optional loose scanner is a separate acceptance level. It may scan the same bounded one-cluster root, skip valid-but-unsupported entries, and return only regular root files whose Stream Extension directly proves a contiguous `NoFatChain` allocation. It aborts on malformed/ambiguous structures and never walks a FAT chain or descends into directories. A volume which still satisfies the strict fauxFAT seal is reported as fauxFAT-valid even when reached through the loose API; recognizable fauxFAT with a failed structural seal is reported as changed, not valid.

There is no general path lookup, cluster allocator, directory repair, orphan recovery, free-space reconstruction, arbitrary FAT-chain traversal, TexFAT handling, intent-log interpretation, or journal replay. If some future host behavior would require any of those for strict acceptance, fauxFAT rejects the image instead of learning another filesystem feature.

## 16. Important qualification points

The format is intentionally legal-but-hostile. Before treating it as product behavior, qualify at least:

- Windows 10 and 11 native exFAT;
- ordinary Explorer mount/eject and file overwrite;
- the tiny supported updater doing `OPEN_EXISTING` in-place writes;
- antivirus/indexing/filter stacks likely to be encountered;
- Linux exFAT if cards may be handled there;
- `chkdsk` only to document how it damages/reclassifies the deliberately fake bad clusters. `chkdsk` is not an accepted writer.

The central empirical question is whether desktop exFAT implementations leave the manufactured bad-cluster ranges, fauxFAT Vendor Extension/Vendor Allocation descriptor sets, and `0xA1` root padding alone during ordinary mount/write/unmount. The base compatibility rules say unknown benign vendor secondaries and their allocations should survive that cycle. Product qualification gets the final vote, because storage software enjoys interpretive dance.
