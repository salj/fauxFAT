# fauxFAT SD-card partition layout

This note describes the GPT layout for a fauxFAT volume and an optional user-data partition.

## 1. Recommendation

Use a normal GPT disk with a protective MBR.

The canonical card layout is:

```text
LBA 0                 protective MBR
LBA 1                 primary GPT header
LBA 2..33             primary GPT entry array (128 x 128-byte entries)
...
partition 1           fauxFAT, Microsoft Basic Data GUID
alignment gap         optional, normally zero after planning
partition 2           blank user-data partition, Microsoft Basic Data GUID
...
last-33 .. last-2     backup GPT entry array
last LBA              backup GPT header
```

Partition 1 has a fixed application-selected capacity and starts at a stable aligned LBA. Partition 2 consumes the remaining usable capacity when ordinary host storage is desired. Partition 2 is deliberately published without a filesystem; the firmware leaves its body alone and the user may format it with the host OS if they want the space.

Both host-visible partitions use the Microsoft Basic Data partition type GUID:

```text
EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
```

No GPT attributes are set in the normal layout. In particular, fauxFAT is not an ESP, recovery partition, hidden partition, shadow copy, read-only partition, or no-drive-letter partition. Its entire purpose is to be mounted and written by ordinary consumer OS filesystem code.

The GPT partition names are informational only. Application identity comes from fauxFAT's own OEM identity and structural seal, not from a mutable GPT name.

## 2. Why GPT is the default

GPT costs almost nothing on an SD card and has several properties that match the rest of this design:

- 64-bit LBAs, so the layout remains usable for SDUC media above 2 TiB;
- primary and backup partition tables;
- CRC32 protection of both headers and partition-entry arrays;
- stable per-disk and per-partition GUIDs;
- explicit type GUIDs and partition names;
- no four-primary-partition limit;
- a standard protective MBR for old software which only understands MBR enough to avoid treating the disk as empty.

The required partition-entry array is 16 KiB. With 512-byte logical blocks the conventional 128-entry GPT therefore consumes only 34 sectors at the front and 33 sectors at the back, including headers and the protective MBR. Saving tens of KiB by inventing a smaller table would be filesystem-goblin economics.

GPT also gives us a clean narrow validator. The firmware does not need a general partition manager: it can validate one protective MBR, two GPT headers, the fixed 16 KiB entry arrays, and the small number of partition entries it expects.

## 3. MBR remains a deliberate compatibility fallback

A legacy MBR renderer is reasonable if qualification finds equipment that must read the card but does not understand GPT.

For cards at or below the 32-bit-LBA limit, an MBR profile can expose the same two partitions using type `0x07` for exFAT/basic data. The second partition may remain unformatted there as well. It is extremely cheap to generate and widely understood.

It is not the canonical format because it has no redundant table, no checksums, no stable GUID identity, only four primary entries, and a 32-bit LBA ceiling. At 512-byte sectors that ceiling is approximately 2 TiB, exactly where SDUC begins.

Do not implement a hybrid GPT/MBR. The only MBR on a GPT disk is the standards-defined protective MBR with one `0xEE` entry. Maintaining two simultaneously authoritative partition maps would recreate the sort of ambiguity fauxFAT exists to avoid.

A partitionless "superfloppy" layout is also rejected for the canonical card because it prevents recovering the otherwise unused capacity as a second ordinary volume.

## 4. Partition ordering

Put fauxFAT first and the user-owned data partition second.

This gives fauxFAT a stable physical base independent of card capacity:

```text
first usable aligned LBA
    -> fauxFAT fixed-size partition
    -> align up
    -> blank user-data partition occupying the remainder
```

The blank user partition can grow with whatever card the user installs without moving fauxFAT or changing any application raw-block assumptions. Once a host formats it, that filesystem is entirely user-owned and remains opaque to firmware.

There is also a legacy benefit: old removable-media stacks which expose only one partition will expose the purpose-built ingress volume rather than the optional convenience storage. Current desktop systems should expose both.

Do not depend on drive-letter order or mount order. Human-facing software should identify fauxFAT by its volume label and, where practical, confirm the fauxFAT OEM identity before offering an update operation.

## 5. Alignment

The minimum alignment is 1 MiB, i.e. 2048 512-byte sectors.

If the SD card reports a larger Allocation Unit (AU), use the larger of 1 MiB and the reported AU size as the partition alignment. The SD physical specification describes the AU as a physical boundary in the card's user area and recommends that hosts manage data areas in AU units. On modern high-capacity cards the AU can be much larger than 1 MiB.

For a chosen alignment `A` blocks:

```text
fauxfat_first = align_up(first_usable_lba, A)
fauxfat_last  = fauxfat_first + fauxfat_blocks - 1
data_first    = align_up(fauxfat_last + 1, A)
data_last     = last_usable_lba
```

Prefer to make the fauxFAT volume itself an aligned size rather than placing dead sectors after it. fauxFAT already supports anonymous opaque tail reserve, so its `data_cluster_count` can be rounded upward until `fauxfat_block_count()` is an alignment multiple. With the fixed 64 KiB fauxFAT cluster size, 1 MiB alignment is only a 16-cluster quantum.

This reserve is not wasted: the application can later carve it into named opaque objects without changing the partition table.

## 6. Host compatibility

The target is current consumer operating systems, not arbitrary cameras, game consoles, printers, or other appliances that happen to have an SD slot.

### Windows

Current Windows supports GPT on detachable disks. Microsoft documents multiple partitions on removable USB media for Windows 10 version 1703 and later. Windows exposes a GPT partition as ordinary data storage when its type is Microsoft Basic Data, which is why both host-visible partitions use that GUID.

A zero-attribute Basic Data fauxFAT partition therefore follows the normal mount path. The second Basic Data partition is intentionally RAW/unformatted at manufacture. Windows can format such a partition through its normal UI/tooling and will commonly offer to do so when the user tries to access it. That host behavior is convenience, not a firmware contract: our requirement is merely to publish sane partition bounds and never overwrite the partition body. Do not set `HIDDEN`, `NO_DRIVE_LETTER`, `READ_ONLY`, `SHADOW_COPY`, or similar attributes in the ordinary layout.

Older Windows compatibility is the principal reason to retain an optional MBR renderer. It is not a reason to weaken the canonical format in 2026.

### macOS

Disk Utility supports both GUID Partition Map and Master Boot Record schemes and can format ordinary external partitions as exFAT or other supported filesystems. Multiple partitions on an external physical disk are normal Disk Utility objects.

GPT + Basic Data is therefore conventional enough for current macOS even though the second partition starts unformatted. Apple still presents MBR as the maximum-legacy-Windows compatibility choice, which again argues for MBR as an optional compatibility format rather than as the default.

### Linux

Normal Linux block tooling understands GPT and can create it directly with common partitioning tools. The fauxFAT volume itself is exFAT 1.00 and is presented through the normal exFAT filesystem path. Nothing in the proposed GPT layout requires Linux-specific partition types.

### Android

Current AOSP `vold` parses both MBR and GPT removable disks. For GPT it creates public volumes for Microsoft Basic Data partitions; for MBR it recognizes the conventional FAT/exFAT partition types including `0x07` in current code. This makes the proposed GPT layout structurally compatible with current AOSP.

OEM Android builds are less predictable than desktop operating systems, so this remains a qualification target rather than a promise that every phone UI will expose both volumes sanely.

### SD-device interoperability

The SD Association convention is FAT12/16 for SDSC, FAT32 for SDHC, and exFAT for SDXC/SDUC. The official formatter is explicitly intended to produce SD-standard layouts optimized for cards.

A multi-partition fauxFAT appliance card is deliberately outside the "stick this in any camera" interchange profile. Partition 1 remains exFAT-compatible fauxFAT and we respect card AU alignment; partition 2 has no filesystem until the user chooses one. Generic consumer-electronics compatibility is not a design goal unless testing creates a specific requirement.

## 7. Blank user-data second partition

Partition 2 exists to avoid wasting the rest of a large card. Firmware does not need to decide which general-purpose filesystem the user wants, and it does not need another formatter merely to be helpful.

The canonical behavior is therefore deliberately boring:

```text
create GPT entry 2 as Microsoft Basic Data
leave every sector in partition 2 untouched
publish the GPT
let the host/user format partition 2 if desired
```

A newly provisioned card may contain arbitrary old bytes in that range. Those bytes are not interpreted as a filesystem and are not part of device authority. If product provisioning wants a cleaner user experience it may erase the range out of band, but the GPT generator must not depend on that and must never synthesize a filesystem there.

Once a host formats partition 2, its contents become user-owned. Subsequent fauxFAT/GPT regeneration must preserve the partition bounds and body unless the user explicitly requested a destructive whole-card reformat. Firmware should not mount it, scan it, resize it, repair it, or infer application state from it.

This removes the proposed format-only exFAT component entirely. The card may cease to match SD Association single-filesystem conventions after the user formats the second partition, but it was already an appliance-specific multi-partition layout; pretending otherwise buys us code and obligations, not compatibility.

## 8. Narrow GPT generator

The generator is a small whole-disk companion to fauxFAT, not a generic partition library.

The implemented `fauxgpt` companion is a renderer/writer rather than a general partition manager. For the canonical profile it emits exactly:

```text
1 x protective MBR
1 x primary GPT header
32 x primary entry-array sectors
32 x backup entry-array sectors
1 x backup GPT header
```

Use 128 entries of 128 bytes each even though only two are normally populated. Unused entries are zero.

Its public input is intentionally close to the following shape:

```c
struct fauxgpt_partition {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t block_count;
    uint64_t attributes;
    const char *name;          /* restricted to simple ASCII by our API */
};

struct fauxgpt_layout {
    uint64_t disk_blocks;      /* 512-byte logical blocks */
    uint8_t  disk_guid[16];
    const struct fauxgpt_partition *partitions;
    size_t partition_count;
};
```

The generator accepts raw on-disk GUID bytes rather than inventing identifiers or parsing UUID strings. The product can preserve disk/partition GUIDs across a reformat, and GUID generation policy remains outside this low-level module.

Names are restricted to printable ASCII and encoded as UTF-16LE into GPT's 36-code-unit field. We have no reason to acquire a Unicode subsystem merely to make Disk Management prettier.

## 9. CRC and memory requirements

GPT requires IEEE CRC-32 for:

- the complete partition-entry array;
- the primary header;
- the backup header.

This is not the CRC32C that was removed from fauxFAT. Implement a tiny streaming IEEE CRC-32 locally in the GPT module. Formatting is rare, so a tableless implementation is perfectly acceptable.

No 16 KiB entry-array buffer is required. `fauxgpt_init()` synthesizes each 512-byte entry-array sector once to compute the CRC, and `fauxgpt_render_block()`/`fauxgpt_format()` synthesize sectors again when they are emitted:

```text
init: synthesize sectors and feed CRC32
write: synthesize sectors again as they are emitted
```

The same partition-array CRC is used in both GPT headers. Peak scratch remains one 512-byte block plus CRC/header state. `fauxgpt_format()` writes only GPT metadata and never issues a write inside either partition body.

## 10. Write and repair ordering

GPT is presentation metadata, not the authority for application state. The device should retain the intended layout elsewhere or be able to derive it from configuration.

When materializing a new layout, materialize any firmware-owned partition contents before publishing the GPT entries that expose them. In the canonical two-partition card this means fauxFAT partition 1; partition 2 is intentionally not formatted or touched.

For GPT itself, write the backup copy before the primary copy:

```text
write backup entry array
write backup header
flush
write primary entry array
write primary header
write/repair protective MBR
flush
```

This does not make an arbitrary layout change transactionally atomic, but it makes it difficult to lose both GPT copies in one interrupted update. If the primary and backup disagree after a crash, firmware should reconstruct the intended table from authoritative product state rather than attempt a general-purpose GPT merge.

A product-side validator/repair pass should remain similarly bounded; that parser is not part of the current `fauxgpt` slice:

- validate protective MBR shape;
- validate primary header and entry-array CRC;
- validate backup header and entry-array CRC;
- require the expected fixed geometry and expected partition entries;
- if only one copy is valid, repair the other from authoritative layout;
- if both are valid but disagree, report structural change and rebuild from authority.

There is no need to implement partition insertion, deletion, resize heuristics, hybrid-MBR interpretation, or filesystem discovery in this module.

## 11. Relationship to fauxFAT recovery

The partition layer and fauxFAT recovery are deliberately separate.

A normal boot can do:

```text
validate narrow GPT profile
    -> locate fauxFAT partition 1
    -> fauxfat_reopen() / strict validation
    -> recover/preserve understood payload descriptors as needed
```

A damaged GPT does not imply damaged fauxFAT payloads. Because partition 1 normally has a deterministic aligned base and fixed capacity, product authority can reconstruct the GPT without touching the partition bodies.

Conversely, a damaged fauxFAT volume does not require rewriting GPT. Its in-place regeneration remains wholly inside partition 1.

This separation also makes the optional second metadata/checkpoint copy discussed elsewhere less interesting: GPT already gives us one standard redundant metadata layer at the whole-disk boundary, while fauxFAT can regenerate its own metadata from descriptors and authoritative application state.

## 12. Qualification matrix

Before calling the GPT profile boring enough to ship, test at least:

| Host | GPT fauxFAT only | GPT fauxFAT + blank user partition | MBR fallback |
| --- | --- | --- | --- |
| Windows 10 22H2 | mount/write/eject/reinsert | RAW partition visible; format it; both survive reinsert | mount/write |
| Windows 11 current | same | same | same |
| macOS current | same | partition visible in Disk Utility; format and remount | same |
| Linux current | same | partition node visible; format/mount normally | same |
| representative Android/AOSP-ish device | detect/mount | behavior with second unformatted Basic Data partition | detect/mount |

For each host, also inspect whether it rewrites GPT names, attributes, partition GUIDs, entry ordering, or backup tables during ordinary filesystem use. After formatting partition 2, regenerate/repair GPT metadata and verify that every partition-2 sector remains untouched and the host filesystem still mounts. It should, but this is removable storage and optimism has already had enough turns at the controls.

The same test should exercise common USB and built-in SD readers. The OS sees a block device either way, but removable-media policy and driver stacks have historically found opportunities to be special.

## References

- UEFI Specification, GPT disk layout: https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html
- Microsoft, Windows and GPT FAQ: https://learn.microsoft.com/windows-hardware/manufacture/desktop/windows-and-gpt-faq
- Microsoft, GPT Basic Data partition information: https://learn.microsoft.com/windows/win32/api/winioctl/ns-winioctl-partition_information_gpt
- Microsoft, multiple-partition removable USB media: https://learn.microsoft.com/windows-hardware/manufacture/desktop/winpe--use-a-single-usb-key-for-winpe-and-a-wim-file
- Apple, partition schemes in Disk Utility: https://support.apple.com/guide/disk-utility/partition-schemes-dsku1c614201/mac
- Apple, Windows-compatible external formats: https://support.apple.com/guide/disk-utility/dskutl1010/mac
- SD Association, capacity/filesystem conventions: https://www.sdcard.org/developers/sd-standard-overview/capacity-sd-sdhc-sdxc-sduc/
- SD Association, SD Memory Card Formatter: https://www.sdcard.org/downloads/formatter/
- AOSP vold removable-disk parser: https://android.googlesource.com/platform/system/vold/+/master/model/Disk.cpp
