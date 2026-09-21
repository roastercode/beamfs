#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Mirror the repository into the Yocto layer.
#
# The kernel compiles the layer's copy, not this repository. A fix
# committed here and not copied there is not in the build, and the
# campaign measures the code the fix was meant to replace while every
# commit, diff and addr2line says otherwise. That happened.
#
# The layer is a mirror from here on: edit the repository, run this.

set -e

B="${BEAMFS_REPO:-$HOME/git/beamfs}"
L="${BEAMFS_LAYER_SRC:-$HOME/git/yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.5}"

[ -d "$B" ] || { echo "no repository at $B" >&2; exit 1; }
[ -d "$L" ] || { echo "no layer at $L" >&2; exit 1; }

# The module's own sources, at the root of the repository.
rsync -a --include="*.c" --include="*.h" --include="Kconfig" \
      --include="Makefile" --exclude="*" "$B/" "$L/"

# The formatter and the decoder it shares with the checker. They live
# under tools/ in the repository and flat in the layer, because the
# recipe unpacks one directory.
rsync -a "$B/tools/mkfs.beamfs/mkfs.beamfs.c" "$L/"
rsync -a "$B/tools/fsck.beamfs/rs_decode.c" \
         "$B/tools/fsck.beamfs/rs_decode.h" \
         "$B/tools/fsck.beamfs/rs_decode_internal.h" "$L/"

echo "layer mirrored from $B"
