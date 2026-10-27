# torne_drv - Sony CECH-ZD1 J (PS3「torne」) 用 Linux ドライバー

PS3 用の地デジチューナー **Sony CECH-ZD1 J(torne)** を、[px4_drv](https://github.com/tsukumijima/px4_drv) と同じ形で使えるようにする、非公式の Linux ドライバーです。

[English](README.en.md)

- **映像:** `/dev/tornevideoN` ができます。`/dev/px4video*` と同じ ioctl・`read()` なので、`recisdb`、`recpt1`、Mirakurun、EDCB、KonomiTV から、PLEX のチューナーと同じように使えます。
- **B-CAS カード:** torne 本体のカードスロットを、pcscd から普通のリーダーとして使えます(`/dev/tornecardN` と pcscd 用ドライバー)。カードは torne に差したままにできます。
- **地デジ(ISDB-T)のみ。** 1台につきチューナー1基です。**torne 専用**で、ほかの機器には対応しません。
- **DKMS + Debian パッケージ対応。** `.deb` を入れるだけで、カーネルが更新されたときに、ドライバーが自動で再ビルドされます。

> **ベータ版です。** 実機(torne 1台)で動作を確認したのは、Ubuntu 22.04(カーネル 5.15)・x86-64 だけです。新しいカーネルについては、[確認済みのカーネル](#確認済みのカーネル) を見てください。

## 元にしたもの

torne の富士通 MB86H47A という USB ブリッジの動かし方は、[coax](https://github.com/Till0196/coax)(Till0196 さん)が解析したものです。このドライバーは、coax のブリッジと DiB8096 の処理を Go から C に移して、カーネルモジュールにし、px4_drv(本家 [nns779](https://github.com/nns779/px4_drv) と、メンテナンスを引き継いだ [tsukumijima 版](https://github.com/tsukumijima/px4_drv) 0.6.1)の文字デバイス層を載せたものです。詳しくは [NOTICE](NOTICE) を見てください。

## インストール (Debian / Ubuntu)

Debian パッケージは [リリースページ](https://github.com/mihalinn/torne_drv/releases) にあります。ドライバー本体(DKMS のソースパッケージなので、CPU の種類によらず共通)と、B-CAS カードスロット用の pcscd ドライバー(amd64)の2つです。

```bash
# Debian パッケージをダウンロードしてインストール
wget https://github.com/mihalinn/torne_drv/releases/download/v0.1.0/torne-drv-dkms_0.1.0_all.deb
wget https://github.com/mihalinn/torne_drv/releases/download/v0.1.0/torne-drv-pcsc_0.1.0_amd64.deb
sudo apt install -y ./torne-drv-dkms_0.1.0_all.deb ./torne-drv-pcsc_0.1.0_amd64.deb
rm ./torne-drv-dkms_0.1.0_all.deb ./torne-drv-pcsc_0.1.0_amd64.deb

# ドライバーをロード (インストール時にもロードされますが、念のため。次回の起動からは自動でロードされます)
sudo modprobe torne_drv

# torne_drv がロードされているかを確認 (ちゃんとログが出てきたら OK)
lsmod | grep -e ^torne_drv
dmesg | grep torne
ls -la /dev | grep -e torne
```

`/dev/tornevideo0` と `/dev/tornecard0` が見えれば成功です。見えないときは、USB の接続を確認したり、抜き差ししてください。

`torne-drv-pcsc` を入れると、pcscd が torne のカードスロットを「Sony CECH-ZD1 J Smart Card Reader」として認識します。確認は次のとおりです(カードは torne のスロットに差しておきます)。

```bash
pcsc_scan
```

`torne-drv-pcsc` を入れなければ、外付けのカードリーダー(SCR3310 など)を、これまでどおり使えます。

アンインストールは、`sudo apt remove torne-drv-dkms torne-drv-pcsc` です。

### Debian パッケージを使わずに入れる

ソースから入れる場合です(Debian 系以外のディストリビューションなど)。

```bash
sudo apt install dkms linux-headers-$(uname -r) build-essential libpcsclite-dev pcscd   # Debian / Ubuntu の場合
git clone https://github.com/mihalinn/torne_drv.git && cd torne_drv
make                  # pcscd 用ドライバー(ifd/)と試験ツール(tools/)をビルド
sudo ./install.sh     # DKMS のカーネルモジュール、udev ルール、pcscd ドライバーを入れる
```

取り外しは `sudo ./install.sh uninstall` です。自分で Debian パッケージを作るには、`./packaging/build-deb.sh`(`dist/` に出力されます)です。

## 使い方

```bash
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

pcscd に、torne のカードスロット以外のリーダー(外付けリーダーなど)もある場合は、recisdb が最初のリーダーを選びます。使うリーダーを指定したいときは、`--card` を使ってください。

### EDCB から直接使う (BonDriver_LinuxPTX)

[BonDriver_LinuxPTX](https://github.com/nns779/BonDriver_LinuxPTX) でも使えます。`BonDriver_LinuxPTX.ini` の `Device` と `Space` を、次のように変えてください。torne は地デジ専用なので、BS/CS の空間は外します。

```ini
Name="torne"
Device=/dev/tornevideo0
Space=UHF
```

- 信号のないチャンネルの選局は、失敗するまでに約 7 秒かかります。チャンネルスキャンは、数分かかります。
- ドライバーは、受信中にデータが 100 ミリ秒以上途切れると、空のパケット(PID 0x1FFF)を流します。PLEX のチューナーと同じく、データが流れ続けないと、BonDriver_LinuxPTX が終了できなくなるためです(v0.1.0 では、スキャンの後に終了できなくなる不具合がありました)。止めたいときは、`modprobe torne_drv idle_fill_ms=0` です。

## 確認済みのカーネル

| ディストリビューション | カーネル | 確認した内容 |
| --- | --- | --- |
| Ubuntu 22.04 | 5.15 | **実機の torne で動作確認**(視聴、録画、番組表の取得、B-CAS の復号、USB の抜き差し、`.deb` からの導入) |
| Ubuntu 24.04 | 6.8 | ビルド、DKMS の導入と削除、モジュールの読み込み(**実機の torne は未接続**) |
| Ubuntu 24.04 (HWE) | 7.0 | ビルドと DKMS の導入のみ(**実機の torne は未接続**) |

x86-64 だけを確認しています。ARM は未確認です。ほかのカーネルで問題があれば、issue で教えてください。

## 制限

- **チューナーは1基で、地デジのみ。** 視聴、録画、番組表(EPG)の取得は、同時にできません。
- **電源を入れた直後の状態でしか起動できません。** ほかのソフトや OS が torne を使った後は、USB を抜き差ししてください。
- **選局に 2〜3 秒かかります。** レジスタの書き込みを USB で1つずつ送るためです(coax のリング・コマンドチャンネルによる高速化は移植していません)。
- **複数台は未確認です。** `tornevideo0`、`tornevideo1` の番号は、差した位置ではなく、認識された順です。複数台で使うときは、recisdb の `--card` で、チューナーとカードの組み合わせを自分で指定してください。
- **USB を抜き差ししても、デバイスは自動で復帰します。** ただし、止まった視聴や録画は、自動では再開しません。もう一度始めてください。

## 構成

| 場所 | 内容 |
| --- | --- |
| `driver/` | カーネルモジュール(`torne_kern.c`)、OS に依存しない中核(`torne_bridge.c` `torne_dib.c` `torne_card.c`)、px4_drv の文字デバイス層 |
| `ifd/` | カードスロット用の pcscd リーダードライバー |
| `packaging/` | Debian パッケージを作るスクリプト |
| `tools/` | `torne_user`(同じ中核を libusb でユーザー空間から動かす試験ツール)、試験スクリプト、表と機械語の生成プログラム(`tools/gen`) |
| `udev/` | udev ルール |

`driver/torne_fw_gen.h` と `driver/torne_dib_tables_gen.h` は、coax から自動生成したものです。作り方は [tools/gen/README.md](tools/gen/README.md) にあります。

## カーネルモジュールなしでの試験

カーネルモジュールを外してから使います(両方は同時に torne を使えません)。

```bash
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
