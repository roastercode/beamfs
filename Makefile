# SPDX-License-Identifier: GPL-2.0-only
#
# BEAMFS — Fault-Tolerant Radiation-Robust Filesystem
#

obj-$(CONFIG_BEAMFS_FS) += beamfs.o

beamfs-y := super.o      \
            inode.o      \
            dir.o        \
            file.o       \
            file_inline.o \
            edac.o       \
            alloc.o      \
            namei.o

# Strip absolute build paths from __FILE__ macros so the resulting .ko
# binary does not embed TMPDIR (Yocto buildpaths QA fix).
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
