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
#   snd-usb-babyface-pro-objs := main.o protocol.o pcm.o mixer.o state.o panel.o
#   obj-$(CONFIG_SND_USB_BABYFACE_PRO) += snd-usb-babyface-pro.o
#
# and Kconfig gains a `config SND_USB_BABYFACE_PRO` entry (see Kconfig).

obj-m := snd-usb-babyface-pro.o
snd-usb-babyface-pro-objs := main.o protocol.o pcm.o mixer.o state.o panel.o
