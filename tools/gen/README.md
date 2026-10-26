# Regenerating the generated headers

`driver/torne_fw_gen.h` (FR30 machine code, register addresses) and `driver/torne_dib_tables_gen.h` (DiB8096 tables and board
configuration) are generated from [coax](https://github.com/Till0196/coax) (tested with v0.1.0), which builds the machine code
with its own program builder. The Go files here are tests that print C source.

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

`trace_test.go` (`TRACE_OUT=file go test ... -run TestTraceTorne`) writes the USB request sequence of coax's bridge against a fake
memory device. `tools/torne_user emu-trace` prints the same for the C code, so the two can be compared with `diff`
(only the time-derived value written at 0x017a3c00 and coax's bulk command channel setup differ).
