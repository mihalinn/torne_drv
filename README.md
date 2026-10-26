# torne_drv

PS3用の地デジチューナー **Sony CECH-ZD1 J(torne)** を、[px4_drv](https://github.com/nns779/px4_drv) と同じ形で使えるようにする Linux ドライバーです。

[English](README.en.md)

- **映像:** `/dev/tornevideoN` ができます。`/dev/px4video*` と同じ ioctl・`read()` なので、`recisdb`、`recpt1`、Mirakurun、EDCB、KonomiTV から、PLEX のチューナーと同じように使えます。
- **B-CAS カード:** torne 本体のカードスロットを、pcscd から普通のリーダーとして使えます(`/dev/tornecardN` と pcscd 用ドライバー)。カードは torne に差したままにできます。
- **地デジ(ISDB-T)のみ。** 1台につきチューナー1基です。**torne 専用**で、ほかの機器には対応しません。

> **ベータ版です。** 確認したのは、torne 1台、Ubuntu 22.04(カーネル 5.15)、x86-64 だけです。ほかのカーネル、2台以上、長時間の運用は未確認です。[制限](#制限)も読んでください。

## 元にしたもの

torne の富士通 MB86H47A という USB ブリッジの動かし方は、[coax](https://github.com/Till0196/coax)(Till0196 さん)が解析したものです。このドライバーは、coax のブリッジと DiB8096 の処理を Go から C に移して、カーネルモジュールにし、px4_drv(本家 [nns779](https://github.com/nns779/px4_drv) と、メンテナンスを引き継いだ [tsukumijima 版](https://github.com/tsukumijima/px4_drv) 0.6.1)の文字デバイス層を載せたものです。詳しくは [NOTICE](NOTICE) を見てください。

## 必要なもの

- Linux(x86-64)。Ubuntu 22.04 / カーネル 5.15 で確認しています。
- torne(CECH-ZD1 J)、アンテナ、B-CAS カード(torne のスロットに差す)。
- 録画ソフト側: `recisdb` や Mirakurun など、px4_drv のチューナーを使えるもの。

## インストール

```sh
sudo apt install dkms linux-headers-$(uname -r) build-essential libpcsclite-dev pcscd
make                  # pcscd 用ドライバー(ifd/)と試験ツール(tools/)をビルド
sudo ./install.sh     # DKMS のカーネルモジュール、udev ルール、pcscd ドライバーを入れる
```

`/dev/tornevideo0` と `/dev/tornecard0` ができます。取り外しは `sudo ./install.sh uninstall` です。

pcscd のドライバーを入れたあとは、`sudo systemctl restart pcscd.socket pcscd` で、リーダーが見えるようになります(`pcsc_scan` で「Sony CECH-ZD1 J Smart Card Reader」が出れば成功です)。

## 使い方

```sh
# pcscd のカードで復号しながら録画
recisdb tune --device /dev/tornevideo0 --channel T33 -t 60 out.ts
# 復号しない
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

チャンネルの指定は px4_drv の ioctl(`PTX_SET_CHANNEL`)と同じです。地デジ UHF 13〜62ch は `freq_no = ch + 50` です。

## 制限

- **チューナーは1基で、地デジのみ。** 視聴、録画、番組表(EPG)の取得は、同時にできません。
- **電源を入れた直後の状態でしか起動できません。** ほかのソフトや OS が torne を使った後は、USB を抜き差ししてください。
- **選局に 2〜3 秒かかります。** レジスタの書き込みを USB で1つずつ送るためです(coax のリング・コマンドチャンネルによる高速化は移植していません)。
- **複数台は未確認です。** `tornevideo0`、`tornevideo1` の番号は、差した位置ではなく、認識された順です。複数台で使うときは、recisdb の `--card` で、チューナーとカードの組み合わせを自分で指定してください。
- **USB を抜き差ししても、デバイスは自動で復帰します。** ただし、止まった視聴や録画は、自動では再開しません。もう一度始めてください。
- **Ubuntu 22.04 / カーネル 5.15 以外は未確認です。**

## 構成

| 場所 | 内容 |
| --- | --- |
| `driver/` | カーネルモジュール(`torne_kern.c`)、OS に依存しない中核(`torne_bridge.c` `torne_dib.c` `torne_card.c`)、px4_drv の文字デバイス層 |
| `ifd/` | カードスロット用の pcscd リーダードライバー |
| `tools/` | `torne_user`(同じ中核を libusb でユーザー空間から動かす試験ツール)、試験スクリプト、表と機械語の生成プログラム(`tools/gen`) |
| `udev/` | udev ルール |

`driver/torne_fw_gen.h` と `driver/torne_dib_tables_gen.h` は、coax から自動生成したものです。作り方は [tools/gen/README.md](tools/gen/README.md) にあります。

## カーネルモジュールなしでの試験

カーネルモジュールを外してから使います(両方は同時に torne を使えません)。

```sh
sudo rmmod torne_drv
sudo tools/torne_user probe             # ブリッジの起動と DiB8000 の ID 確認
sudo tools/torne_user tune 33 10 > t.ts # 地デジ 33ch を 10 秒受信
sudo tools/torne_user card              # カードスロット(ATR と B-CAS の初期化コマンド)
tools/torne_user emu-trace              # 偽のメモリデバイスで USB 要求の列を出す
                                        # (coax の Go 版と同じ。coax の bulk コマンドチャンネルの設定だけは移植していないので除く)
```

## ライセンス

**GPL-2.0-only。** [LICENSE](LICENSE) と [NOTICE](NOTICE) を見てください。

Sony、PlayStation、torne は、それぞれの権利者の商標です。このプロジェクトは、Sony、DiBcom、富士通、PLEX、および [NOTICE](NOTICE) に挙げたプロジェクトとは、関係ありません。
