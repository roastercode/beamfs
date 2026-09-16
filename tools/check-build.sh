#!/bin/sh
# Compile beamfs against the kernel it targets, in seconds.
#
# `make` in this directory builds against the workstation's kernel, and
# beamfs targets another one: super.c fails on sync_inode_metadata --
# whose signature changed in 7.3 -- and the build stops there, two files
# into sixteen. The fourteen it never reaches are not checked at all.
#
# Twice in one morning that silence read as a pass. A %lu given an
# unsigned long long in file_inline.c and a function without a prototype
# in indparity.c both went to a commit, and bitbake found them twenty
# minutes later.
#
# kbuild is asked to build the one directory, in the tree Yocto already
# unpacked, with the config it already generated. Same compiler, same
# flags, same headers -- and about ten seconds against bitbake's twenty
# minutes.
#
# Not a replacement for bitbake: what happens at link time, and whether
# the recipe's Kconfig lines take, only show up there. This is what runs
# before proposing a commit.
set -u

K="${BEAMFS_KSRC:-$HOME/yocto/poky/build-qemux86/tmp/work-shared/qemux86-64/kernel-source}"
B="${BEAMFS_KBUILD:-$HOME/yocto/poky/build-qemux86/tmp/work/qemux86_64-poky-linux/linux-mainline/7.3-rc2/build}"
NATIVE="${BEAMFS_NATIVE:-$HOME/yocto/poky/build-qemux86/tmp/work/qemux86_64-poky-linux/linux-mainline/7.3-rc2/recipe-sysroot-native}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

[ -f "$B/.config" ] || {
	echo "no configured kernel build at $B"
	echo "run bitbake linux-mainline once, or set BEAMFS_KBUILD"
	exit 2
}
[ -d "$K/fs/beamfs" ] || {
	echo "no fs/beamfs in $K -- has the recipe copied the sources yet?"
	exit 2
}

# The sources under test, not whatever bitbake left there last time.
for f in "$SRC"/*.c "$SRC"/*.h "$SRC"/Makefile "$SRC"/Kconfig; do
	[ -e "$f" ] || continue
	case "$(basename "$f")" in *.mod.c) continue ;; esac
	cp "$f" "$K/fs/beamfs/" || exit 2
done

PATH="$NATIVE/usr/bin/x86_64-poky-linux:$PATH"
export PATH

OUT=$(make -C "$K" O="$B" \
     ARCH=x86_64 CROSS_COMPILE=x86_64-poky-linux- \
     fs/beamfs/ 2>&1)
echo "$OUT" |
	grep -E "(error|warning):|CC \[M\]|CC " |
	sed "s|$K/||"

# How many files it actually compiled.
#
# A make that compiles nothing returns zero, and "clean" then means
# "nothing was checked". The sources are copied in above so kbuild
# always has something newer to build, but saying the number out loud
# is what makes that visible rather than assumed.
N=$(echo "$OUT" | grep -cE "^  CC")

# The pipeline's status is grep's, so ask make again -- it is a no-op
# the second time and answers in a fraction of a second.
if make -C "$K" O="$B" ARCH=x86_64 CROSS_COMPILE=x86_64-poky-linux- \
	fs/beamfs/ >/dev/null 2>&1; then
	echo
	echo "  clean against $(basename "$(dirname "$B")") -- $N file(s) compiled"
	[ "$N" -eq 0 ] && echo "  nothing was compiled: this says nothing about the code"
	echo
	echo "  and this is not bitbake: the kernel the node boots is only"
	echo "  rebuilt by 'bitbake linux-mainline', and only deployed by"
	echo "  'beamfs-xfstests deploy'." 
else
	echo
	echo "  it does not build"
	exit 1
fi
