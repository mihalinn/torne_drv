# 自動生成ヘッダーの作り直し

`driver/torne_fw_gen.h`(FR30 の機械語とレジスタのアドレス)と `driver/torne_dib_tables_gen.h`(DiB8096 の表とボード設定)は、
[coax](https://github.com/Till0196/coax)(v0.1.0 で確認)から生成したものです。機械語は、coax 自身のプログラムビルダーで作っています。
このディレクトリの Go ファイルは、C のソースを出力するテストです。

```sh
git clone https://github.com/Till0196/coax && cd coax
cp /path/to/torne_drv/tools/gen/dump_test.go  internal/coaxd/usb/bridge/mb86h47/
cp /path/to/torne_drv/tools/gen/trace_test.go internal/coaxd/usb/bridge/mb86h47/
cp /path/to/torne_drv/tools/gen/gen_dib_test.go internal/coaxd/demod/dib8096/

DUMP_OUT=/path/to/torne_drv/driver/torne_fw_gen.h \
    go test ./internal/coaxd/usb/bridge/mb86h47/ -run TestDumpTorne -count=1
GEN_OUT=/path/to/torne_drv/driver/torne_dib_tables_gen.h \
    go test ./internal/coaxd/demod/dib8096/ -run TestGenDibTables -count=1
```

`trace_test.go`(`TRACE_OUT=file go test ... -run TestTraceTorne`)は、coax のブリッジが偽のメモリデバイスに対して出す USB 要求の列を、ファイルに書き出します。
`tools/torne_user emu-trace` が、C 版の同じ列を出力するので、`diff` で比べられます
(違うのは、0x017a3c00 に書く時刻由来の値と、coax の bulk コマンドチャンネルの設定だけです)。
