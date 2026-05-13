# Upstream kernel patch report tracking

This directory tracks patches sent upstream to kernel.org as a result
of issues discovered during beamfs development.

## v1 - lib/reed_solomon: document rs_control concurrency contract

- **Sent**: 2026-05-14 00:17 CEST
- **From**: Aurelien DESBRIERES <aurelien@hackers.camp>
- **To**: Thomas Gleixner <tglx@kernel.org>
- **Cc**: linux-kernel@vger.kernel.org
- **Cover-letter Message-ID**: `<cover.1778710307.git.aurelien@hackers.camp>`
- **Patch 1/1 Message-ID**: `<10d4000e212db297ee7b7658bb7125cc8ca56997.1778710307.git.aurelien@hackers.camp>`
- **lore.kernel.org thread**: https://lore.kernel.org/lkml/cover.1778710307.git.aurelien@hackers.camp/
- **Base**: linux master at e1914add2799 ("Merge tag 'for-linus' of git://git.kernel.org/pub/scm/virt/kvm/kvm")
- **Local branch**: ~/git/linux/rslib-doc-thread-safety @ 10d4000e212d
- **Status**: SENT, awaiting maintainer response

### Scope

Documentation-only patch. Extends the kdoc of `struct rs_control` in
`include/linux/rslib.h` with an explicit "Locking and concurrency"
paragraph stating that a single rs_control instance is not safe to
share across concurrent encode_rs*() / decode_rs*() calls, and that
callers needing concurrent codec use must allocate one rs_control
per concurrent caller.

No code change, no ABI change.

### Origin

Discovered while building beamfs: an out-of-tree filesystem that
decodes RS(255,239) shortened subblocks per 4 KiB disk block on read.
A single shared rs_control (allocated once at module_init via
init_rs(8, 0x187, 0, 1, 16)) raced under parallel read load: 8
parallel sha256sum invocations on a read-only mount produced 150-240
wrong hashes per run and ~160 RS uncorrectable entries per batch.

Root cause: `lib/reed_solomon/decode_rs.c` keeps its scratch buffers
(lambda, syn, b, t, omega, root, reg, loc) inside
`rs_control->buffers[]`. Two concurrent callers on the same rs_control
race on those buffers. Behaviour latent since 2002 (Phil Karn KA9Q
implementation adapted to kernel by Thomas Gleixner). No existing
in-tree user appears to decode concurrently against a shared
rs_control.

The beamfs-side fix is per-CPU rs_control allocation (alloc_percpu +
for_each_possible_cpu init_rs, with get_cpu_ptr / put_cpu_ptr around
encode_rs8 / decode_rs8). See beamfs commit d64c727 ("fs/beamfs:
per-CPU rs_control to avoid upstream lib/reed_solomon race") and the
corresponding yocto-beamfs lockstep commit b978889.

### Follow-up

If Thomas accepts the doc patch and is open to it, two possible
follow-up patches were mentioned in the cover-letter but not bundled:

1. Move scratch arrays out of struct rs_control to caller stack or
   per-CPU helper inside the library (so a shared rs_control becomes
   safe). Touches the hot path of decode_rs.c and encode_rs.c.

2. Add an `rs_control_get_for_cpu()` helper that wraps alloc_percpu +
   for_each_possible_cpu init_rs / get_cpu_ptr. Additive, narrow.

Decision deferred until Thomas responds.
