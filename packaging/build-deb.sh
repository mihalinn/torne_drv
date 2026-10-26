#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Debian packages:
#   torne-drv-dkms_<version>_all.deb    the kernel module (DKMS source package), udev rules, load at boot
#   torne-drv-pcsc_<version>_<arch>.deb the pcscd reader driver for the torne's built-in card slot
#
# Needs: dpkg-dev (dpkg-deb), gcc, libpcsclite-dev. Run from anywhere:  ./packaging/build-deb.sh
set -e
VERSION=0.1.0
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out="$top/dist"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"

# ---------------- torne-drv-dkms ----------------
pkg="$work/torne-drv-dkms"
install -d "$pkg/DEBIAN" "$pkg/usr/src/torne_drv-$VERSION" "$pkg/usr/lib/udev/rules.d" "$pkg/usr/lib/modules-load.d" \
	"$pkg/usr/share/doc/torne-drv-dkms"
for f in "$top"/driver/*; do
	install -m 644 "$f" "$pkg/usr/src/torne_drv-$VERSION/$(basename "$f")"
done
install -m 644 "$top/udev/99-torne.rules" "$pkg/usr/lib/udev/rules.d/99-torne.rules"
echo torne_drv >"$pkg/usr/lib/modules-load.d/torne_drv.conf"
chmod 644 "$pkg/usr/lib/modules-load.d/torne_drv.conf"
install -m 644 "$top/LICENSE" "$pkg/usr/share/doc/torne-drv-dkms/copyright"
install -m 644 "$top/NOTICE" "$pkg/usr/share/doc/torne-drv-dkms/NOTICE"

cat >"$pkg/DEBIAN/control" <<EOF
Package: torne-drv-dkms
Version: $VERSION
Section: kernel
Priority: optional
Architecture: all
Depends: dkms (>= 2.1.0.0), build-essential
Recommends: linux-headers-generic | linux-headers-amd64 | linux-headers, torne-drv-pcsc
Maintainer: torne_drv <noreply@users.noreply.github.com>
Homepage: https://github.com/mihalinn/torne_drv
Description: Linux driver for the Sony CECH-ZD1 J ("torne", PS3 terrestrial tuner), DKMS source
 Gives the torne a px4_drv-compatible character device (/dev/tornevideoN) and a card slot device (/dev/tornecardN).
 recisdb, recpt1, Mirakurun, EDCB and KonomiTV can use it like a PLEX tuner. ISDB-T (terrestrial) only.
 The module is rebuilt by DKMS whenever the kernel is updated.
EOF

cat >"$pkg/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
NAME=torne_drv
VERSION=$VERSION
case "\$1" in
configure)
	if dkms status -m \$NAME -v \$VERSION 2>/dev/null | grep -q .; then
		dkms remove -m \$NAME -v \$VERSION --all || true
	fi
	dkms add -m \$NAME -v \$VERSION
	if ! dkms build -m \$NAME -v \$VERSION; then
		echo "torne-drv-dkms: the build failed. See /var/lib/dkms/\$NAME/\$VERSION/build/make.log" >&2
		exit 1
	fi
	dkms install -m \$NAME -v \$VERSION --force
	udevadm control --reload 2>/dev/null || true
	udevadm trigger --subsystem-match=usb --attr-match=idVendor=054c --attr-match=idProduct=04b2 2>/dev/null || true
	modprobe torne_drv 2>/dev/null || true
	;;
esac
exit 0
EOF

cat >"$pkg/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
case "\$1" in
remove|upgrade|deconfigure)
	rmmod torne_drv 2>/dev/null || true
	dkms remove -m torne_drv -v $VERSION --all 2>/dev/null || true
	;;
esac
exit 0
EOF
chmod 755 "$pkg/DEBIAN/postinst" "$pkg/DEBIAN/prerm"
dpkg-deb --root-owner-group --build "$pkg" "$out/torne-drv-dkms_${VERSION}_all.deb" >/dev/null
echo "built $out/torne-drv-dkms_${VERSION}_all.deb"

# ---------------- torne-drv-pcsc ----------------
arch=$(dpkg --print-architecture)
pcsc="$work/torne-drv-pcsc"
bundle="$pcsc/usr/lib/pcsc/drivers/ifd-torne.bundle"
install -d "$pcsc/DEBIAN" "$bundle/Contents/Linux" "$pcsc/usr/share/doc/torne-drv-pcsc"
make -s -C "$top/ifd" clean
make -s -C "$top/ifd"
install -m 755 "$top/ifd/libifd_torne.so" "$bundle/Contents/Linux/libifd_torne.so"
install -m 644 "$top/ifd/Info.plist" "$bundle/Contents/Info.plist"
install -m 644 "$top/LICENSE" "$pcsc/usr/share/doc/torne-drv-pcsc/copyright"
install -m 644 "$top/NOTICE" "$pcsc/usr/share/doc/torne-drv-pcsc/NOTICE"
make -s -C "$top/ifd" clean
cat >"$pcsc/DEBIAN/control" <<EOF
Package: torne-drv-pcsc
Version: $VERSION
Section: libs
Priority: optional
Architecture: $arch
Depends: pcscd, libc6
Maintainer: torne_drv <noreply@users.noreply.github.com>
Homepage: https://github.com/mihalinn/torne_drv
Description: pcscd reader driver for the Sony CECH-ZD1 J's built-in card slot
 Shows the B-CAS card slot of the torne to pcscd as a normal smart card reader (needs the torne-drv-dkms module).
EOF
cat >"$pcsc/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
case "$1" in
configure)
	systemctl restart pcscd.socket pcscd.service 2>/dev/null || true
	;;
esac
exit 0
EOF
chmod 755 "$pcsc/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$pcsc" "$out/torne-drv-pcsc_${VERSION}_${arch}.deb" >/dev/null
echo "built $out/torne-drv-pcsc_${VERSION}_${arch}.deb"
