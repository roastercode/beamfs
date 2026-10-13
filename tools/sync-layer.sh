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
#
# The versions come from the sources: MODULE_VERSION in super.c,
# FSCK_BEAMFS_VERSION in fsck.beamfs.c and MKFS_BEAMFS_VERSION in
# mkfs.beamfs.c, whose recipe alone is renamed. When one has moved, the
# layer's
# source directory and recipe are renamed with git mv and every file of
# the layer that names the directory is rewritten: the module recipe,
# the mkfs recipe (it unpacks the module's directory), and the kernel
# bbappend that copies the module into fs/beamfs of the tree. Until
# this version the two numbers were written here by hand, the script
# refused to run until the directories had been renamed by hand, and
# the bbappend was forgotten: beamfs 0.1.22 built a kernel that could
# not parse.

set -e

B="${BEAMFS_REPO:-$HOME/git/beamfs}"
LAYER="${BEAMFS_LAYER:-$HOME/git/yocto-beamfs}"
R="recipes-kernel/beamfs"

[ -d "$B" ] || { echo "no repository at $B" >&2; exit 1; }
[ -d "$LAYER/$R/files" ] || { echo "no layer at $LAYER/$R/files" >&2; exit 1; }

MV="$(sed -n 's/^MODULE_VERSION("\([0-9.]*\)");.*/\1/p' "$B/super.c")"
FV="$(sed -n 's/^#define FSCK_BEAMFS_VERSION "\([0-9.]*\)".*/\1/p' "$B/tools/fsck.beamfs/fsck.beamfs.c")"
[ -n "$MV" ] || { echo "no MODULE_VERSION in $B/super.c" >&2; exit 1; }
[ -n "$FV" ] || { echo "no FSCK_BEAMFS_VERSION in $B/tools/fsck.beamfs/fsck.beamfs.c" >&2; exit 1; }
KV="$(sed -n 's/^#define MKFS_BEAMFS_VERSION "\([0-9.]*\)".*/\1/p' "$B/tools/mkfs.beamfs/mkfs.beamfs.c")"
[ -n "$KV" ] || { echo "no MKFS_BEAMFS_VERSION in $B/tools/mkfs.beamfs/mkfs.beamfs.c" >&2; exit 1; }

cd "$LAYER"

# rename <stem> <recipe> <old> <new>: the directory files/<stem>-<old>,
# the recipe <recipe>_<old>.bb, and every "<stem>-<old>" in the layer.
# The stem is matched after a non-letter so that "beamfs-" does not
# take "fsck-beamfs-" with it.
rename() {
	stem="$1" recipe="$2" old="$3" new="$4"
	[ "$old" = "$new" ] && return 0
	echo "$stem: $old -> $new"
	git mv "$R/files/$stem-$old" "$R/files/$stem-$new"
	[ -f "$R/${recipe}_$old.bb" ] && git mv "$R/${recipe}_$old.bb" "$R/${recipe}_$new.bb"
	o="$(printf '%s' "$old" | sed 's/\./\\./g')"
	git grep -l -- "$stem-$o" -- recipes-kernel recipes-core conf 2>/dev/null |
	while read -r f; do
		sed -i "s/\(^\|[^a-z-]\)$stem-$o\([^0-9]\|\$\)/\1$stem-$new\2/g" "$f"
		echo "  $f"
	done
	git add -A "$R" recipes-core conf 2>/dev/null || true
}

cur_m="$(printf '%s\n' "$R"/files/beamfs-[0-9]* | sed 's/.*\/beamfs-//' | sort -V | tail -1)"
cur_f="$(printf '%s\n' "$R"/files/fsck-beamfs-[0-9]* | sed 's/.*\/fsck-beamfs-//' | sort -V | tail -1)"
# A glob that matches nothing stays as written: what it gives is a
# directory only if it names one.
[ -d "$R/files/beamfs-$cur_m" ] || { echo "no $R/files/beamfs-<version> in the layer" >&2; exit 1; }
[ -d "$R/files/fsck-beamfs-$cur_f" ] || cur_f="$FV"

# fsck first: its name contains the module's stem.
rename fsck-beamfs fsck-beamfs "$cur_f" "$FV"
rename beamfs beamfs-module "$cur_m" "$MV"

# mkfs has a recipe and no directory of its own: it unpacks the
# module's. Its recipe follows MKFS_BEAMFS_VERSION, which until now
# was done by hand.
cur_k="$(printf '%s\n' "$R"/mkfs-beamfs_*.bb | sed 's/.*mkfs-beamfs_//; s/\.bb$//' | sort -V | tail -1)"
[ -f "$R/mkfs-beamfs_$cur_k.bb" ] || { echo "no $R/mkfs-beamfs_<version>.bb in the layer" >&2; exit 1; }
if [ "$cur_k" != "$KV" ]; then
	echo "mkfs-beamfs: $cur_k -> $KV"
	git mv "$R/mkfs-beamfs_$cur_k.bb" "$R/mkfs-beamfs_$KV.bb"
fi

L="$LAYER/$R/files/beamfs-$MV"
F="$LAYER/$R/files/fsck-beamfs-$FV"
mkdir -p "$F"

# The module's own sources, at the root of the repository.
rsync -ac --include="*.c" --include="*.h" --include="Kconfig" \
      --include="Makefile" --exclude="*" "$B/" "$L/"

# The formatter and the decoder it shares with the checker. They live
# under tools/ in the repository and flat in the layer, because the
# recipe unpacks one directory.
rsync -ac "$B/tools/mkfs.beamfs/mkfs.beamfs.c" "$L/"
# And its Makefile, under another name, since the module's has that one
# here: the recipe builds with it, so the formatter gets the warnings
# the repository gives it, and not a list the recipe keeps apart.
rsync -ac "$B/tools/mkfs.beamfs/Makefile" "$L/Makefile.mkfs"
rsync -ac "$B/tools/fsck.beamfs/rs_decode.c" \
         "$B/tools/fsck.beamfs/rs_decode.h" \
         "$B/tools/fsck.beamfs/rs_decode_internal.h" "$L/"

# The checker, whole.
#
# Only mkfs and the RS decoder were mirrored, so the layer kept a copy
# of fsck from whenever it was last edited by hand: 0.1.1 fixed a walk
# through uninitialised stack in the repository while the node went on
# running 0.1.0 and reporting leaks that were not there.
#
# Directories as well: --exclude="*" excluded tests/ too, and the
# layer's tests/, copied by hand once, went stale.
rsync -ac --include="*/" --include="*.c" --include="*.h" --include="Makefile" \
         --include="*.8" --include="COPYING" --exclude="*" --prune-empty-dirs \
         "$B/tools/fsck.beamfs/" "$F/"

# What still names another version is a reference this script does not
# know about; say so rather than let bitbake find it.
stale="$(git grep -n -- 'beamfs-0\.[0-9]*\.[0-9]*' -- recipes-kernel recipes-core conf 2>/dev/null |
	grep -v -- "beamfs-$MV\b" | grep -v -- "fsck-beamfs-$FV\b" | grep -v -- 'mkfs-beamfs' || true)"
[ -z "$stale" ] || { echo "still naming another version:" >&2; echo "$stale" >&2; exit 1; }

echo "layer mirrored from $B: beamfs $MV, fsck.beamfs $FV, mkfs.beamfs $KV"
