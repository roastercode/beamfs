/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fsck.beamfs -- on-disk layout
 *
 * This file used to carry its own copy of the format: the structures,
 * the RS geometry, the offsets. Keeping a second copy in step with the
 * kernel's turned out to be something nobody does reliably, and on
 * 2026-08-30 three defects landed in one day because of it -- fsck
 * rejecting the reserved inodes' pointers, its superblock struct
 * lagging the kernel's by sixteen bytes, and mkfs computing the canary
 * bit differently.
 *
 * It now includes the format header the kernel uses. There is one
 * description of the format, and everything reads it.
 */

#ifndef FSCK_BEAMFS_SB_LAYOUT_H
#define FSCK_BEAMFS_SB_LAYOUT_H

#include "beamfs_format.h"

#endif /* FSCK_BEAMFS_SB_LAYOUT_H */
