// SPDX-License-Identifier: GPL-2.0-only
// Generator for torne_drv. Copy this file into a coax checkout (see tools/gen/README.md) and run it there.

package dib8096

import (
	"fmt"
	"os"
	"strings"
	"testing"
)

// TestGenDibTables は dib8096 の表と基板設定を、実際の Go の値のまま C の定数として書き出す。
func TestGenDibTables(t *testing.T) {
	var b strings.Builder
	b.WriteString("/* generated from coax internal/coaxd/demod/dib8096 (tables.go, board.go) by gen_dib_test.go - do not edit */\n\n")

	u16 := func(name string, vals []uint16) {
		fmt.Fprintf(&b, "static const u16 %s[%d] = {", name, len(vals))
		for i, v := range vals {
			if i%10 == 0 {
				b.WriteString("\n\t")
			}
			fmt.Fprintf(&b, "%d, ", v)
		}
		b.WriteString("\n};\n\n")
	}
	s16 := func(name string, vals []int16) {
		fmt.Fprintf(&b, "static const s16 %s[%d] = {", name, len(vals))
		for i, v := range vals {
			if i%10 == 0 {
				b.WriteString("\n\t")
			}
			fmt.Fprintf(&b, "%d, ", v)
		}
		b.WriteString("\n};\n\n")
	}
	s32 := func(name string, vals []int32) {
		fmt.Fprintf(&b, "static const s32 %s[%d] = {", name, len(vals))
		for i, v := range vals {
			if i%10 == 0 {
				b.WriteString("\n\t")
			}
			fmt.Fprintf(&b, "%d, ", v)
		}
		b.WriteString("\n};\n\n")
	}

	s16("ana_fe_coeff_3seg", anaFECoeff3Seg[:])
	s16("ana_fe_coeff_1seg", anaFECoeff1Seg[:])
	s16("ana_fe_coeff_13seg", anaFECoeff13Seg[:])
	u16("dib8000_defaults", dib8000Defaults)
	u16("adc_target_16db", adcTarget16dB[:])
	u16("adp_q64", adpQ64[:])
	u16("adp_q16", adpQ16[:])
	u16("adp_q_default", adpQDefault[:])
	fmt.Fprintf(&b, "static const u8 permu_seg[13] = {")
	for _, v := range permuSeg {
		fmt.Fprintf(&b, "%d, ", v)
	}
	b.WriteString("};\n\n")
	fmt.Fprintf(&b, "static const struct linear_segment strength_to_db_table[%d] = {\n", len(strengthToDBTable))
	for _, s := range strengthToDBTable {
		fmt.Fprintf(&b, "\t{ %d, %d },\n", s.x, s.y)
	}
	b.WriteString("};\n\n")
	u16("bb_ramp_pwm_normal", bbRampPWMNormal)
	u16("bb_ramp_pwm_normal_socs", bbRampPWMNormalSoCs)
	u16("rf_ramp_pwm_cband_7090p", rfRampPWMCBand7090P)
	u16("rf_ramp_pwm_cband", rfRampPWMCBand)
	u16("rf_ramp_pwm_vhf", rfRampPWMVHF)
	u16("rf_ramp_pwm_uhf", rfRampPWMUHF)
	s32("lut_1000ln_mant", lut1000lnMant)
	u16("dib0090_defaults", dib0090Defaults)
	u16("dib0090_p1g_additional_defaults", dib0090P1GAdditionalDefaults)

	dc := func(name string, table []dcCalibration) {
		fmt.Fprintf(&b, "static const struct dc_calibration %s[%d] = {\n", name, len(table))
		for _, e := range table {
			fmt.Fprintf(&b, "\t{ 0x%x, %d, %d, 0x%x, %d },\n", e.addr, e.offset, e.pga, e.bb1, e.i)
		}
		b.WriteString("};\n\n")
	}
	dc("dc_table", dcTable)
	dc("dc_p1g_table", dcP1GTable)

	pll := func(name string, table []pllEntry) {
		fmt.Fprintf(&b, "static const struct pll_entry %s[%d] = {\n", name, len(table))
		for _, e := range table {
			fmt.Fprintf(&b, "\t{ %d, %d, %d, %d, %d },\n", e.maxFreq, e.vcoBand, e.hfdivCode, e.hfdiv, e.topresc)
		}
		b.WriteString("};\n\n")
	}
	pll("dib0090_pll_table", dib0090PLLTable)
	pll("dib0090_p1g_pll_table", dib0090P1GPLLTable)

	tune := func(name string, table []tuningEntry) {
		fmt.Fprintf(&b, "static const struct tuning_entry %s[%d] = {\n", name, len(table))
		for _, e := range table {
			fmt.Fprintf(&b, "\t{ %d, %d, %d, %d, 0x%x, 0x%x, 0x%x, 0x%x },\n", e.maxFreq, e.switchTrim, e.lnaTune, e.lnaBias, e.v2i, e.mix, e.load, e.tunerEnable)
		}
		b.WriteString("};\n\n")
	}
	tune("dib0090_tuning_table_fm_vhf_on_cband", dib0090TuningTableFMVHFOnCBand)
	tune("dib0090_tuning_table", dib0090TuningTable)
	tune("dib0090_p1g_tuning_table", dib0090P1GTuningTable)
	tune("dib0090_p1g_tuning_table_fm_vhf_on_cband", dib0090P1GTuningTableFMVHFOnCBand)

	wbd := func(name string, table []wbdSlope) {
		fmt.Fprintf(&b, "static const struct wbd_slope %s[%d] = {\n", name, len(table))
		for _, e := range table {
			fmt.Fprintf(&b, "\t{ %d, %d, %d, %d, %d, %d },\n", e.maxFreq, e.slopeCold, e.offsetCold, e.slopeHot, e.offsetHot, e.wbdGain)
		}
		b.WriteString("};\n\n")
	}
	wbd("dib0090_wbd_table_default", dib0090WBDTableDefault)
	wbd("dib8090_wbd_table", dib8090WBDTable)

	fmt.Fprintf(&b, "static const struct agc_config dib8090_agc_config[%d] = {\n", len(dib8090AGCConfig))
	for _, a := range dib8090AGCConfig {
		fmt.Fprintf(&b, "\t{ .band_caps = 0x%x, .setup = %d, .inv_gain = %d, .time_stabiliz = %d, .alpha_level = %d, .thlock = %d,\n", a.bandCaps, a.setup, a.invGain, a.timeStabiliz, a.alphaLevel, a.thlock)
		fmt.Fprintf(&b, "\t  .wbd_inv = %d, .wbd_ref = %d, .wbd_sel = %d, .wbd_alpha = %d,\n", a.wbdInv, a.wbdRef, a.wbdSel, a.wbdAlpha)
		fmt.Fprintf(&b, "\t  .agc1_max = %d, .agc1_min = %d, .agc2_max = %d, .agc2_min = %d,\n", a.agc1Max, a.agc1Min, a.agc2Max, a.agc2Min)
		fmt.Fprintf(&b, "\t  .agc1_pt1 = %d, .agc1_pt2 = %d, .agc1_pt3 = %d, .agc1_slope1 = %d, .agc1_slope2 = %d,\n", a.agc1Pt1, a.agc1Pt2, a.agc1Pt3, a.agc1Slope1, a.agc1Slope2)
		fmt.Fprintf(&b, "\t  .agc2_pt1 = %d, .agc2_pt2 = %d, .agc2_slope1 = %d, .agc2_slope2 = %d,\n", a.agc2Pt1, a.agc2Pt2, a.agc2Slope1, a.agc2Slope2)
		fmt.Fprintf(&b, "\t  .alpha_mant = %d, .alpha_exp = %d, .beta_mant = %d, .beta_exp = %d, .perform_agc_softsplit = %d },\n", a.alphaMant, a.alphaExp, a.betaMant, a.betaExp, a.performAGCSoftsplit)
	}
	b.WriteString("};\n\n")

	p := dib8090PLLConfig12MHz
	fmt.Fprintf(&b, "static const struct bandwidth_config dib8090_pll_config_12mhz = {\n")
	fmt.Fprintf(&b, "\t.internal = %d, .sampling = %d, .pll_prediv = %d, .pll_ratio = %d, .pll_range = %d, .pll_reset = %d, .pll_bypass = %d,\n", p.internal, p.sampling, p.pllPrediv, p.pllRatio, p.pllRange, p.pllReset, p.pllBypass)
	fmt.Fprintf(&b, "\t.enable_refdiv = %d, .bypclk_div = %d, .io_clk_en_core = %d, .adc_clk_src = %d, .modulo = %d,\n", p.enableRefdiv, p.bypclkDiv, p.ioClkEnCore, p.adcClkSrc, p.modulo)
	fmt.Fprintf(&b, "\t.sad_cfg = %d, .ifreq = %d, .timf = %d, .xtal_hz = %d,\n};\n\n", p.sadCfg, p.ifreq, p.timf, p.xtalHz)

	c := dib809xDiB8000Config()
	fmt.Fprintf(&b, "#define DIB8000_CFG_OUTPUT_MPEG2_IN_188_BYTES %d\n#define DIB8000_CFG_TUNER_IS_BASEBAND %d\n", c.outputMPEG2In188Bytes, c.tunerIsBaseband)
	fmt.Fprintf(&b, "#define DIB8000_CFG_GPIO_DIR 0x%x\n#define DIB8000_CFG_GPIO_VAL 0x%x\n#define DIB8000_CFG_GPIO_PWM_POS 0x%x\n#define DIB8000_CFG_PWM_FREQ_DIV %d\n", c.gpioDir, c.gpioVal, c.gpioPWMPos, c.pwmFreqDiv)
	fmt.Fprintf(&b, "#define DIB8000_CFG_HOSTBUS_DIVERSITY %d\n#define DIB8000_CFG_DIV_CFG 0x%x\n#define DIB8000_CFG_OUTPUT_MODE %d\n#define DIB8000_CFG_DRIVES 0x%x\n#define DIB8000_CFG_DIVERSITY_DELAY %d\n#define DIB8000_CFG_REFCLKSEL %d\n", c.hostbusDiversity, c.divCfg, c.outputMode, c.drives, c.diversityDelay, c.refclksel)

	t0 := dib809xDiB0090Config()
	fmt.Fprintf(&b, "#define DIB0090_CFG_CLKOUTTOBAMSE %d\n#define DIB0090_CFG_ANALOG_OUTPUT %d\n#define DIB0090_CFG_USE_PWM_AGC %d\n#define DIB0090_CFG_CLKOUTDRIVE %d\n", t0.clkouttobamse, t0.analogOutput, t0.usePWMAGC, t0.clkoutdrive)
	fmt.Fprintf(&b, "#define DIB0090_CFG_FREQ_OFFSET_KHZ_UHF (%d)\n#define DIB0090_CFG_FREQ_OFFSET_KHZ_VHF (%d)\n#define DIB0090_CFG_F_REF_CLOCK_RATIO %d\n", t0.freqOffsetKHzUHF, t0.freqOffsetKHzVHF, t0.fRefClockRatio)
	fmt.Fprintf(&b, "#define DIB0090_CFG_IO_PLL_BYPASS %d\n#define DIB0090_CFG_IO_PLL_RANGE %d\n#define DIB0090_CFG_IO_PLL_PREDIV %d\n#define DIB0090_CFG_IO_PLL_LOOPDIV %d\n#define DIB0090_CFG_IO_ADC_CLOCK_RATIO %d\n#define DIB0090_CFG_IO_PLL_INT_LOOP_FILT %d\n#define DIB0090_CFG_IO_CLOCK_KHZ %d\n",
		t0.io.pllBypass, t0.io.pllRange, t0.io.pllPrediv, t0.io.pllLoopdiv, t0.io.adcClockRatio, t0.io.pllIntLoopFilt, t0.io.clockKHz)
	fmt.Fprintf(&b, "#define DIB0090_CFG_WBD_VHF_OFFSET %d\n#define DIB0090_CFG_WBD_CBAND_OFFSET %d\n#define DIB0090_CFG_FORCE_CRYSTAL_MODE %d\n", t0.wbdVHFOffset, t0.wbdCBandOffset, t0.forceCrystalMode)

	if err := os.WriteFile(os.Getenv("GEN_OUT"), []byte(b.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}
