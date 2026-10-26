# torne_drv

[日本語](README.md)

A Linux driver for the **Sony CECH-ZD1 J** ("torne", the PS3 terrestrial tuner) that looks like [px4_drv](https://github.com/nns779/px4_drv) from the outside.

- Video: `/dev/tornevideoN`, with the same ioctl and `read()` interface as `/dev/px4video*`. `recisdb`, `recpt1`, Mirakurun, EDCB and KonomiTV use it like a PLEX tuner.
- B-CAS card: the torne's built-in card slot is shown to `pcscd` as a normal reader (`/dev/tornecardN` plus a pcscd driver), so the card can be left in the torne.
- ISDB-T (terrestrial) only. One tuner per device. **Torne only**: nothing else is supported.

> **Status: beta.** Tested with one torne, Ubuntu 22.04 (kernel 5.15), x86-64. Not tested: other kernels, two or more devices, long runs beyond some hours. See [Limitations](#limitations).

## What it is built from

The hard part, working out how the torne's Fujitsu MB86H47A USB bridge is driven, was done by [coax](https://github.com/Till0196/coax) (Till0196). This driver is a port of coax's bridge and DiB8096 code from Go to C, put into a kernel module, with the character device layer of px4_drv (the original by [nns779](https://github.com/nns779/px4_drv) and the maintained [tsukumijima fork](https://github.com/tsukumijima/px4_drv), version 0.6.1) on top. See [NOTICE](NOTICE). It is **not** a fork of either project.

## Install

```sh
sudo apt install dkms linux-headers-$(uname -r) build-essential libpcsclite-dev pcscd
make                  # builds the pcscd driver (ifd/) and the user-space test tool (tools/)
sudo ./install.sh     # DKMS kernel module, udev rules, pcscd driver
```

You get `/dev/tornevideo0` and `/dev/tornecard0`. Remove with `sudo ./install.sh uninstall`.

## Use

```sh
recisdb tune --device /dev/tornevideo0 --channel T33 -t 60 out.ts            # decoded through pcscd
recisdb tune --device /dev/tornevideo0 --channel T33 --no-decode -t 60 out.ts
```

Mirakurun `tuners.yml`, in the same form as for PLEX tuners:

```yaml
- name: 'Sony torne'
  types:
    - GR
  command: recisdb tune --device /dev/tornevideo0 --channel <channel><satellite>-
  isDisabled: false
```

Channels are selected with px4_drv's ioctl (`PTX_SET_CHANNEL`; UHF 13-62ch is `freq_no = ch + 50`).

## Limitations

- **One tuner, terrestrial only.** Watching, recording and EPG gathering share it.
- **The bridge starts only from its power-up state.** After another driver or OS has used the torne, replug it.
- **Tuning takes 2-3 s.** The register writes go over USB one by one (coax's ring and command channel speed-ups are not ported).
- **Several devices are untested.** Device numbers (`tornevideo0`, `tornevideo1`) follow the probe order, not the port. If you use more than one, bind each card to its tuner yourself (recisdb `--card`).
- **Hot unplug works** (the device comes back when replugged), but a stream that was running does not resume by itself; start it again.
- Only tested on Ubuntu 22.04 with kernel 5.15.

## Layout

| Path | |
| --- | --- |
| `driver/` | the kernel module (`torne_kern.c`), the OS-independent core (`torne_bridge.c`, `torne_dib.c`, `torne_card.c`), px4_drv's character device layer |
| `ifd/` | the pcscd reader driver for the card slot |
| `tools/` | `torne_user` (the same core run from user space with libusb), test scripts, and the generators for the tables and machine code (`tools/gen`) |
| `udev/` | udev rules |

The `driver/torne_fw_gen.h` and `driver/torne_dib_tables_gen.h` files are generated from coax with the programs in `tools/gen` (see the comments in them).

## Testing without the kernel module

```sh
sudo rmmod torne_drv
sudo tools/torne_user probe             # bridge start, DiB8000 ID
sudo tools/torne_user tune 33 10 > t.ts # tune UHF 33ch for 10 s
sudo tools/torne_user card              # the card slot
tools/torne_user emu-trace              # run against a fake memory device and print the USB requests (identical to coax's, apart from its bulk command channel setup, which is not ported)
```

## License

GPL-2.0-only. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
