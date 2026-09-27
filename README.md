# fauxFAT

An exFAT-shaped façade for devices where the host gets a few writable files,
but does not get to rearrange the storage underneath them. It is a carefully
maintained filesystem-shaped fiction. The firmware knows where the props are.

The same fixed volume does two jobs:

- **Virtual projection, used as a protocol.** Give fauxFAT's synthesized
  sectors to a caller's USB Mass Storage Class implementation. The host sees
  predefined files; reads and writes are the message exchange. Payload I/O
  goes to application callbacks, so there need not be a fauxFAT image sitting
  on a disk. fauxFAT supplies the block view; the USB glue is yours.
- **Real media, used as a formatter and almost-filesystem.** Materialize that
  volume on flash or a disk, optionally with GPT and a separate user partition.
  fauxFAT can then validate it, reopen it, recover bounded file descriptors,
  and read or write their physical ranges. This is enough filesystem to be
  useful, and enough exFAT to know when to stop pretending it is general.

## How the trick works

- **There is no room to improvise.** Every meaningful cluster is marked
  allocated. The one-cluster root directory is full of real entries and
  padding. No free clusters, no spare directory slots, no host creativity.
- **Files have assigned seats.** They use contiguous `NoFatChain` extents.
  Block reads and writes map those ranges to callbacks owned by the
  application; writes elsewhere are rejected.
- **Private storage cosplays as damaged media.** Private and reserved clusters
  carry exFAT's `0xFFFFFFF7` bad-cluster marker. The host is told to stay out;
  firmware uses the space anyway. Disk diagnostics may be unimpressed, and a
  raw writer can still ignore the sign.
- **The metadata leaves fingerprints.** fauxFAT stores XXH32 structural and
  component hashes in the exFAT OEM area. They catch accidental edits and
  unsupported host changes. They are not authentication; an attacker with raw
  writes can update the hashes too.
- **Recovery reads the clues, not the whole filesystem.** Vendor entries keep
  logical names and named opaque-range descriptors on disk. A bounded parser
  can recover physical extents without the application's old schema. It does
  not walk arbitrary FAT chains, recurse through directories, or repair exFAT.
- **Regeneration tries not to trample the set.** fauxFAT can rebuild canonical
  metadata while preserving selected payload and private ranges. The GPT
  wrapper can do the same around a separate user-data partition.

This is a deliberately small exFAT profile: fixed geometry, contiguous files,
one fixed-size root directory.

## Build

```sh
make          # build the test programs
make test-all # run the test suite
```

The code is C99 and intended to be linked into firmware. There is no install
target.

## Docs

- [Overview](docs/fauxfat-overview.md)
- [On-disk format](docs/fauxfat-disk-format.md)
- [GPT partition layout](docs/fauxfat-partition-layout.md)
- [Programmer's guide](docs/fauxfat-usage.md)

Windows VHDX qualification has been exercised. Linux/macOS and real-media
durability still need qualification.
