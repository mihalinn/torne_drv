# torne_drv - Linux driver for the Sony CECH-ZD1 J (PS3 "torne")

An unofficial Linux driver for the PS3 terrestrial tuner **Sony CECH-ZD1 J ("torne")** that looks like [px4_drv](https://github.com/tsukumijima/px4_drv) from the outside.

[日本語](README.md)

- **Video:** `/dev/tornevideoN`, with the same ioctl and `read()` interface as `/dev/px4video*`. `recisdb`, `recpt1`, Mirakurun, EDCB and KonomiTV use it like a PLEX tuner.
- **B-CAS card:** the torne's built-in card slot is shown to `pcscd` as a normal reader (`/dev/tornecardN` plus a pcscd driver), so the card can stay in the torne.
- **ISDB-T (terrestrial) only.** One tuner per device. **torne only**: nothing else is supported.
- **DKMS + Debian packages.** Install the `.deb` and the module is rebuilt automatically when the kernel is updated.

> **Beta.** Verified on real hardware only with Ubuntu 22.04 (kernel 5.15), x86-64. See [Verified kernels](#verified-kernels).

## What it is built from

How the torne's Fujitsu MB86H47A USB bridge is driven was worked out by [coax](https://github.com/Till0196/coax) (Till0196). This driver is a port of coax's bridge and DiB8096 code from Go to C, turned into a kernel module, with the character device layer of px4_drv (the original by [nns779](https://github.com/nns779/px4_drv) and the maintained [tsukumijima fork](https://github.com/tsukumijima/px4_drv), version 0.6.1) on top. See [NOTICE](NOTICE). It is **not** a fork of either project.

## Install (Debian / Ubuntu)

The Debian packages are on the [releases page](https://github.com/mihalinn/torne_drv/releases): the driver (a DKMS source package, the same for every CPU) and the pcscd driver for the card slot (amd64).

```bash
# download and install the Debian packages
wget https://github.com/mihalinn/torne_drv/releases/download/v0.1.0/torne-drv-dkms_0.1.0_all.deb
wget https://github.com/mihalinn/torne_drv/releases/download/v0.1.0/torne-drv-pcsc_0.1.0_amd64.deb
sudo apt install -y ./torne-drv-dkms_0.1.0_all.deb ./torne-drv-pcsc_0.1.0_amd64.deb
rm ./torne-drv-dkms_0.1.0_all.deb ./torne-drv-pcsc_0.1.0_amd64.deb

# load the driver (the install does it too; from the next boot it is loaded automatically)
sudo modprobe torne_drv

# check that it is loaded (log lines should appear)
lsmod | grep -e ^torne_drv
dmesg | grep torne
ls -la /dev | grep -e torne
```

You should see `/dev/tornevideo0` and `/dev/tornecard0`. If not, check the USB connection or replug the torne.

With `torne-drv-pcsc` installed, pcscd shows the card slot as "Sony CECH-ZD1 J Smart Card Reader" (keep the B-CAS card in the torne's slot):

```bash
pcsc_scan
```

Without `torne-drv-pcsc` you can keep using an external card reader (SCR3310 and the like) as before.

Uninstall with `sudo apt remove torne-drv-dkms torne-drv-pcsc`.

### Without the Debian packages

From source (other distributions, for example):

```bash
sudo apt install dkms linux-headers-$(uname -r) build-essential libpcsclite-dev pcscd   # Debian / Ubuntu
git clone https://github.com/mihalinn/torne_drv.git && cd torne_drv
make                  # the pcscd driver (ifd/) and the user-space test tool (tools/)
sudo ./install.sh     # DKMS kernel module, udev rules, pcscd driver
```

Remove with `sudo ./install.sh uninstall`. To build the Debian packages yourself: `./packaging/build-deb.sh` (output in `dist/`).

## Use

```bash
# record, decoded with the card through pcscd
recisdb tune --device /dev/tornevideo0 --channel T33 -t 60 out.ts
# without decoding
recisdb tune --device /dev/tornevideo0 --channel T33 --no-decode -t 60 out.ts
```

Mirakurun `tuners.yml`, in the same form as for PLEX tuners (terrestrial only):

```yaml
- name: 'Sony torne'
  types:
    - GR
  command: recisdb tune --device /dev/tornevideo0 --channel <channel><satellite>-
  isDisabled: false
```

Channels are selected with px4_drv's ioctl (`PTX_SET_CHANNEL`; UHF 13-62ch is `freq_no = ch + 50`).

If pcscd has other readers besides the torne's slot (an external reader, for example), recisdb takes the first one. Use `--card` to choose.

## Verified kernels

| Distribution | Kernel | What was checked |
| --- | --- | --- |
| Ubuntu 22.04 | 5.15 | **Real torne**: watching, recording, EPG, B-CAS decoding, USB unplug and replug, install from the `.deb` files |
| Ubuntu 24.04 | 6.8 | Build, DKMS install and removal, module load (**no torne attached**) |
| Ubuntu 24.04 (HWE) | 7.0 | Build and DKMS install only (**no torne attached**) |

Only x86-64 was checked; ARM was not. Please report problems with other kernels as issues.

## Limitations

- **One tuner, terrestrial only.** Watching, recording and EPG gathering share it.
- **The bridge starts only from its power-up state.** After another driver or OS has used the torne, replug it.
- **Tuning takes 2-3 s.** The register writes go over USB one by one (coax's ring and command channel speed-ups are not ported).
- **Several devices are untested.** Device numbers (`tornevideo0`, `tornevideo1`) follow the probe order, not the port. If you use more than one, bind each card to its tuner yourself (recisdb `--card`).
- **Hot unplug works** (the device comes back when replugged), but a stream that was running does not resume by itself; start it again.

## Layout

| Path | |
| --- | --- |
| `driver/` | the kernel module (`torne_kern.c`), the OS-independent core (`torne_bridge.c`, `torne_dib.c`, `torne_card.c`), px4_drv's character device layer |
| `ifd/` | the pcscd reader driver for the card slot |
| `packaging/` | the script that builds the Debian packages |
| `tools/` | `torne_user` (the same core run from user space with libusb), test scripts, and the generators for the tables and machine code (`tools/gen`) |
| `udev/` | udev rules |

`driver/torne_fw_gen.h` and `driver/torne_dib_tables_gen.h` are generated from coax; see [tools/gen/README.md](tools/gen/README.md).

## Testing without the kernel module

Unload the module first (both cannot own the device at once).

```bash
sudo rmmod torne_drv
sudo tools/torne_user probe             # bridge start, DiB8000 ID
sudo tools/torne_user tune 33 10 > t.ts # tune UHF 33ch for 10 s
sudo tools/torne_user card              # the card slot
tools/torne_user emu-trace              # run against a fake memory device and print the USB requests
                                        # (identical to coax's, apart from its bulk command channel setup, which is not ported)
```

## License

**GPL-2.0-only.** See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Sony, PlayStation and torne are trademarks of their owners. This project is not affiliated with or endorsed by Sony, DiBcom, Fujitsu, PLEX or any of the projects listed in [NOTICE](NOTICE).
