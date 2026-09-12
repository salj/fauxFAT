# fauxFAT v1 on-disk format

This is the on-disk contract for fauxFAT v1. It defines the exFAT layout, file extents, and host-allowed changes.

The profile follows Microsoft exFAT 1.00.

## 1. Required behavior

A fauxFAT volume shall have these properties:

1. Every cluster is unavailable for allocation from the instant the volume is manufactured.
2. A fixed set of host-visible files exists in fixed root-directory slots. Each file is a contiguous `NoFatChain` extent.
3. An ordinary host may overwrite bytes inside an existing file without changing allocation metadata.
4. The root directory has no free directory entries and cannot grow.
5. Project-private and reserved ranges have no directory entry at all. They are represented to exFAT as bad clusters.
6. Structural state is checksummed in the OEM Parameters sectors. Any structural mutation outside the explicit host-mutable fields is incompatible.
7. On incompatible mutation the firmware does not repair arbitrary exFAT. It discards/rebuilds the façade from authoritative private state.

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
up-case table             recommended exFAT table, 5836 bytes
up-case checksum          0xE619D30D
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
| 112 | 1 | `100`, or host-written `0xff` accepted |
| 113 | 7 | zero |
| 120 | 390 | `0xF4` |
| 510 | 2 | `55 AA` |

Only the **Main** Boot Sector carries current host-volatile state:

```
VolumeFlags.VolumeDirty
PercentInUse, accepted only as 100 or 0xff
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

### 5.1 Parameter 0: map SHA-256

GUID:

```
{995BE6E7-3445-46DC-A213-74D985B30134}
on-disk bytes: E7 E6 5B 99 45 34 DC 46 A2 13 74 D9 85 B3 01 34
```

Its 32-byte `CustomDefined` payload is:

```
SHA256(fauxfat_map_bytes)
```

`fauxfat_map_bytes` is defined in section 11. It is derived directly from the on-disk boot geometry, FAT, Allocation Bitmap, and root directory after masking the explicitly host-mutable file metadata.

### 5.2 Parameter 1: epoch and component CRCs

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
| 16 | 4 | CRC32C of canonical FAT bytes |
| 20 | 4 | CRC32C of Allocation Bitmap meaningful bytes |
| 24 | 4 | CRC32C of canonicalized root cluster |
| 28 | 4 | CRC32C of bytes 0..27 of this payload |

All CRC fields use CRC-32C/Castagnoli. The SHA field uses ordinary SHA-256.

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

There is no directory entry for such a range.

This is the core fauxFAT reservation trick. exFAT itself treats `0xFFFFFFF7` as a bad cluster, and the Allocation Bitmap marks it unavailable. Firmware is knowingly lying about media health and may use those physical clusters for littlefs, A/B inactive copies, version stores, raw databases, or future reserve space.

`VolumeFlags.MediaFailure` remains zero. In exFAT, zero is valid when known failures have already been represented as bad clusters in the FAT.

This avoids depending on a deletable anchor file or on inventing a manufacturer-defined primary directory-entry type. It also means generic disk diagnostics may report absurd quantities of bad space. Such diagnostics are not part of the supported write protocol; if they rewrite the map, the OEM seal fails.

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

Every meaningful bitmap bit is `1`. There are no free clusters in a mounted fauxFAT volume. Unused high bits in the final byte and bytes after `BitmapBytes` in the final cluster are zero.

The bitmap therefore never changes during ordinary host operation or during fauxFAT range reclassification. A bitmap change is always structural corruption/incompatibility.

## 8. Up-case Table and root cluster placement

Immediately after the bitmap chain:

```
UpcaseCluster = 2 + BitmapClusters
RootCluster   = UpcaseCluster + 1
DataFirstCluster = RootCluster + 1
```

The Up-case Table occupies one cluster. Its root entry is type `0x82` with:

```
TableChecksum = 0xE619D30D
FirstCluster  = UpcaseCluster
DataLength    = 5836
```

The table contents are exactly the recommended compressed exFAT 1.00 Up-case Table. Bytes 5836..65535 in the cluster are zero. `FAT[UpcaseCluster] = 0xFFFFFFFF`.

The root occupies exactly one cluster. `FAT[RootCluster] = 0xFFFFFFFF`.

## 9. Root directory layout

The root cluster contains exactly 2048 entries of 32 bytes each. No entry is unused and there is no end-of-directory marker.

Canonical ordering:

```
entry 0        Allocation Bitmap (0x81)
entry 1        Up-case Table (0x82)
entry 2        Volume Label (0x83)
entry 3        Volume GUID (0xA0)
entries 4..    681 fixed 3-entry file slots
final entry    padding
```

`(2048 - 4) / 3 = 681` complete short-name file slots with one directory entry left over.

A product may use fewer than 681 files. An unused file slot is still three occupied padding entries. Host software therefore has no directory slot it may legally claim.

### 9.1 Volume Label

Entry 2 is a normal `0x83` Volume Label. The product chooses one fixed label of at most 11 UTF-16 code units. `RP UPDATE` is the current suggested label.

### 9.2 Volume GUID

Entry 3 is a normal `0xA0` Volume GUID entry with `SecondaryCount = 0`, `GeneralPrimaryFlags = 0`, and a stable product-generated GUID. Its one-entry `SetChecksum` is valid.

Keeping this GUID stable across façade regeneration encourages the host to regard the rebuilt volume as the same volume.

### 9.3 Padding entry

Every directory entry not currently used by a real file set is a one-entry TexFAT Padding record:

```
byte 0      0xA1
byte 1      0x00        SecondaryCount
bytes 2..3  0x0508 LE   SetChecksum for the canonical all-zero body
bytes 4..31 0x00
```

The base exFAT 1.00 specification says `0xA1` TexFAT Padding must be treated like an unrecognized benign primary entry and must not be moved. fauxFAT uses it only as namespace padding, never as an allocation owner.

Three consecutive padding entries form one inactive file slot. Firmware may turn an inactive slot into a file set only while the host does not own the volume.

## 10. Host-visible file entry set

fauxFAT v1 restricts host-visible names to 1..15 uppercase ASCII characters from:

```
A-Z 0-9 _ - .
```

Thus every file consumes exactly three directory entries.

### 10.1 File entry, type `0x85`

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

Create time is structural for fauxFAT and is initialized by the formatter. Last-modified and last-access metadata are host-volatile.

### 10.2 Stream Extension, type `0xC0`

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

`DataLength` is the fixed host-visible capacity, not the current logical payload length. It never changes during a host session. The stream owns exactly `ceil(DataLength / 65536)` clusters, and that count **must equal** the size of the assigned contiguous extent. There may not be unowned slack clusters hidden after the logical end of a file. Large files should therefore normally use cluster-multiple `DataLength` values.

`ValidDataLength` is manufactured equal to `DataLength` so an in-place overwrite never needs to extend the valid range. The extent must therefore contain initialized bytes before first exposure.

### 10.3 File Name, type `0xC1`

```
byte 0      0xC1
byte 1      0x00
bytes 2..   UTF-16LE file name, up to 15 code units
unused name positions = 0x0000
```

`NameHash` is the normal exFAT 16-bit rotate/add hash over the uppercase UTF-16LE name.

### 10.4 EntrySetChecksum

`SetChecksum` uses the normal exFAT rotate/add checksum over all 96 bytes of the three-entry set, excluding bytes 2 and 3 of the File entry.

A host is expected to rewrite this checksum when it changes timestamps or the Archive bit. fauxFAT validates the checksum before accepting the entry set.

## 11. Structural seal computation

The OEM SHA-256 and component CRCs are computed from disk structures, not from a separate manifest serialization.

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

Then CRC32C the 65536 canonicalized root bytes.

### 11.4 Map SHA-256 input

`fauxfat_map_bytes` is the byte concatenation:

```
ASCII "FFMAP1\0\0"                  8 bytes
Boot Sector bytes 64..105           geometry through FileSystemRevision
Boot Sector bytes 108..111          sector/cluster shifts, NumberOfFats, DriveSelect
canonical FAT bytes
meaningful Allocation Bitmap bytes
canonicalized root cluster
```

The current `VolumeFlags` and `PercentInUse` are intentionally absent.

The SHA-256 of that exact byte stream is OEM Parameter 0.

This seal detects every change to physical extent ownership, file positions/sizes/names, FAT bad-cluster reservations, root slot use, filesystem metadata chains, and geometry while tolerating ordinary timestamp/archive updates.

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
6. recompute FAT/root CRCs and map SHA-256;
7. update both OEM sectors and both boot checksums;
8. validate the rebuilt façade before exposing it.

To retire that file, reverse the operation: convert its extent back to `0xFFFFFFF7`, turn its three root entries back into padding, and reseal.

The same operation may shorten the high end of a large bulk extent and hand the released clusters to new tail files. No Allocation Bitmap change is required because all clusters remain unavailable throughout.

## 13. A/B and continuously versioned data on the same volume

Nothing special is required in the disk format.

A staged A/B object can use two fixed raw extents:

```
visible staging slot       FAT = 0, named file entry exists
current/private slot       FAT = 0xFFFFFFF7, no directory entry
```

After validation firmware may swap their roles with one structural update. The optional dual-view exFAT trick may later make this swap cheaper, but it is not part of fauxFAT v1.

A continuously versioned configuration object can simultaneously use:

```
small visible edit file    FAT = 0, named file entry exists
private version store      FAT = 0xFFFFFFF7, no directory entry
```

The version store format is outside fauxFAT. fauxFAT only reserves its physical range from the host.

## 14. Host mutation contract

While the volume is host-owned, the only intended data operation is overwriting bytes inside existing file extents without changing their length.

The following metadata changes are accepted after validating the File entry-set checksum:

```
Main Boot Sector `VolumeFlags.VolumeDirty` toggle
Main Boot Sector `PercentInUse`: 100 <-> 0xff
Backup Boot Sector `VolumeFlags` and `PercentInUse`: stale, ignored except for valid-range checks
FileAttributes.Archive
LastModifiedTimestamp
LastAccessedTimestamp
LastModified10msIncrement
LastModifiedUtcOffset
LastAccessedUtcOffset
corresponding File SetChecksum
```

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
root-chain growth
up-case changes
boot geometry changes
OEM seal changes not made by firmware
NumberOfFats != 1
TexFAT state
```

An incompatible volume is never interpreted more deeply in an attempt to infer intent. Firmware returns to authoritative private state and regenerates fauxFAT.

## 15. Validation order after host use

Firmware performs this bounded validation only:

1. validate Main and Backup Boot Checksums independently; treat Backup `VolumeFlags` and `PercentInUse` as stale;
2. require exact fauxFAT v1 geometry and one FAT;
3. validate the Allocation Bitmap and Up-case root entries;
4. verify the standard Up-case checksum;
5. verify the root is exactly one cluster and FAT-chained EOC;
6. verify every root entry is one of the expected system entries, file slots, or canonical `0xA1` padding;
7. verify each active file set checksum and permit only the host-mutable fields listed above;
8. recompute FAT CRC, bitmap CRC, canonical root CRC, and map SHA-256;
9. compare those values to OEM Parameters;
10. only then inspect candidate file payloads.

There is no general path lookup, cluster allocator, directory repair, orphan recovery, free-space reconstruction, arbitrary FAT-chain traversal, TexFAT handling, intent-log interpretation, or journal replay. If some future host behavior would require any of those, fauxFAT rejects the image instead of learning another filesystem feature.

## 16. Important qualification points

The format is intentionally legal-but-hostile. Before treating it as product behavior, qualify at least:

- Windows 10 and 11 native exFAT;
- ordinary Explorer mount/eject and file overwrite;
- the tiny supported updater doing `OPEN_EXISTING` in-place writes;
- antivirus/indexing/filter stacks likely to be encountered;
- Linux exFAT if cards may be handled there;
- `chkdsk` only to document how it damages/reclassifies the deliberately fake bad clusters. `chkdsk` is not an accepted writer.

The central empirical question is whether desktop exFAT implementations leave the manufactured bad-cluster ranges and `0xA1` root padding alone during ordinary mount/write/unmount. The base specification says they should. Product qualification gets the final vote, because storage software enjoys interpretive dance.
