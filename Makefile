# SPDX-License-Identifier: GPL-2.0-only
#
# beamfs - resilient filesystem
#

obj-$(CONFIG_BEAMFS_FS) += beamfs.o

beamfs-y := treecheck.o super.o      \
            inode.o      \
            dir.o        \
            dirent.o     \
            file.o       \
            file_inline.o \
            scrub.o      \
            indparity.o  \
            rsbench.o    \
            budget.o     \
            clock.o      \
            alert.o      \
            edac.o       \
            alloc.o      \
            namei.o

# Strip absolute build paths from __FILE__ macros so the resulting .ko
# binary does not embed TMPDIR (Yocto buildpaths QA fix).
# Tracepoint definitions live in beamfs_trace.h next to the sources;
# define_trace.h re-includes it by name, so the directory has to be on
# the include path.
# The tree checker is a build-time choice like the others: it records
# every live indirect pointer and reports a slot that loses one at the
# instant it happens. Off unless asked for.
ifeq ($(BEAMFS_DEBUG_TREE),1)
ccflags-y += -DCONFIG_BEAMFS_DEBUG_TREE
endif

ccflags-y += -I$(src)

ccflags-y += -fmacro-prefix-map=$(src)/=
ccflags-y += -fmacro-prefix-map=$(srctree)/=
ccflags-y += -ffile-prefix-map=$(src)/=
ccflags-y += -ffile-prefix-map=$(srctree)/=


ifneq ($(KERNELRELEASE),)
else

ifneq ($(KERNEL_SRC),)
  KERNELDIR := $(KERNEL_SRC)
else
  KERNELDIR ?= /lib/modules/$(shell uname -r)/build
endif

ifneq ($(O),)
  KBUILD_OUTPUT := O=$(O)
else
  KBUILD_OUTPUT :=
endif

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) \
		CONFIG_BEAMFS_FS=m \
		modules

clean:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KERNELDIR) $(KBUILD_OUTPUT) M=$(PWD) modules_install

endif
