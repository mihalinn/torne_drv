// SPDX-License-Identifier: GPL-2.0-only
// Generator for torne_drv. Copy this file into a coax checkout (see tools/gen/README.md) and run it there.

package mb86h47

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"
)

// memDevice は MB86H47A のメモリ要求 (vendor 0xbc) を、ただのメモリとして模倣する偽デバイス。
// Start / ResetDemodulator / StartStream / StopStream が出す要求の列 (トレース) を取り出すために使う。
type memDevice struct {
	mem   map[uint32]byte
	log   *strings.Builder
	alive uint32
}

func (d *memDevice) addr(value, index uint16) uint32 {
	return uint32(value>>8)<<16 | uint32(value&0xff)<<24 | uint32(index)
}

func (d *memDevice) Control(requestType, request byte, value, index uint16, buffer []byte, timeoutMS uint32) (int, error) {
	if request != requestMemory {
		return 0, errors.New("unexpected request")
	}
	a := d.addr(value, index)
	if requestType == vendorIn {
		if a == core1Base+mailbox+mailboxAlive {
			d.alive++
			binary.BigEndian.PutUint32(buffer, d.alive)
			fmt.Fprintf(d.log, "R %08x %d (alive)\n", a, len(buffer))
			return len(buffer), nil
		}
		for i := range buffer {
			buffer[i] = d.mem[a+uint32(i)]
		}
		fmt.Fprintf(d.log, "R %08x %d\n", a, len(buffer))
		return len(buffer), nil
	}
	for i, b := range buffer {
		d.mem[a+uint32(i)] = b
	}
	fmt.Fprintf(d.log, "W %08x %x\n", a, buffer)
	return len(buffer), nil
}

func (d *memDevice) Bulk(endpoint byte, buffer []byte, timeoutMS uint32) (int, error) {
	fmt.Fprintf(d.log, "B %02x %d\n", endpoint, len(buffer))
	return 0, errors.New("no bulk")
}

// TestTraceTorne は Start → ResetDemodulator → StartStream → StopStream の要求の列を書き出す。
func TestTraceTorne(t *testing.T) {
	var log strings.Builder
	dev := &memDevice{mem: map[uint32]byte{}, log: &log}
	// the memory request handler as it is at power-up: Start checks it before widening it
	for _, w := range addressings[0].words {
		var word [4]byte
		binary.BigEndian.PutUint32(word[:], w.original)
		for i := range word {
			dev.mem[w.address+uint32(i)] = word[i]
		}
	}
	bridge := New(dev)
	step := func(name string, err error) {
		fmt.Fprintf(&log, "# %s -> %v\n", name, err)
	}
	step("Start", bridge.Start())
	step("ResetDemodulator", bridge.ResetDemodulator())
	step("StartStream", bridge.StartStream())
	step("StopStream", bridge.StopStream())
	if err := os.WriteFile(os.Getenv("TRACE_OUT"), []byte(log.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}
