# Migrating the INLINE path to iomap

Date: 2026-08-29
Branch: `iomap-migration` (private remote only)
Status: in progress

## Why

Matthew Wilcox asked, reviewing the RFC v2 series, that the read and
write paths move off buffer_head and onto iomap. v3 answered by
migrating `file.c` -- `beamfs_iomap_begin/end`, `iomap_file_buffered_write`,
`iomap_fiemap`.

Development then continued on `file_inline.c`, which carries the
RS(255,239) inline format and 59 buffer_head call sites, and that file
became the active path: `inode.c` and `namei.c` both install
`beamfs_inline_file_operations` for regular files under the current
scheme. The migrated path is now the secondary one, kept for
`scheme=5 INODE_UNIVERSAL`.

A reviewer looking at v4 would see the request satisfied on a path that
no longer runs. That is not an answer.

## What made it look impossible

The INLINE format interleaves data and parity inside each 4096-byte
block: 16 subblocks of 239 data + 16 parity. 3824 logical bytes occupy
4096 physical bytes, so file offset and disk offset are not related by
addition. `iomap_sector()` computes

    (iomap->addr + pos - iomap->offset) >> SECTOR_SHIFT

which assumes exactly that affine relation. Reading 100 bytes at offset
0 means reading the whole block, decoding sixteen RS codewords,
extracting 3824 usable bytes and returning 100 of them. iomap maps
ranges; it does not transform bytes between disk and page cache.

Three things were checked before concluding:

- `IOMAP_INLINE` lets the filesystem hand over a data pointer, but
  `iomap_read_inline_data()` computes `size = i_size - iomap->offset`
  and the header says "only a single IOMAP_INLINE extent is allowed at
  the end of each file". It is built for small files living inside the
  inode, not for a whole file of interleaved blocks.

- `IOMAP_F_BUFFER_HEAD` works and is honoured in
  `iomap_write_begin`/`iomap_write_end`, but one filesystem uses it
  (gfs2) and it compiles to 0 without `CONFIG_BUFFER_HEAD`. It is a
  transition mode, and a new filesystem requiring it would be arguing
  against the direction the kernel is moving.

- erofs looked like the precedent, but its aops are documented "for
  uncompressed (aligned) files"; decompression runs through a separate
  `z_erofs` path outside iomap. Following erofs would reproduce exactly
  the split beamfs already has.

## What makes it possible

`struct iomap_read_ops` (include/linux/iomap.h) is a public interface,
and `iomap_bio_read_ops` is one implementation of it, not the only one.
`read_folio_range` receives the iterator and the target folio, fills the
requested range however it likes, and guarantees
`iomap_finish_folio_read()` is called. `iomap_bio_read_folio_range_sync`
shows the synchronous shape is expected.

beamfs can therefore read the physical block, decode the sixteen RS
codewords, copy the usable slice into the folio, and finish the read --
inside iomap rather than beside it. `decode_block_into_buf` already
takes a destination buffer, so the correction logic is reused as-is.

The non-affine mapping is handled by mapping one block per iteration
(`length = 3824`), which iomap supports: it iterates over mappings.

## Why a branch, and why private

The rewrite touches the path that carries the entire value of the
project -- RS correction, DATA_CSUM, DATA_SELFID, fail-closed on
corrupt pointers. `devel` stays the reference while this is in flight.

Private remote only, no public push, until the migration is validated
end to end on the cluster.

## Noted for a later release

The kernel has no generic per-block transformation hook under iomap.
fscrypt decrypts outside the iomap path (`ext4/inode.c` calls
`fscrypt_decrypt_pagecache_blocks` directly), erofs decompresses in a
parallel path, and beamfs will implement `iomap_read_ops` by hand.
Three filesystems, three different answers to the same question.

`iomap_read_ops` is close to being that hook, and this migration is a
concrete use of it. Worth reporting as an observation in the RFC --
"here is a limit we ran into" -- without making it a precondition. If
it draws interest, proposing an extension afterwards rests on a
demonstrated case rather than on an argument.
