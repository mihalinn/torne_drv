# torne_drv

PS3用の地デジチューナー **Sony CECH-ZD1 J(torne)** を、[px4_drv](https://github.com/nns779/px4_drv) と同じ形で使えるようにする Linux ドライバーです。

- 映像: `/dev/tornevideoN`。`/dev/px4video*` と同じ ioctl・`read()` なので、`recisdb`、`recpt1`、Mirakurun、EDCB、KonomiTV から PLEX のチューナーと同じように使えます。
- B-CAS カード: torne 本体のカードスロットを、pcscd から普通のリーダーとして使えます(`/dev/tornecardN` と pcscd 用ドライバー)。カードは torne に差したままにできます。
- 地デジ(ISDB-T)のみ。1台につきチューナー1基。**torne 専用**で、ほかの機器には対応しません。

> **ベータ版です。** 確認は、torne 1台、Ubuntu 22.04(カーネル 5.15)、x86-64 のみです。ほかのカーネル、2台以上、長時間の運用は未確認です。

## 元にしたもの

torne の富士通 MB86H47A という USB ブリッジの動かし方は、[coax](https://github.com/Till0196/coax)(Till0196 さん)が解析したものです。このドライバーは、coax のブリッジと DiB8096 の処理を Go から C に移して、カーネルモジュールにし、px4_drv の文字デバイス層を載せたものです。詳しくは [NOTICE](NOTICE) を見てください。どちらのフォークでもありません。

## インストール

```sh
sudo apt install dkms linux-headers-$(uname -r) build-essential libpcsclite-dev pcscd
make                  # pcscd 用ドライバー(ifd/)と試験ツール(tools/)をビルド
sudo ./install.sh     # DKMS のカーネルモジュール、udev ルール、pcscd ドライバーを入れる
```

`/dev/tornevideo0` と `/dev/tornecard0` ができます。取り外しは `sudo ./install.sh uninstall` です。

## 使い方

```sh
recisdb tune --device /dev/tornevideo0 --channel T33 -t 60 out.ts            # pcscd で復号
recisdb tune --device /dev/tornevideo0 --channel T33 --no-decode -t 60 out.ts
```

Mirakurun の `tuners.yml` は、PLEX のチューナーと同じ書き方です(地デジのみ)。

```yaml
- name: 'Sony torne'
  types:
    - GR
  command: recisdb tune --device /dev/tornevideo0 --channel <channel><satellite>-
  isDisabled: false
```

## 制限

- **チューナーは1基で、地デジのみ。** 視聴、録画、番組表の取得は同時にできません。
- **電源を入れた直後の状態でしか起動できません。** ほかのソフトや OS が使った後は、USB を抜き差ししてください。
- **選局に 2〜3 秒かかります。**
- **複数台は未確認です。** `tornevideo0` `tornevideo1` の番号は、差した位置ではなく認識された順です。複数台で使うときは、recisdb の `--card` で、tuner とカードの組み合わせを指定してください。
- **USB を抜き差ししても自動で復帰します。** ただし、止まった視聴や録画は、自動では再開しません。

## ライセンス

GPL-2.0-only。[LICENSE](LICENSE) と [NOTICE](NOTICE) を見てください。
