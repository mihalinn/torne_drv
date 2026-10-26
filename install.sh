#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Install torne_drv (kernel module through DKMS) and the pcscd reader driver for the card slot.
#   sudo ./install.sh            install
#   sudo ./install.sh uninstall  remove
set -e
VERSION=0.1.0
SRC=/usr/src/torne_drv-$VERSION
here=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" != 0 ]; then
	echo "run as root (sudo $0)" >&2
	exit 1
fi

pcsc_dir() {
	pkg-config --variable=usbdropdir libpcsclite 2>/dev/null || echo /usr/lib/pcsc/drivers
}

if [ "$1" = "uninstall" ]; then
	rmmod torne_drv 2>/dev/null || true
	dkms remove torne_drv/$VERSION --all 2>/dev/null || true
	rm -rf "$SRC"
	rm -f /etc/udev/rules.d/99-torne.rules /etc/modules-load.d/torne_drv.conf
	rm -rf "$(pcsc_dir)/ifd-torne.bundle"
	udevadm control --reload 2>/dev/null || true
	echo "removed. restart pcscd."
	exit 0
fi

command -v dkms >/dev/null || { echo "dkms is required: apt install dkms linux-headers-\$(uname -r)" >&2; exit 1; }

# 1. kernel module (DKMS)
rm -rf "$SRC"
install -d "$SRC"
for f in "$here"/driver/*; do
	install -m 644 "$f" "$SRC/$(basename "$f")"
done
dkms remove torne_drv/$VERSION --all 2>/dev/null || true
dkms add -m torne_drv -v $VERSION
dkms build -m torne_drv -v $VERSION
dkms install -m torne_drv -v $VERSION

# 2. udev rules and load at boot
install -m 644 "$here/udev/99-torne.rules" /etc/udev/rules.d/99-torne.rules
echo torne_drv >/etc/modules-load.d/torne_drv.conf
udevadm control --reload

# 3. pcscd reader driver for the card slot (build it first with `make`)
if [ -f "$here/ifd/libifd_torne.so" ]; then
	bundle="$(pcsc_dir)/ifd-torne.bundle"
	install -d "$bundle/Contents/Linux"
	install -m 644 "$here/ifd/Info.plist" "$bundle/Contents/Info.plist"
	install -m 755 "$here/ifd/libifd_torne.so" "$bundle/Contents/Linux/libifd_torne.so"
	echo "installed the pcscd driver into $bundle (restart pcscd: systemctl restart pcscd.socket pcscd)"
else
	echo "ifd/libifd_torne.so not found: the card slot driver was not installed (run 'make' first)"
fi

modprobe torne_drv
echo "done. check /dev/tornevideo0 and /dev/tornecard0"
