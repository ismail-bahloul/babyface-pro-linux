# SPDX-License-Identifier: GPL-2.0-only
#
# Out-of-tree build of the RME Babyface Pro FS proprietary-mode driver
# against the running kernel:
#
#   make -C /lib/modules/$(uname -r)/build M=$(PWD) modules
#
# CachyOS ships a clang-built kernel: pass LLVM=1 (and CC=clang) or the
# build fails on the LLVM-only flags:
#
#   make LLVM=1 -C /lib/modules/$(uname -r)/build M=$(PWD) modules
#
# To install (as root):
#
#   make LLVM=1 -C /lib/modules/$(uname -r)/build M=$(PWD) modules_install
#
# For the upstream integration, this Makefile is merged into
# sound/usb/Makefile as:
#
#   snd-usb-babyface-pro-objs := babyfacepro.o babyfacepro-ctl.o
#   obj-$(CONFIG_SND_USB_BABYFACE_PRO) += snd-usb-babyface-pro.o
#
# and Kconfig gains a `config SND_USB_BABYFACE_PRO` entry (see Kconfig).
# The MIDI ports use snd-usbmidi-lib, which sound/usb/Makefile builds for
# the drivers that need it; add this driver to that list:
#
#   obj-$(CONFIG_SND_USB_BABYFACE_PRO) += snd-usbmidi-lib.o
#
# and replace the prototypes in babyfacepro.h with #include "midi.h".

obj-m := snd-usb-babyface-pro.o
snd-usb-babyface-pro-objs := babyfacepro.o babyfacepro-ctl.o
