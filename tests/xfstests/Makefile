# SPDX-License-Identifier: GPL-2.0
#
# The beamfs tests of xfstests, installed as tests/beamfs: a copy of
# tests/ext4/Makefile with the directory renamed.
#

TOPDIR = ../..
include $(TOPDIR)/include/builddefs
include $(TOPDIR)/include/buildgrouplist

BEAMFS_DIR = beamfs
TARGET_DIR = $(PKG_LIB_DIR)/$(TESTS_DIR)/$(BEAMFS_DIR)
DIRT = group.list

default: $(DIRT)

include $(BUILDRULES)

install: default
	$(INSTALL) -m 755 -d $(TARGET_DIR)
	$(INSTALL) -m 755 $(TESTS) $(TARGET_DIR)
	$(INSTALL) -m 644 group.list $(TARGET_DIR)
	$(INSTALL) -m 644 $(OUTFILES) $(TARGET_DIR)

# Nothing.
install-dev install-lib:
