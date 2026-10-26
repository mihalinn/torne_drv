// SPDX-License-Identifier: GPL-2.0-only
// Generator for torne_drv. Copy this file into a coax checkout (see tools/gen/README.md) and run it there.

package mb86h47

import (
	"fmt"
	"os"
	"strings"
	"testing"

	"github.com/Till0196/coax/internal/coaxd/usb/bridge/mb86h47/regs"
)

// TestDumpTorne は MB86H47A 用に coax が生成する FR30 プログラム (機械語) と、
// Start() で使うレジスタアドレスを C の配列/定数として書き出す。
func TestDumpTorne(t *testing.T) {
	var b strings.Builder
	arr := func(name string, words []uint32) {
		fmt.Fprintf(&b, "static const u32 %s[%d] = {", name, len(words))
		for i, w := range words {
			if i%6 == 0 {
				b.WriteString("\n\t")
			}
			fmt.Fprintf(&b, "0x%08x, ", w)
		}
		b.WriteString("\n};\n\n")
	}
	b.WriteString("/* generated from coax (Till0196) mb86h47 by dump_test.go — do not edit */\n\n")
	arr("torne_trap_recorder", trapRecorder)
	arr("torne_trap_stubs", trapStubCode)
	arr("torne_core1_program", core1Program)
	arr("torne_core0_program", core0Program)
	for i := range addressings {
		arr(fmt.Sprintf("torne_addressing%d_code", i), addressings[i].code)
		fmt.Fprintf(&b, "static const struct torne_handler_word torne_addressing%d_words[%d] = {\n", i, len(addressings[i].words))
		for _, w := range addressings[i].words {
			fmt.Fprintf(&b, "\t{ 0x%x, 0x%08x, 0x%08x },\n", w.address, w.original, w.widened)
		}
		b.WriteString("};\n\n")
	}
	fmt.Fprintf(&b, "#define TORNE_TRAP_STUB_STRIDE %d\n", trapStubStride)
	fmt.Fprintf(&b, "#define TORNE_TRAP_RECORDER_WORDS %d\n", len(trapRecorder))
	fmt.Fprintf(&b, "#define TORNE_TRAP_STUBS_WORDS %d\n", len(trapStubCode))
	fmt.Fprintf(&b, "#define TORNE_CORE1_PROGRAM_WORDS %d\n", len(core1Program))
	fmt.Fprintf(&b, "#define TORNE_CORE0_PROGRAM_WORDS %d\n", len(core0Program))
	fmt.Fprintf(&b, "#define TORNE_ADDRESSING_WORDS %d\n", len(addressings[0].code))
	b.WriteString("\n/* register addresses used by Start() */\n")
	c, m := regs.Clock, regs.Memory
	fmt.Fprintf(&b, "#define TORNE_REG_CLOCK_PLL 0x%x\n", c.PLL.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_CLOCK_UNK114 0x%x\n", c.Unk114.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_CLOCK_GATES 0x%x\n", c.Gates.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_CLOCK_UNK11C 0x%x\n", c.Unk11C.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_CLOCK_UNK128 0x%x\n", c.Unk128.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_CLOCK_UNK12C 0x%x\n", c.Unk12C.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_MEMORY_FORMAT2_1 0x%x\n", m.Format2[1].Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_MEMORY_P2_2_C0 0x%x\n", m.Planes2[2].Chroma[0].Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_MEMORY_P2_2_C1 0x%x\n", m.Planes2[2].Chroma[1].Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_MEMORY_P2_4_C0 0x%x\n", m.Planes2[4].Chroma[0].Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_MEMORY_P2_4_C1 0x%x\n", m.Planes2[4].Chroma[1].Addr())
	u := regs.USB
	fmt.Fprintf(&b, "#define TORNE_REG_USB_ENABLE 0x%x\n", u.Enable.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_USB_RESET 0x%x\n", u.Reset.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_USB_UNK0C0 0x%x\n", u.Unk0C0.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_USB_UNK0C4 0x%x\n", u.Unk0C4.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_I2C_UNK18 0x%x\n", u.I2C.Unk18.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_I2C_DIVIDER 0x%x\n", u.I2C.Divider.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_I2C_CLOCK 0x%x\n", u.I2C.Clock.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_I2C_CONTROL 0x%x\n", u.I2C.Control.Addr())
	for i := 0; i < 3; i++ {
		fmt.Fprintf(&b, "#define TORNE_REG_USB_DMA%d_CONTROL 0x%x\n#define TORNE_REG_USB_DMA%d_CONFIG 0x%x\n", i, u.DMA[i].Control.Addr(), i, u.DMA[i].Config.Addr())
	}
	fmt.Fprintf(&b, "#define TORNE_REG_USB_DMA8_CONTROL 0x%x\n#define TORNE_REG_USB_DMA8_CONFIG 0x%x\n", u.DMA[8].Control.Addr(), u.DMA[8].Config.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_USB_EP1_CONTROL 0x%x\n", u.Endpoint[1].Control.Addr())
	fmt.Fprintf(&b, "#define TORNE_REG_USB_EPMODE 0x%x\n", u.EPMode.Addr())
	fmt.Fprintf(&b, "#define TORNE_USB_BLOCK_I2C 0x%x\n#define TORNE_USB_BLOCK_STREAM 0x%x\n", regs.USBBlockI2C, regs.USBBlockStream)
	fmt.Fprintf(&b, "#define TORNE_CLUSTER_STANDBY_WAIT %d\n", regs.ClusterStandbyWait)
	for i, base := range standbyBlocks {
		fmt.Fprintf(&b, "#define TORNE_STANDBY_BLOCK_%d_MODE 0x%x\n", i, regs.ClusterAt(base).StandbyMode.Addr())
	}
	fmt.Fprintf(&b, "#define TORNE_STANDBY_BLOCKS %d\n", len(standbyBlocks))
	if err := os.WriteFile(os.Getenv("DUMP_OUT"), []byte(b.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}
