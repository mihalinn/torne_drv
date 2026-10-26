// SPDX-License-Identifier: GPL-2.0-only
/*
 * torne_dib.c - DiBcom DiB8096 (DiB8000 ISDB-T demodulator + DiB0090 tuner) of the Sony CECH-ZD1 J
 *
 * Copyright (C) 2005-9 DiBcom, SA et al   (Linux dib8000.c, dib0090.c, dibx000_common.c)
 * Port to Go: Copyright 2026 Till0196 (coax, internal/coaxd/demod/dib8096), GPL-2.0-only
 * Port to C: this file. Function and register names follow the Linux and coax sources so that the
 * three can be read side by side. Register numbers are kept in decimal as Linux writes them.
 *
 * Only 13-segment ISDB-T at 6 MHz is supported, with every transmission parameter found by the
 * demodulator's own search.
 */
#include "torne_dib.h"

#ifdef __KERNEL__
#include <linux/math64.h>
#define div_u64_(a, b) div64_u64(a, b)
#define div_s64_(a, b) div64_s64(a, b)
#else
#define div_u64_(a, b) ((u64)(a) / (u64)(b))
#define div_s64_(a, b) ((s64)(a) / (s64)(b))
#endif

#define LOG(d, lvl, ...)                                                           \
	do {                                                                       \
		if ((d)->br->ops->log && (lvl) <= (d)->br->log_level)              \
			(d)->br->ops->log((d)->br->priv, (lvl), __VA_ARGS__);      \
	} while (0)

/* ---- constants (dibx000_common.h, dib8000.c, dib0090.c) ---- */

#define BAND_LBAND 0x01
#define BAND_UHF 0x02
#define BAND_VHF 0x04
#define BAND_SBAND 0x08
#define BAND_FM 0x10
#define BAND_CBAND 0x20

enum {
	OUTMODE_HIGH_Z = 0,
	OUTMODE_PARALLEL_GATED = 1,
	OUTMODE_PARALLEL_CONT = 2,
	OUTMODE_DIVERSITY = 4,
	OUTMODE_FIFO = 5,
	OUTMODE_ANALOG_ADC = 6,
	OUTMODE_SERIAL = 7,
};

enum { ADC_SLOW_ON, ADC_SLOW_OFF, ADC_ON, ADC_OFF, ADC_VBG_ENABLE, ADC_VBG_DISABLE };

enum {
	TUNER_START = 10,
	TUNER_STEP0,
	TUNER_STEP1,
	TUNER_STEP2,
	TUNER_STEP3,
	TUNER_STEP4,
	TUNER_STEP5,
	TUNER_STEP6,
	TUNER_STEP7,
	TUNER_STOP,
};

enum { AGC_START = 20, AGC_STEP0, AGC_STEP1, AGC_STEP2, AGC_STEP3, AGC_STEP4, AGC_STOP };

enum {
	DEMOD_START = 30,
	DEMOD_STEP1,
	DEMOD_STEP2,
	DEMOD_STEP3,
	DEMOD_STEP4,
	DEMOD_STEP5,
	DEMOD_STEP6,
	DEMOD_STEP7,
	DEMOD_STEP8,
	DEMOD_STEP9,
	DEMOD_STEP10,
	DEMOD_STEP11,
};

enum { DEMOD_SEARCH_NEXT = 51, DEMOD_STEP_LOCKED, DEMOD_STOP };

#define STATUS_TUNE_FAILED 0
#define STATUS_TUNE_TIMED_OUT (-1)
#define STATUS_TUNE_TIME_TOO_SHORT (-2)
#define STATUS_TUNE_PENDING (-3)
#define STATUS_STD_SUCCESS (-4)
#define STATUS_FFT_SUCCESS (-5)
#define STATUS_DEMOD_SUCCESS (-6)
#define STATUS_LOCKED (-7)
#define STATUS_DATA_LOCKED (-8)

#define CALLBACK_TIME_NEVER 0xffffffffu

enum { TIMF_SET = 0, TIMF_GET = 1, TIMF_UPDATE = 2 };

enum { INTERFACE_TUNER = 0 };
#define DEVICE_DIB7000 2
#define DEVICE_DIB8000 13

/* enum fe_modulation / fe_code_rate / fe_transmit_mode / fe_guard_interval values as in linux/dvb/frontend.h */
enum {
	MODULATION_QPSK = 0,
	MODULATION_QAM16 = 1,
	MODULATION_QAM32 = 2,
	MODULATION_QAM64 = 3,
	MODULATION_QAM_AUTO = 6,
	MODULATION_DQPSK = 12,
};
enum { FEC_NONE = 0, FEC_1_2 = 1, FEC_2_3 = 2, FEC_3_4 = 3, FEC_4_5 = 4, FEC_5_6 = 5, FEC_6_7 = 6, FEC_7_8 = 7, FEC_AUTO = 9 };
enum { TRANSMISSION_2K = 0, TRANSMISSION_8K = 1, TRANSMISSION_AUTO = 2, TRANSMISSION_4K = 3 };
enum { GUARD_1_32 = 0, GUARD_1_16 = 1, GUARD_1_8 = 2, GUARD_1_4 = 3, GUARD_AUTO = 4 };
#define INVERSION_AUTO 2
#define SYSTEM_ISDBT 8

enum { AUTOSEARCH_START = 0, AUTOSEARCH_SEARCHING_FFT = 1, AUTOSEARCH_SEARCHING_GUARD = 2, AUTOSEARCH_DONE = 100 };
enum { POWER_ALL = 0, POWER_INTERFACE_ONLY };
enum { LOOP_TUNE1 = 0, LOOP_TUNE2 };

/* enable bits of dib0090 registers 0x1b and 0x24 */
#define EN_IQADC 0x0040
#define EN_PLL 0x0020
#define EN_BB 0x0008
#define EN_LO 0x0004
#define EN_BIAS 0x0001
#define EN_DIG_CLK 0x0080
#define EN_CRYSTAL 0x0002
#define EN_UHF 0x22E9
#define EN_VHF 0x44E9
#define EN_CAB 0x88E9

#define DC_CAL 0x1
#define WBD_CAL 0x2
#define TEMP_CAL 0x4
#define CAPTRIM_CAL 0x8

#define KROSUS_PLL_LOCKED 0x800
#define KROSUS 0x2
#define SOC 0x02
#define P1AB 0x0
#define P1C 0x1
#define P1DEF 0x3
#define P1G 0x7
#define P1G21R2 0xf
#define MP001 0x1
#define MP005 0x4
#define MP008 0x6
#define MP009 0x7

#define TUNER_ADDRESS (0x60 << 1)
#define MIN_FREQUENCY_HZ 45000000u
#define MAX_FREQUENCY_HZ 860000000u
#define DEFAULT_TUNE_TIMEOUT_MS 5000
#define MIN_TUNE_STEP_MS 10

#define FE_HAS_SIGNAL TORNE_FE_HAS_SIGNAL
#define FE_HAS_CARRIER TORNE_FE_HAS_CARRIER
#define FE_HAS_VITERBI TORNE_FE_HAS_VITERBI
#define FE_HAS_SYNC TORNE_FE_HAS_SYNC
#define FE_HAS_LOCK TORNE_FE_HAS_LOCK

#include "torne_dib_tables_gen.h"

/* ---- helpers ---- */

static inline u16 bw(bool v)
{
	return v ? 1 : 0;
}

static inline int iabs(int v)
{
	return v < 0 ? -v : v;
}

static void record(struct torne_dib *d, int err)
{
	if (err && !d->err)
		d->err = err;
}

static void msleep_(struct torne_dib *d, unsigned int ms)
{
	d->br->ops->msleep(d->br->priv, ms);
}

static u64 now_ms(struct torne_dib *d)
{
	return d->br->ops->now_ms(d->br->priv);
}

static u8 band_of_frequency(u32 khz)
{
	if (khz <= 170000)
		return BAND_CBAND;
	if (khz <= 250000)
		return BAND_VHF;
	if (khz <= 863000)
		return BAND_UHF;
	if (khz <= 2000000)
		return BAND_LBAND;
	return BAND_SBAND;
}

static u32 fls8(u8 v)
{
	u32 n = 0;

	while (v) {
		n++;
		v >>= 1;
	}
	return n;
}

/* ---- DiB8000 register access ---- */

static u16 dib_read_word(struct torne_dib *d, u16 reg)
{
	u8 w[2], r[2];
	int rc;

	if (d->err)
		return 0;
	w[0] = reg >> 8;
	w[1] = reg;
	rc = torne_i2c_write_read(d->br, TORNE_DIB_I2C_ADDRESS, w, 2, r, 2);
	if (rc) {
		LOG(d, TORNE_LOG_ERR, "read DiB8000 register %u: %d", reg, rc);
		record(d, rc);
		return 0;
	}
	return (u16)r[0] << 8 | r[1];
}

static void dib_write_word(struct torne_dib *d, u16 reg, u16 value)
{
	u8 w[4];
	int rc;

	if (d->err)
		return;
	w[0] = reg >> 8;
	w[1] = reg;
	w[2] = value >> 8;
	w[3] = value;
	rc = torne_i2c_write(d->br, TORNE_DIB_I2C_ADDRESS, w, 4);
	if (rc) {
		LOG(d, TORNE_LOG_ERR, "write DiB8000 register %u: %d", reg, rc);
		record(d, rc);
	}
}

static void dib_modify_word(struct torne_dib *d, u16 reg, u16 keep, u16 set)
{
	if (d->err)
		return;
	dib_write_word(d, reg, (u16)((dib_read_word(d, reg) & keep) | set));
}

static u32 dib_read32(struct torne_dib *d, u16 reg)
{
	u16 high = dib_read_word(d, reg + 0);
	u16 low = dib_read_word(d, reg + 1);

	return (u32)high << 16 | low;
}

/* ---- dibx000 I2C master gate ---- */

static u16 gate_control(u8 address7, bool open)
{
	u16 value;

	if (open)
		value = (u16)address7 << 8;
	else
		value = 1 << 7;
	/* later chips moved the field up one bit */
	return (u16)(value << 1);
}

static void master_initialize(struct torne_dib *d)
{
	d->master.base_register = 1024;
	dib_write_word(d, d->master.base_register + 1, gate_control(0, false));
	d->master.gate = 0;
}

static void master_select_interface(struct torne_dib *d, u16 intf)
{
	if (d->master.selected_interface != intf) {
		d->master.selected_interface = intf;
		dib_write_word(d, d->master.base_register + 4, intf);
	}
}

static void master_reset(struct torne_dib *d)
{
	dib_write_word(d, d->master.base_register + 1, gate_control(0, false));
	d->master.gate = 0;
	/* force a select the first time */
	d->master.selected_interface = 0xff;
	master_select_interface(d, INTERFACE_TUNER);
}

/* messages to the tuner go out on the host's bus once the DiB8000 has opened its gate to the tuner's address */
static int tuner_gate(struct torne_dib *d, u8 address)
{
	if (d->master.gate != address) {
		master_select_interface(d, INTERFACE_TUNER);
		dib_write_word(d, d->master.base_register + 1, gate_control(address >> 1, true));
		if (d->err)
			return d->err;
		d->master.gate = address;
	}
	return 0;
}

/* ---- DiB0090 register access ---- */

static u16 tun_read_reg(struct torne_dib *d, u8 reg)
{
	u8 r[2];
	int rc;

	if (d->err)
		return 0;
	rc = tuner_gate(d, TUNER_ADDRESS);
	if (!rc)
		rc = torne_i2c_write_read(d->br, TUNER_ADDRESS, &reg, 1, r, 2);
	if (rc) {
		LOG(d, TORNE_LOG_ERR, "read DiB0090 register %#02x: %d", reg, rc);
		record(d, rc);
		return 0;
	}
	return (u16)r[0] << 8 | r[1];
}

static void tun_write_reg(struct torne_dib *d, u8 reg, u16 value)
{
	u8 w[3];
	int rc;

	if (d->err)
		return;
	w[0] = reg;
	w[1] = value >> 8;
	w[2] = value;
	rc = tuner_gate(d, TUNER_ADDRESS);
	if (!rc)
		rc = torne_i2c_write(d->br, TUNER_ADDRESS, w, 3);
	if (rc) {
		LOG(d, TORNE_LOG_ERR, "write DiB0090 register %#02x: %d", reg, rc);
		record(d, rc);
	}
}

static void tun_write_regs(struct torne_dib *d, u8 reg, const u16 *values, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		tun_write_reg(d, reg++, values[i]);
}

/* ---- DiB8000: output, power, ADC, PLL, GPIO ---- */

static u16 fft_to_mode(struct torne_dib *d)
{
	switch (d->c.transmission_mode) {
	case TRANSMISSION_2K:
		return 1;
	case TRANSMISSION_4K:
		return 2;
	default:
		return 3;
	}
}

static void set_acquisition_mode(struct torne_dib *d)
{
	u16 nud = dib_read_word(d, 298);

	nud |= 1 << 3 | 1 << 0;
	dib_write_word(d, 298, nud);
}

static int dib_set_output_mode(struct torne_dib *d, int mode)
{
	u16 outreg = 0, fifo_threshold = 1792, sram = 0x0205, smo_mode;

	d->output_mode = mode;
	smo_mode = (dib_read_word(d, 299) & 0x0050) | 1 << 1;

	switch (mode) {
	case OUTMODE_PARALLEL_GATED:
		outreg = 1 << 10;
		break;
	case OUTMODE_PARALLEL_CONT:
		outreg = 1 << 10 | 1 << 6;
		break;
	case OUTMODE_SERIAL:
		outreg = 1 << 10 | 2 << 6 | 0 << 1;
		break;
	case OUTMODE_DIVERSITY:
		if (DIB8000_CFG_HOSTBUS_DIVERSITY) {
			outreg = 1 << 10 | 4 << 6;
			sram &= 0xfdff;
		} else {
			sram |= 0x0c00;
		}
		break;
	case OUTMODE_FIFO:
		smo_mode |= 3 << 1;
		fifo_threshold = 512;
		outreg = 1 << 10 | 5 << 6;
		break;
	case OUTMODE_HIGH_Z:
		outreg = 0;
		break;
	case OUTMODE_ANALOG_ADC:
		outreg = 1 << 10 | 3 << 6;
		set_acquisition_mode(d);
		break;
	default:
		LOG(d, TORNE_LOG_ERR, "DiB8000 output mode %d not handled", mode);
		return -EINVAL;
	}

	if (DIB8000_CFG_OUTPUT_MPEG2_IN_188_BYTES)
		smo_mode |= 1 << 5;

	dib_write_word(d, 299, smo_mode);
	dib_write_word(d, 300, fifo_threshold); /* synchronous fread */
	dib_write_word(d, 1286, outreg);
	dib_write_word(d, 1291, sram);
	return d->err;
}

static void set_diversity_in(struct torne_dib *d, int onoff)
{
	u16 sync_wait = dib_read_word(d, 273) & 0xfff0;

	if (d->differential_constellation == 0) {
		dib_write_word(d, 272, 1 << 9);		   /* dvsy_off_lmod4 = 1 */
		dib_write_word(d, 273, sync_wait | 1 << 2 | 2); /* sync_enable = 1; comb_mode = 2 */
	} else {
		dib_write_word(d, 272, 0);
		dib_write_word(d, 273, sync_wait);
	}
	d->diversity_onoff = onoff;

	switch (onoff) {
	case 0: /* only use the internal way - not the diversity input */
		dib_write_word(d, 270, 1);
		dib_write_word(d, 271, 0);
		break;
	case 1: /* both ways */
		dib_write_word(d, 270, 6);
		dib_write_word(d, 271, 6);
		break;
	case 2: /* only the diversity input */
		dib_write_word(d, 270, 0);
		dib_write_word(d, 271, 1);
		break;
	}

	if (d->revision == 0x8002) {
		u16 tmp = dib_read_word(d, 903);

		dib_write_word(d, 903, tmp & ~(1 << 3));
		msleep_(d, 30);
		dib_write_word(d, 903, tmp | 1 << 3);
	}
}

static void set_power_mode(struct torne_dib *d, int mode)
{
	u16 reg774 = 0x3fff, reg775 = 0xffff, reg776 = 0xffff;
	u16 reg900 = (dib_read_word(d, 900) & 0xfffc) | 0x3;
	u16 reg1280 = (dib_read_word(d, 1280) & 0x00ff) | 0xff00;

	switch (mode) {
	case POWER_ALL:
		reg774 = 0x0000;
		reg775 = 0x0000;
		reg776 = 0x0000;
		reg900 &= 0xfffc;
		reg1280 &= 0x00ff;
		break;
	case POWER_INTERFACE_ONLY:
		reg1280 &= 0x00ff;
		break;
	}

	dib_write_word(d, 774, reg774);
	dib_write_word(d, 775, reg775);
	dib_write_word(d, 776, reg776);
	dib_write_word(d, 900, reg900);
	dib_write_word(d, 1280, reg1280);
}

static void set_adc_state(struct torne_dib *d, int no)
{
	u16 reg907 = dib_read_word(d, 907);
	u16 reg908 = dib_read_word(d, 908);

	switch (no) {
	case ADC_SLOW_ON:
		reg908 |= 1 << 1 | 1 << 0;
		dib_write_word(d, 908, reg908);
		reg908 &= ~(1 << 1);
		break;
	case ADC_SLOW_OFF:
		reg908 |= 1 << 1 | 1 << 0;
		break;
	case ADC_ON:
		reg907 &= 0x0fff;
		reg908 &= 0x0003;
		break;
	case ADC_OFF: /* leave the VBG voltage on */
		reg907 = 1 << 13 | 1 << 12;
		reg908 = 1 << 6 | 1 << 5 | 1 << 4 | 1 << 3 | 1 << 1;
		break;
	case ADC_VBG_ENABLE:
		reg907 &= ~(1 << 15);
		break;
	case ADC_VBG_DISABLE:
		reg907 |= 1 << 15;
		break;
	}

	dib_write_word(d, 907, reg907);
	dib_write_word(d, 908, reg908);
}

/* the power the ADC sees, raw or, with mode set, in 1/256 dB */
static s32 dib_get_adc_power(struct torne_dib *d, u8 mode)
{
	s32 val = (s32)dib_read32(d, 384);

	if (mode) {
		u32 tmp = (u32)val, exp = 0, mant;
		int ix;

		for (tmp >>= 1; tmp != 0; tmp >>= 1)
			exp++;
		mant = (u32)div_s64_((s64)val * 1000, (s64)((u64)1 << exp));
		/* index of the LUT; Linux reads past the table for a power of 0, the index is held to it here */
		ix = (u8)((mant - 1000) / 100);
		if (ix >= (int)(sizeof(lut_1000ln_mant) / sizeof(lut_1000ln_mant[0])))
			ix = sizeof(lut_1000ln_mant) / sizeof(lut_1000ln_mant[0]) - 1;
		val = lut_1000ln_mant[ix] + 693 * ((s32)exp - 20) - 6908;
		val = val * 256 / 1000;
	}
	return val;
}

static int set_agc_config(struct torne_dib *d, u8 band);
static void tun_dcc_freq(struct torne_dib *d, u8 fast);
static void tun_set_dc_servo(struct torne_dib *d, u8 dc_servo_cutoff);

static void pwm_agc_reset(struct torne_dib *d)
{
	set_adc_state(d, ADC_ON);
	record(d, set_agc_config(d, band_of_frequency(d->c.frequency / 1000)));
}

static void set_bandwidth(struct torne_dib *d, u32 bandwidth)
{
	u32 timf = d->timf;

	if (d->timf == 0)
		timf = d->timf_default;
	dib_write_word(d, 29, (timf >> 16) & 0xffff);
	dib_write_word(d, 30, timf & 0xffff);
}

static void sad_calibrate(struct torne_dib *d)
{
	/* internal */
	dib_write_word(d, 923, 0 << 1 | 0 << 0);
	dib_write_word(d, 924, 776);

	/* do the calibration */
	dib_write_word(d, 923, 1 << 0);
	dib_write_word(d, 923, 0 << 0);

	msleep_(d, 1);
}

static void set_wbd_ref(struct torne_dib *d, u16 value)
{
	if (value > 4095)
		value = 4095;
	d->wbd_ref = value;
	dib_write_word(d, 106, value);
}

static void reset_pll_common(struct torne_dib *d, const struct bandwidth_config *bw_)
{
	dib_write_word(d, 23, (u16)(((bw_->internal * 1000) >> 16) & 0xffff));
	dib_write_word(d, 24, (u16)((bw_->internal * 1000) & 0xffff));
	dib_write_word(d, 27, (u16)((bw_->ifreq >> 16) & 0x01ff));
	dib_write_word(d, 28, (u16)(bw_->ifreq & 0xffff));
	dib_write_word(d, 26, (u16)((bw_->ifreq >> 25) & 0x0003));
	dib_write_word(d, 922, bw_->sad_cfg);
}

static void reset_pll(struct torne_dib *d)
{
	const struct bandwidth_config *pll = &dib8090_pll_config_12mhz;
	u16 clk_cfg1;

	dib_write_word(d, 901, (u16)pll->pll_prediv << 8 | (u16)pll->pll_ratio << 0);

	clk_cfg1 = (u16)(1 << 10 | 0 << 9 | (u16)pll->io_clk_en_core << 8 | (u16)pll->bypclk_div << 5 |
			 (u16)pll->enable_refdiv << 4 | 1 << 3 | (u16)pll->pll_range << 1 | (u16)pll->pll_reset << 0);

	dib_write_word(d, 902, clk_cfg1);
	clk_cfg1 = (u16)((clk_cfg1 & 0xfff7) | (u16)pll->pll_bypass << 3);
	dib_write_word(d, 902, clk_cfg1);

	/* smpl_cfg: P_refclksel=2, P_ensmplsel=1 nodivsmpl=1 */
	if (pll->adc_clk_src == 0)
		dib_write_word(d, 904, (u16)(0 << 15 | 0 << 12 | 0 << 10 | (u16)pll->modulo << 8 | (u16)pll->adc_clk_src << 7 | 0 << 1));
	else if (DIB8000_CFG_REFCLKSEL != 0)
		dib_write_word(d, 904, (u16)(0 << 15 | 1 << 12 | (u16)(DIB8000_CFG_REFCLKSEL & 0x3) << 10 |
					     (u16)pll->modulo << 8 | (u16)pll->adc_clk_src << 7 | 0 << 1));
	else
		dib_write_word(d, 904, (u16)(0 << 15 | 1 << 12 | 3 << 10 | (u16)pll->modulo << 8 | (u16)pll->adc_clk_src << 7 | 0 << 1));

	reset_pll_common(d, pll);
}

/* dib8000_update_pll for the DiB8000: only the ratio update is reachable */
static void update_pll(struct torne_dib *d, const struct bandwidth_config *pll, u32 bandwidth, u8 ratio)
{
	u16 reg1856 = dib_read_word(d, 1856);
	u8 prediv = reg1856 & 0x3f;
	u8 loopdiv = (reg1856 >> 6) & 0x3f;

	if (!pll || (pll->pll_prediv == prediv && pll->pll_ratio == loopdiv))
		return;

	if (ratio != 0)
		dib_write_word(d, 901, (u16)dib8090_pll_config_12mhz.pll_prediv << 8 | (u16)ratio << 0);
}

static void reset_gpio(struct torne_dib *d)
{
	dib_write_word(d, 1029, DIB8000_CFG_GPIO_DIR);
	dib_write_word(d, 1030, DIB8000_CFG_GPIO_VAL);
	dib_write_word(d, 1032, DIB8000_CFG_GPIO_PWM_POS);
	dib_write_word(d, 1037, DIB8000_CFG_PWM_FREQ_DIV);
}

/* direction 0 is output */
static void dib_set_gpio(struct torne_dib *d, u8 num, u8 dir, u8 value)
{
	d->cfg_gpio_dir = dib_read_word(d, 1029);
	d->cfg_gpio_dir &= ~(1 << num);
	d->cfg_gpio_dir |= (u16)(dir & 0x1) << num;
	dib_write_word(d, 1029, d->cfg_gpio_dir);

	d->cfg_gpio_val = dib_read_word(d, 1030);
	d->cfg_gpio_val &= ~(1 << num);
	d->cfg_gpio_val |= (u16)(value & 0x01) << num;
	dib_write_word(d, 1030, d->cfg_gpio_val);
}

/* the vendor register is read twice because the first read after power-up sometimes glitches */
static int dib_identify(struct torne_dib *d, u16 *revision)
{
	u16 value;

	dib_read_word(d, 896);
	value = dib_read_word(d, 896);
	if (value != 0x01b3) {
		if (d->err)
			return d->err;
		LOG(d, TORNE_LOG_ERR, "DiB8000: wrong vendor ID %#04x", value);
		return -ENODEV;
	}

	value = dib_read_word(d, 897);
	if (d->err)
		return d->err;
	switch (value) {
	case 0x8000:
	case 0x8001:
	case 0x8002:
	case 0x8090:
		*revision = value;
		return 0;
	}
	LOG(d, TORNE_LOG_ERR, "DiB8000: wrong device ID %#04x", value);
	return -ENODEV;
}

static u32 read_unc_blocks(struct torne_dib *d)
{
	return dib_read_word(d, 565);
}

static int dib_reset(struct torne_dib *d)
{
	u16 revision;
	const u16 *defaults;
	int rc;

	rc = dib_identify(d, &revision);
	if (rc)
		return rc;
	d->revision = revision;
	if (d->revision == 0x8090) {
		LOG(d, TORNE_LOG_ERR, "DiB8096P (device ID 0x8090) is not supported by this driver");
		return -ENODEV;
	}

	/* sram lead in, rdy */
	dib_write_word(d, 1287, 0x0003);

	master_reset(d);

	set_power_mode(d, POWER_ALL);

	/* always leave the VBG voltage on: it consumes almost nothing but takes a long time to start */
	set_adc_state(d, ADC_OFF);

	/* restart all parts */
	dib_write_word(d, 770, 0xffff);
	dib_write_word(d, 771, 0xffff);
	dib_write_word(d, 772, 0xfffc);
	dib_write_word(d, 898, 0x000c); /* restart sad */
	dib_write_word(d, 1280, 0x004d);
	dib_write_word(d, 1281, 0x000c);

	dib_write_word(d, 770, 0x0000);
	dib_write_word(d, 771, 0x0000);
	dib_write_word(d, 772, 0x0000);
	dib_write_word(d, 898, 0x0004); /* sad */
	dib_write_word(d, 1280, 0x0000);
	dib_write_word(d, 1281, 0x0000);

	/* drives */
	if (DIB8000_CFG_DRIVES != 0)
		dib_write_word(d, 906, DIB8000_CFG_DRIVES);
	else
		dib_write_word(d, 906, 0x2d98); /* min drive SDRAM - not optimal - adjust */

	reset_pll(d);
	dib_write_word(d, 898, 0x0004);

	reset_gpio(d);

	rc = dib_set_output_mode(d, OUTMODE_HIGH_Z);
	if (rc)
		return rc;

	d->current_agc = NULL;

	/* P_iqc_alpha_pha, P_iqc_alpha_amp, P_iqc_dcc_alpha, ... */
	if (dib8090_pll_config_12mhz.ifreq == 0)
		dib_write_word(d, 40, 0x0755); /* P_iqc_corr_inh = 0 enable IQcorr block */
	else
		dib_write_word(d, 40, 0x1f55); /* P_iqc_corr_inh = 1 disable IQcorr block */

	/* the defaults are runs of consecutive registers: a count, the first register, then the values */
	defaults = dib8000_defaults;
	for (;;) {
		u16 count = defaults[0], reg;

		if (count == 0)
			break;
		reg = defaults[1];
		defaults += 2;
		for (; count > 0; count--) {
			dib_write_word(d, reg, defaults[0]);
			defaults++;
			reg++;
		}
	}

	d->isdbt_cfg_loaded = 0;

	/* div_cfg override for special configs. Linux tests the revision against decimal 8090 here, which is always true */
	if (d->revision != 8090 && DIB8000_CFG_DIV_CFG != 0)
		dib_write_word(d, 903, DIB8000_CFG_DIV_CFG);

	/* unforce divstr regardless whether i2c enumeration was done or not */
	dib_modify_word(d, 1285, (u16)~(1 << 1), 0);

	set_bandwidth(d, 6000);

	set_adc_state(d, ADC_SLOW_ON);
	sad_calibrate(d);
	set_adc_state(d, ADC_SLOW_OFF);

	/* ber_rs_len = 3 */
	dib_modify_word(d, 285, (u16)~0x60, 3 << 5);

	set_power_mode(d, POWER_INTERFACE_ONLY);

	read_unc_blocks(d); /* dib8000_reset_stats */

	return d->err;
}

static void restart_agc(struct torne_dib *d)
{
	dib_write_word(d, 770, 0x0a00);
	dib_write_word(d, 770, 0x0000);
}

static int set_agc_config(struct torne_dib *d, u8 band)
{
	const struct agc_config *agc = NULL;
	unsigned int i;

	if (d->current_band == band && d->current_agc)
		return 0;
	d->current_band = band;

	for (i = 0; i < sizeof(dib8090_agc_config) / sizeof(dib8090_agc_config[0]); i++) {
		if (dib8090_agc_config[i].band_caps & band) {
			agc = &dib8090_agc_config[i];
			break;
		}
	}
	if (!agc) {
		LOG(d, TORNE_LOG_ERR, "DiB8000: no valid AGC configuration for band %#02x", band);
		return -EINVAL;
	}

	d->current_agc = agc;

	/* AGC */
	dib_write_word(d, 76, agc->setup);
	dib_write_word(d, 77, agc->inv_gain);
	dib_write_word(d, 78, agc->time_stabiliz);
	dib_write_word(d, 101, (u16)agc->alpha_level << 12 | agc->thlock);

	/* Demod AGC loop configuration */
	dib_write_word(d, 102, (u16)agc->alpha_mant << 5 | agc->alpha_exp);
	dib_write_word(d, 103, (u16)agc->beta_mant << 6 | agc->beta_exp);

	/* AGC continued */
	if (d->wbd_ref != 0)
		dib_write_word(d, 106, d->wbd_ref);
	else /* use default */
		dib_write_word(d, 106, agc->wbd_ref);

	dib_write_word(d, 107, (u16)agc->wbd_alpha << 9 | (u16)agc->perform_agc_softsplit << 8);
	dib_write_word(d, 108, agc->agc1_max);
	dib_write_word(d, 109, agc->agc1_min);
	dib_write_word(d, 110, agc->agc2_max);
	dib_write_word(d, 111, agc->agc2_min);
	dib_write_word(d, 112, (u16)agc->agc1_pt1 << 8 | agc->agc1_pt2);
	dib_write_word(d, 113, (u16)agc->agc1_slope1 << 8 | agc->agc1_slope2);
	dib_write_word(d, 114, (u16)agc->agc2_pt1 << 8 | agc->agc2_pt2);
	dib_write_word(d, 115, (u16)agc->agc2_slope1 << 8 | agc->agc2_slope2);

	dib_write_word(d, 75, agc->agc1_pt3);
	dib_modify_word(d, 923, 0xffe3, (u16)agc->wbd_inv << 4 | (u16)agc->wbd_sel << 2);

	return 0;
}

static int agc_soft_split(struct torne_dib *d)
{
	const struct agc_config *agc = d->current_agc;
	u16 global, split_offset;

	if (!agc || agc->perform_agc_softsplit == 0 || agc->split.max == 0)
		return 0;

	/* n_agc_global */
	global = dib_read_word(d, 390);

	if (global > agc->split.min_thres)
		split_offset = agc->split.min;
	else if (global < agc->split.max_thres)
		split_offset = agc->split.max;
	else
		split_offset = (u16)((int)agc->split.max * ((int)global - (int)agc->split.min_thres) /
				     ((int)agc->split.max_thres - (int)agc->split.min_thres));

	/* P_agc_force_split and P_agc_split_offset */
	dib_modify_word(d, 107, 0xff00, split_offset);
	return 5000;
}

/* returns the time to wait before the next step, in 0.1 ms units */
static int agc_startup(struct torne_dib *d)
{
	int ret = 0;

	switch (d->tune_state) {
	case AGC_START:
		/* set power-up level: interf+analog+AGC */
		set_adc_state(d, ADC_ON);

		if (set_agc_config(d, band_of_frequency(d->c.frequency / 1000)) != 0) {
			d->tune_state = AGC_STOP;
			d->status = STATUS_TUNE_FAILED;
			break;
		}

		ret = 70;
		d->tune_state = AGC_STEP0;
		break;

	case AGC_STEP0: /* AGC initialization: dib8090_agc_control(1) */
		tun_dcc_freq(d, 1);
		restart_agc(d);
		/* wait AGC rough lock time */
		ret = 50;
		d->tune_state = AGC_STEP1;
		break;

	case AGC_STEP1:
		/* wait AGC accurate lock time; dib8000_update_lna would shorten the wait when the board has an LNA */
		ret = 70;
		d->tune_state = AGC_STEP2;
		break;

	case AGC_STEP2:
		agc_soft_split(d);

		/* dib8090_agc_control(0): tuner.dccFreq(0), tuner.setDCServo(1) */
		tun_dcc_freq(d, 0);
		tun_set_dc_servo(d, 1);

		d->tune_state = AGC_STOP;
		break;

	default:
		ret = agc_soft_split(d);
		break;
	}
	return ret;
}

static void update_timf(struct torne_dib *d)
{
	u32 timf;

	d->timf = dib_read32(d, 435);
	timf = d->timf;

	dib_write_word(d, 29, (u16)(timf >> 16));
	dib_write_word(d, 30, (u16)(timf & 0xffff));
}

static u32 ctrl_timf(struct torne_dib *d, u8 op, u32 timf)
{
	switch (op) {
	case TIMF_SET:
		d->timf = timf;
		break;
	case TIMF_UPDATE:
		update_timf(d);
		break;
	case TIMF_GET:
		break;
	}
	set_bandwidth(d, 6000);
	return d->timf;
}

static void dib_wakeup(struct torne_dib *d)
{
	set_power_mode(d, POWER_ALL);
	set_adc_state(d, ADC_ON);
	set_adc_state(d, ADC_SLOW_ON);
}

static int dib_sleep_chip(struct torne_dib *d)
{
	int rc = dib_set_output_mode(d, OUTMODE_HIGH_Z);

	if (rc)
		return rc;
	set_power_mode(d, POWER_INTERFACE_ONLY);
	set_adc_state(d, ADC_SLOW_OFF);
	set_adc_state(d, ADC_OFF);
	return d->err;
}

static int dib_initialize(struct torne_dib *d)
{
	int rc;
	u16 revision;

	d->timf_default = dib8090_pll_config_12mhz.timf;

	rc = dib_identify(d, &revision);
	if (rc)
		return rc;

	master_initialize(d);

	rc = dib_reset(d);
	if (rc)
		return rc;

	/* ber_rs_len = 3 */
	dib_modify_word(d, 285, (u16)~0x60, 3 << 5);
	d->current_demod_bw = 6000;

	return d->err;
}

/* ================= ISDB-T channel setup, autosearch and the tune state machine (dib8000_tune.go) ================= */

static int set_layer(struct torne_dib *d, u8 layer_index, int max_constellation)
{
	struct dib_layer *layer = &d->c.layer[layer_index];
	u16 constellation, cr, time_intlv;

	switch (layer->modulation) {
	case MODULATION_DQPSK:
		constellation = 0;
		break;
	case MODULATION_QPSK:
		constellation = 1;
		break;
	case MODULATION_QAM16:
		constellation = 2;
		break;
	default:
		constellation = 3;
		break;
	}

	switch (layer->fec) {
	case FEC_1_2:
		cr = 1;
		break;
	case FEC_2_3:
		cr = 2;
		break;
	case FEC_3_4:
		cr = 3;
		break;
	case FEC_5_6:
		cr = 5;
		break;
	default:
		cr = 7;
		break;
	}

	time_intlv = fls8(layer->interleaving); /* fls() */
	if (time_intlv > 3 && !(time_intlv == 4 && d->c.isdbt_sb_mode == 1))
		time_intlv = 0;

	dib_write_word(d, 2 + layer_index,
		       (u16)(constellation << 10 | ((u16)layer->segment_count & 0xf) << 6 | cr << 3 | time_intlv));
	if (layer->segment_count > 0) {
		switch (max_constellation) {
		case MODULATION_DQPSK:
		case MODULATION_QPSK:
			if (layer->modulation == MODULATION_QAM16 || layer->modulation == MODULATION_QAM64)
				max_constellation = layer->modulation;
			break;
		case MODULATION_QAM16:
			if (layer->modulation == MODULATION_QAM64)
				max_constellation = layer->modulation;
			break;
		}
	}

	return max_constellation;
}

/* channel estimation fine configuration */
static u16 adp_fine_tune(struct torne_dib *d, int max_constellation)
{
	u16 ana_gain;
	const u16 *adp;
	u16 i;

	switch (max_constellation) {
	case MODULATION_QAM64:
		ana_gain = 0x7;
		adp = adp_q64;
		break;
	case MODULATION_QAM16:
		ana_gain = 0x7;
		adp = adp_q16;
		break;
	default:
		ana_gain = 0;
		adp = adp_q_default;
		break;
	}

	for (i = 0; i < 4; i++)
		dib_write_word(d, 215 + i, adp[i]);

	return ana_gain;
}

static void update_ana_gain(struct torne_dib *d, u16 ana_gain)
{
	u16 i;

	dib_write_word(d, 116, ana_gain);

	/* update ADC target depending on ana_gain */
	if (ana_gain) { /* set -16dB ADC target for ana_gain=-1 */
		for (i = 0; i < 10; i++)
			dib_write_word(d, 80 + i, adc_target_16db[i]);
	} else { /* set -22dB ADC target for ana_gain=0 */
		for (i = 0; i < 10; i++)
			dib_write_word(d, 80 + i, (u16)(adc_target_16db[i] - 355));
	}
}

static void load_ana_fe_coefs(struct torne_dib *d, const s16 *ana_fe)
{
	u16 mode;

	if (d->isdbt_cfg_loaded == 0)
		for (mode = 0; mode < 24; mode++)
			dib_write_word(d, 117 + mode, (u16)ana_fe[mode]);
}

static void set_13seg_channel(struct torne_dib *d)
{
	u16 coff_pow = 0x2800, i;

	d->seg_mask = 0x1fff; /* all 13 segments enabled */

	/* ---- COFF ---- Carloff, the most robust --- */
	if (d->isdbt_cfg_loaded == 0) { /* if not Sound Broadcasting mode: put default values for 13 segments */
		dib_write_word(d, 180, 16 << 6 | 9);
		dib_write_word(d, 187, 4 << 12 | 8 << 5 | 0x2);
		coff_pow = 0x2800;
		for (i = 0; i < 6; i++)
			dib_write_word(d, 181 + i, coff_pow);

		/* P_ctrl_corm_thres4pre_freq_inh=1, P_ctrl_pre_freq_mode_sat=1 */
		dib_write_word(d, 338, 1 << 12 | 1 << 10 | 0 << 9 | 3 << 5 | 1);

		/* P_ctrl_pre_freq_win_len=8, P_ctrl_pre_freq_thres_lockin=6 */
		dib_write_word(d, 340, 8 << 6 | 6 << 0);
		/* P_ctrl_pre_freq_thres_lockout=4, P_small_use_tmcc/ac/cp=1 */
		dib_write_word(d, 341, 4 << 3 | 1 << 2 | 1 << 1 | 1 << 0);

		dib_write_word(d, 228, 0);	/* default value */
		dib_write_word(d, 265, 31);	/* default value */
		dib_write_word(d, 205, 0x200f); /* init value */
	}

	if (dib8090_pll_config_12mhz.ifreq == 0)
		dib_write_word(d, 266, (u16)(~d->seg_mask | d->seg_diff_mask | 0x40)); /* P_equal_noise_seg_inh */

	load_ana_fe_coefs(d, ana_fe_coeff_13seg);
}

static void set_subchannel_prbs(struct torne_dib *d, u16 init_prbs)
{
	u16 reg1 = dib_read_word(d, 1);

	dib_write_word(d, 1, init_prbs << 2 | (reg1 & 0x3)); /* ADDR 1 */
}

static void small_fine_tune(struct torne_dib *d)
{
	dib_write_word(d, 352, d->seg_diff_mask);
	dib_write_word(d, 353, d->seg_mask);

	/* P_small_coef_ext_enable=ISDB-Tsb, P_small_narrow_band=ISDB-Tsb, P_small_last_seg=13, P_small_offset_num_car=5 */
	dib_write_word(d, 351, (u16)d->c.isdbt_sb_mode << 9 | (u16)d->c.isdbt_sb_mode << 8 | 13 << 4 | 5);
}

static void set_isdbt_common_channel(struct torne_dib *d, u8 seq, bool autosearching)
{
	u16 p_cfr_left_edge = 0, p_cfr_right_edge = 0;
	u16 tmcc_pow = 0, nbseg_diff = 0;
	int max_constellation = MODULATION_DQPSK;
	struct dib_channel *c = &d->c;
	u16 tmp, i, ana_gain, init_prbs;

	if (autosearching)
		c->isdbt_partial_reception = 1;

	/* P_mode */
	dib_write_word(d, 10, (u16)seq << 4);

	/* init mode */
	d->mode = fft_to_mode(d);

	/* set guard */
	tmp = dib_read_word(d, 1);
	dib_write_word(d, 1, (tmp & 0xfffc) | ((u16)c->guard_interval & 0x3));

	dib_modify_word(d, 274, 0xffcf, (u16)(c->isdbt_partial_reception & 1) << 5 | (u16)(c->isdbt_sb_mode & 1) << 4);

	/* signal optimization parameter */
	if (c->isdbt_partial_reception) {
		d->seg_diff_mask = (u16)(bw(c->layer[0].modulation == MODULATION_DQPSK) << permu_seg[0]);
		for (i = 1; i < 3; i++)
			nbseg_diff += bw(c->layer[i].modulation == MODULATION_DQPSK) * c->layer[i].segment_count;
		/* segment counts are four-bit fields, so a bad TMCC reading can ask for more than the table holds */
		for (i = 0; i < nbseg_diff && (int)i + 1 < 13; i++)
			d->seg_diff_mask |= 1 << permu_seg[i + 1];
	} else {
		/* Linux accumulates into the mask left by the previous call here */
		for (i = 0; i < 3; i++)
			nbseg_diff += bw(c->layer[i].modulation == MODULATION_DQPSK) * c->layer[i].segment_count;
		for (i = 0; i < nbseg_diff && (int)i < 13; i++)
			d->seg_diff_mask |= 1 << permu_seg[i];
	}

	if (d->seg_diff_mask)
		dib_modify_word(d, 268, 0xf9ff, 0x0200);
	else
		dib_write_word(d, 268, 2 << 9 | 39); /* init value */

	for (i = 0; i < 3; i++)
		max_constellation = set_layer(d, (u8)i, max_constellation);
	if (!autosearching) {
		d->layer_b_nb_seg = c->layer[1].segment_count;
		d->layer_c_nb_seg = c->layer[2].segment_count;
	}

	/* WRITE: Mode & Diff mask */
	dib_write_word(d, 0, (u16)(d->mode << 13 | d->seg_diff_mask));

	d->differential_constellation = d->seg_diff_mask != 0;

	/* channel estimation fine configuration */
	ana_gain = adp_fine_tune(d, max_constellation);

	/* update ana_gain depending on max constellation */
	update_ana_gain(d, ana_gain);

	/* ---- ANA_FE ---- */
	if (c->isdbt_partial_reception) /* 3-segments */
		load_ana_fe_coefs(d, ana_fe_coeff_3seg);
	else
		load_ana_fe_coefs(d, ana_fe_coeff_1seg); /* 1-segment */

	/* TSB or ISDBT? Only ISDB-T is ported. */
	set_13seg_channel(d);
	init_prbs = 0xfff;

	/* SMALL */
	small_fine_tune(d);

	set_subchannel_prbs(d, init_prbs);

	/* ---- CHAN_BLK ---- */
	for (i = 0; i < 13; i++) {
		if ((~d->seg_diff_mask >> i) & 1) {
			u16 enabled = d->seg_mask & ~d->seg_diff_mask;

			if (i == 0 || ((enabled >> (i - 1)) & 1) == 0)
				p_cfr_left_edge += 1 << i;
			if (i == 12 || ((enabled >> (i + 1)) & 1) == 0)
				p_cfr_right_edge += 1 << i;
		}
	}
	dib_write_word(d, 222, p_cfr_left_edge);  /* p_cfr_left_edge */
	dib_write_word(d, 223, p_cfr_right_edge); /* p_cfr_right_edge */
	/* "P_cspu_left_edge" & "P_cspu_right_edge" not used => do not care */

	dib_write_word(d, 189, (u16)(~d->seg_mask | d->seg_diff_mask)); /* P_lmod4_seg_inh */
	dib_write_word(d, 192, (u16)(~d->seg_mask | d->seg_diff_mask)); /* P_pha3_seg_inh */
	dib_write_word(d, 225, (u16)(~d->seg_mask | d->seg_diff_mask)); /* P_tac_seg_inh */

	if (!autosearching)
		dib_write_word(d, 288, (u16)((~d->seg_mask | d->seg_diff_mask) & 0x1fff)); /* P_tmcc_seg_eq_inh */
	else
		/* disable equalisation of the TMCC during autosearch so the DQPSK channels can be found */
		dib_write_word(d, 288, 0x1fff);

	dib_write_word(d, 211, d->seg_mask & ~d->seg_diff_mask); /* P_des_seg_enabled */
	dib_write_word(d, 287, (u16)(~d->seg_mask | 0x1000));	 /* P_tmcc_seg_inh */

	dib_write_word(d, 178, 32); /* P_fft_powrange = 32 */

	/* ---- TMCC ---- */
	for (i = 0; i < 3; i++)
		tmcc_pow += (bw(c->layer[i].modulation == MODULATION_DQPSK) * 4 + 1) * c->layer[i].segment_count;

	/* quantif of "P_tmcc_dec_thres_?k" is (0, 5+mode, 9); threshold is set at 1/4 of max power */
	tmcc_pow *= 1 << (9 - 2);
	dib_write_word(d, 290, tmcc_pow); /* P_tmcc_dec_thres_2k */
	dib_write_word(d, 291, tmcc_pow); /* P_tmcc_dec_thres_4k */
	dib_write_word(d, 292, tmcc_pow); /* P_tmcc_dec_thres_8k */

	/* ---- PHA3 ---- */
	if (d->isdbt_cfg_loaded == 0)
		dib_write_word(d, 250, 3285); /* p_2d_hspeed_thr0 */

	d->isdbt_cfg_loaded = 0;
}

/* programs P_search_end0..2 in units of the internal clock and returns the last value written */
static u32 wait_lock(struct torne_dib *d, u32 internal, u32 wait0ms, u32 wait1ms, u32 wait2ms)
{
	u32 value = 0; /* P_search_end0 wait time */
	u16 reg;

	for (reg = 11; reg < 16; reg += 2) {
		switch (reg) {
		case 11:
			value = internal * wait0ms;
			break;
		case 13:
			value = internal * wait1ms;
			break;
		case 15:
			value = internal * wait2ms;
			break;
		}
		dib_write_word(d, reg, (value >> 16) & 0xffff);
		dib_write_word(d, reg + 1, value & 0xffff);
	}
	return value;
}

/* from revision 0x8002 on, the search runs in two passes, FFT size then guard interval */
static void autosearch_start(struct torne_dib *d)
{
	struct dib_channel *c = &d->c;
	u8 slist = 0;
	u32 internal = dib8090_pll_config_12mhz.internal;

	if (d->revision >= 0x8002 && d->autosearch_state == AUTOSEARCH_SEARCHING_FFT) {
		u32 value;

		dib_write_word(d, 37, 0x0065); /* P_ctrl_pha_off_max default values */
		dib_write_word(d, 116, 0x0000); /* P_ana_gain to 0 */

		dib_modify_word(d, 0, 0x1fff, 0 << 13 | 1 << 15); /* P_mode = 0, P_restart_search=1 */
		dib_modify_word(d, 1, 0xfffc, 0);		   /* P_guard = 0 */
		dib_write_word(d, 6, 0);			   /* P_lock0_mask = 0 */
		dib_write_word(d, 7, 0);			   /* P_lock1_mask = 0 */
		dib_write_word(d, 8, 0);			   /* P_lock2_mask = 0 */
		dib_modify_word(d, 10, 0x200, 16 << 4 | 0 << 0);   /* P_search_list=16, P_search_maxtrial=0 */

		/* time in ms configure P_search_end0 P_search_end1 P_search_end2 */
		value = wait_lock(d, internal, 20, 20, 20);

		dib_write_word(d, 17, 0);
		dib_write_word(d, 18, 200); /* P_search_rstst = 200 */
		dib_write_word(d, 19, 0);
		dib_write_word(d, 20, 400); /* P_search_rstend = 400 */
		dib_write_word(d, 21, (value >> 16) & 0xffff); /* P_search_checkst */
		dib_write_word(d, 22, value & 0xffff);

		dib_modify_word(d, 32, 0xf0ff, 9 << 8); /* P_corm_alpha = 3 */
		dib_write_word(d, 355, 2);		/* P_search_param_max = 2 */

		/* P_search_param_select = (1 | 1<<4 | 1 << 8) */
		dib_write_word(d, 356, 0);
		dib_write_word(d, 357, 0x111);

		dib_modify_word(d, 770, 0xdfff, 1 << 13);    /* P_restart_ccg = 1 */
		dib_modify_word(d, 770, 0xdfff, 0 << 13);    /* P_restart_ccg = 0 */
		dib_modify_word(d, 0, 0x7ff, 0 << 15 | 1 << 13); /* P_restart_search = 0 */
	} else if (d->revision >= 0x8002 && d->autosearch_state == AUTOSEARCH_SEARCHING_GUARD) {
		u16 value;

		c->transmission_mode = TRANSMISSION_8K;
		c->guard_interval = GUARD_1_8;
		c->inversion = 0;
		c->layer[0].modulation = MODULATION_QAM64;
		c->layer[0].fec = FEC_2_3;
		c->layer[0].interleaving = 0;
		c->layer[0].segment_count = 13;

		slist = 16;
		c->transmission_mode = d->found_nfft;

		set_isdbt_common_channel(d, slist, true);

		/* set lock_mask values */
		dib_write_word(d, 6, 0x4);
		dib_write_word(d, 7, 0x8);
		dib_write_word(d, 8, 0x1000);

		/* set lock_mask wait time values */
		wait_lock(d, internal, 50, 200, 1000); /* time in ms configure P_search_end0 P_search_end1 P_search_end2 */

		dib_write_word(d, 355, 3); /* P_search_param_max = 3 */

		/* P_search_param_select = 0xf; look for the 4 different guard intervals */
		dib_write_word(d, 356, 0);
		dib_write_word(d, 357, 0xf);

		value = dib_read_word(d, 0);
		dib_write_word(d, 0, 1 << 15 | value);
		dib_read_word(d, 1284); /* reset the INT. n_irq_pending */
		dib_write_word(d, 0, value);
	} else {
		u16 value;

		c->inversion = 0;
		c->layer[0].modulation = MODULATION_QAM64;
		c->layer[0].fec = FEC_2_3;
		c->layer[0].interleaving = 0;
		c->layer[0].segment_count = 13;

		/* choose the right list */
		if (c->guard_interval == GUARD_AUTO) {
			if (c->transmission_mode == TRANSMISSION_AUTO) {
				c->transmission_mode = TRANSMISSION_8K;
				c->guard_interval = GUARD_1_8;
				slist = 7;
				dib_modify_word(d, 0, 0x9fff, 1 << 13); /* P_mode = 1 to have autosearch start ok with mode2 */
			} else {
				c->guard_interval = GUARD_1_8;
				slist = 3;
			}
		} else {
			if (c->transmission_mode == TRANSMISSION_AUTO) {
				c->transmission_mode = TRANSMISSION_8K;
				slist = 2;
				dib_modify_word(d, 0, 0x9fff, 1 << 13); /* P_mode = 1 */
			} else {
				slist = 0;
			}
		}

		set_isdbt_common_channel(d, slist, true);

		/* set lock_mask values */
		dib_write_word(d, 6, 0x4);
		dib_write_word(d, 7, 0x8);
		dib_write_word(d, 8, 0x1000);

		/* set lock_mask wait time values */
		wait_lock(d, internal, 50, 100, 1000);

		value = dib_read_word(d, 0);
		dib_write_word(d, 0, 1 << 15 | value);
		dib_read_word(d, 1284); /* reset the INT. n_irq_pending */
		dib_write_word(d, 0, value);
	}
}

/* 0 still pending, 1 failed, 2 succeeded, 3 the FFT correlation pass has finished */
static int autosearch_irq(struct torne_dib *d)
{
	u16 irq_pending = dib_read_word(d, 1284);

	if (d->revision >= 0x8002 && d->autosearch_state == AUTOSEARCH_SEARCHING_FFT) {
		if (irq_pending & 0x1)
			return 3;
	} else {
		if (irq_pending & 0x1) /* failed */
			return 1;
		if (irq_pending & 0x2) /* succeeded */
			return 2;
	}
	return 0; /* still pending */
}

/* P_restart_chd, the channel decoder */
static void viterbi_state(struct torne_dib *d, bool on)
{
	u16 tmp = dib_read_word(d, 771);

	if (on)
		dib_write_word(d, 771, tmp & 0xfffd);
	else
		dib_write_word(d, 771, tmp | 1 << 1);
}

static void set_dds(struct torne_dib *d, s32 offset_khz)
{
	u32 abs_offset_khz = offset_khz < 0 ? (u32)(-offset_khz) : (u32)offset_khz;
	u32 dds = dib8090_pll_config_12mhz.ifreq & 0x1ffffff;
	u16 invert = (dib8090_pll_config_12mhz.ifreq & (1 << 25)) ? 1 : 0;
	u32 ratio = 2;
	s32 unit_khz_dds_val = (s32)(67108864 / dib8090_pll_config_12mhz.internal);

	if (offset_khz < 0)
		unit_khz_dds_val *= -1;

	/* IF tuner. The product is unsigned in C, so a negative unit wraps. */
	if (invert)
		dds -= abs_offset_khz * (u32)unit_khz_dds_val;
	else
		dds += abs_offset_khz * (u32)unit_khz_dds_val;

	if (abs_offset_khz <= dib8090_pll_config_12mhz.internal / ratio) {
		/* max dds offset is the half of the demod freq */
		dib_write_word(d, 26, invert);
		dib_write_word(d, 27, (u16)(dds >> 16) & 0x1ff);
		dib_write_word(d, 28, (u16)(dds & 0xffff));
	}
}

static void set_frequency_offset(struct torne_dib *d)
{
	u32 current_rf = 1000 * d->t_current_rf; /* dib0090_get_frequency, in Hz */
	s32 total_dds_offset_khz;

	current_rf /= 1000;
	total_dds_offset_khz = (s32)current_rf - (s32)d->c.frequency / 1000;

	set_dds(d, total_dds_offset_khz);
}

/* LUT_isdbt_symbol_duration */
static const u16 isdbt_symbol_duration[4] = { 26, 101, 63, 0 };

/* in 100 us units; for a 6 MHz channel the division truncates to zero and every mode gives 1 */
static u32 get_symbol_duration(struct torne_dib *d)
{
	int i;

	switch (d->c.transmission_mode) {
	case TRANSMISSION_2K:
		i = 0;
		break;
	case TRANSMISSION_4K:
		i = 2;
		break;
	default:
		i = 1;
		break;
	}

	return (u32)isdbt_symbol_duration[i] / (d->c.bandwidth_hz / 1000) + 1;
}

static void set_isdbt_loop_params(struct torne_dib *d, int loop_step)
{
	u16 reg32 = 0, reg37 = 0;

	switch (loop_step) {
	case LOOP_TUNE1:
		/* 13-seg start conf offset loop parameters */
		reg32 = (u16)((9 - d->mode) << 12 | 6 << 8 | 0x80);	   /* P_timf_alpha = (9-P_mode), P_corm_alpha=6, P_corm_thres=0x80 */
		reg37 = (u16)(3 << 5 | 0 << 4 | (8 - d->mode));	   /* P_ctrl_pha_off_max=3, P_ctrl_sfreq_inh=0, P_ctrl_sfreq_step=9 */
		break;
	case LOOP_TUNE2:
		reg32 = (u16)((11 - d->mode) << 12 | 6 << 8 | 0x80);
		reg37 = (u16)((5 + d->mode) << 5 | (10 - d->mode));
		break;
	}
	dib_write_word(d, 32, reg32);
	dib_write_word(d, 37, reg37);
}

static void demod_restart(struct torne_dib *d)
{
	dib_write_word(d, 770, 0x4000);
	dib_write_word(d, 770, 0x0000);
}

static void set_sync_wait(struct torne_dib *d)
{
	u16 sync_wait;

	switch (d->c.transmission_mode) {
	case TRANSMISSION_8K:
		sync_wait = 256;
		break;
	case TRANSMISSION_4K:
		sync_wait = 128;
		break;
	default:
		sync_wait = 64;
		break;
	}

	/* add 50% SFN margin and compensate for the DVSY FIFO */
	if (DIB8000_CFG_DIVERSITY_DELAY == 0)
		sync_wait = (u16)(((int)sync_wait * (1 << (unsigned)d->c.guard_interval) * 3) / 2 + 48);
	else
		sync_wait = (u16)(((int)sync_wait * (1 << (unsigned)d->c.guard_interval) * 3) / 2 + DIB8000_CFG_DIVERSITY_DELAY);

	dib_modify_word(d, 273, 0x000f, (u16)(sync_wait << 4));
}

/* delay is in 100us units, or in symbols when symbol_dependent; the deadline is rounded up to whole ms */
static u64 get_timeout(struct torne_dib *d, u32 delay, bool symbol_dependent)
{
	if (symbol_dependent)
		delay *= d->symbol_duration;
	return now_ms(d) + ((u64)delay * 100 + 999) / 1000;
}

static u16 read_lock(struct torne_dib *d)
{
	return dib_read_word(d, 568);
}

/* whether every parameter is known, so the autosearch can be skipped; with only a frequency known it never is */
static bool is_manual_mode(struct dib_channel *c)
{
	int n_segs = 0, i;

	/* use auto mode on DVB-T compat mode */
	if (c->delivery_system != SYSTEM_ISDBT)
		return false;
	/* transmission mode and guard interval are only detected in auto mode */
	if (c->transmission_mode == TRANSMISSION_AUTO)
		return false;
	if (c->guard_interval == GUARD_AUTO)
		return false;
	/* if no layer is enabled, assume auto mode, as at least one layer should be enabled */
	if (c->isdbt_layer_enabled == 0)
		return false;
	for (i = 0; i < 3; i++) {
		if (!(c->isdbt_layer_enabled & (1 << i)))
			continue;
		if (c->layer[i].segment_count > 13 || c->layer[i].segment_count == 0) {
			c->isdbt_layer_enabled &= ~(1 << i);
			continue;
		}
		n_segs += c->layer[i].segment_count;
		if (c->layer[i].modulation == MODULATION_QAM_AUTO || c->layer[i].fec == FEC_AUTO)
			return false;
	}
	/* a wrong number of segments falls back to auto mode */
	if (n_segs == 0 || n_segs > 13)
		return false;
	return true;
}

/* one step of the demodulator's state machine; returns the time to wait before the next step in 100us units,
 * or 0 once the machine has stopped */
static int dib_tune(struct torne_dib *d)
{
	struct dib_channel *c = &d->c;
	u16 locks = 0, deeper_interleaver = 0;
	int ret = 1; /* 1 symbol duration (in 100us unit) delay most of the time */
	u64 now = now_ms(d);
	int i;

	switch (d->tune_state) {
	case DEMOD_START: /* 30 */
		read_unc_blocks(d); /* dib8000_reset_stats */

		d->status = STATUS_TUNE_PENDING;
		d->channel_parameters_set = is_manual_mode(c) ? 1 : 0;

		viterbi_state(d, false); /* force chan dec in restart */

		/* layer monitor */
		dib_modify_word(d, 285, 0x60, 0);

		set_frequency_offset(d);
		set_bandwidth(d, c->bandwidth_hz / 1000);

		if (d->channel_parameters_set == 0) { /* the channel struct is unknown, search it! */
			d->autosearch_state = AUTOSEARCH_SEARCHING_FFT;
			d->found_nfft = TRANSMISSION_AUTO;
			d->found_guard = GUARD_AUTO;
			d->tune_state = DEMOD_SEARCH_NEXT;
		} else { /* we already know the channel struct so TUNE only! */
			d->autosearch_state = AUTOSEARCH_DONE;
			d->tune_state = DEMOD_STEP3;
		}
		d->symbol_duration = (u8)get_symbol_duration(d);
		break;

	case DEMOD_SEARCH_NEXT: /* 51 */
		autosearch_start(d);
		ret = 15;
		d->tune_state = DEMOD_STEP1;
		break;

	case DEMOD_STEP1: /* 31 */
		switch (autosearch_irq(d)) {
		case 1: /* fail */
			d->status = STATUS_TUNE_FAILED;
			d->autosearch_state = AUTOSEARCH_DONE;
			d->tune_state = DEMOD_STOP; /* else we are done here */
			break;
		case 2: /* success: signal to the upper layer that a channel was found and the parameters can be read */
			d->status = STATUS_FFT_SUCCESS;
			d->tune_state = DEMOD_STEP3;
			if (d->autosearch_state == AUTOSEARCH_SEARCHING_GUARD)
				d->tune_state = DEMOD_STEP2;
			else
				d->autosearch_state = AUTOSEARCH_DONE;
			break;
		case 3: /* autosearch FFT max correlation ended */
			d->tune_state = DEMOD_STEP2;
			break;
		}
		break;

	case DEMOD_STEP2:
		switch (d->autosearch_state) {
		case AUTOSEARCH_SEARCHING_FFT: {
			/* searching for the correct FFT */
			u32 corm[4];
			int max_value = 0, find_index;

			corm[2] = (u32)dib_read_word(d, 594) << 16 | dib_read_word(d, 595);
			corm[1] = (u32)dib_read_word(d, 596) << 16 | dib_read_word(d, 597);
			corm[0] = (u32)dib_read_word(d, 598) << 16 | dib_read_word(d, 599);

			for (find_index = 1; find_index < 3; find_index++)
				if (corm[max_value] < corm[find_index])
					max_value = find_index;

			switch (max_value) {
			case 0:
				d->found_nfft = TRANSMISSION_2K;
				break;
			case 1:
				d->found_nfft = TRANSMISSION_4K;
				break;
			default:
				d->found_nfft = TRANSMISSION_8K;
				break;
			}

			d->tune_state = DEMOD_SEARCH_NEXT;
			d->autosearch_state = AUTOSEARCH_SEARCHING_GUARD;
			ret = 10;
			break;
		}
		case AUTOSEARCH_SEARCHING_GUARD:
			/* searching for the correct guard interval */
			d->found_guard = dib_read_word(d, 570) & 0x3;
			d->tune_state = DEMOD_STEP3;
			break;
		default:
			/* the demod should never be in this state */
			d->status = STATUS_TUNE_FAILED;
			d->autosearch_state = AUTOSEARCH_DONE;
			d->tune_state = DEMOD_STOP; /* else we are done here */
			break;
		}
		break;

	case DEMOD_STEP3: /* 33 */
		set_isdbt_loop_params(d, LOOP_TUNE1);
		set_isdbt_common_channel(d, 0, false); /* setting the known channel parameters here */
		d->tune_state = DEMOD_STEP4;
		break;

	case DEMOD_STEP4: /* 34 */
		demod_restart(d);

		set_sync_wait(d);
		set_diversity_in(d, d->diversity_onoff);

		locks = (dib_read_word(d, 180) >> 6) & 0x3f; /* P_coff_winlen ? */
		/* coff should lock over P_coff_winlen ofdm symbols: give 3 times this length to lock */
		d->timeout_ms = get_timeout(d, 2 * (u32)locks, true);
		d->tune_state = DEMOD_STEP5;
		break;

	case DEMOD_STEP5: /* 35 */
		locks = read_lock(d);
		if (locks & (0x3 << 11)) { /* coff-lock and off_cpil_lock achieved */
			update_timf(d); /* we achieved a coff_cpil_lock - it's time to update the timf */
			if (d->differential_constellation == 0) {
				/* 2 times lmod4_win_len + 10 symbols (pipe delay after coff + nb to compute a 1st correlation) */
				d->timeout_ms = get_timeout(d, 20 * (u32)((dib_read_word(d, 188) >> 5) & 0x1f), true);
				d->tune_state = DEMOD_STEP7;
			} else {
				d->tune_state = DEMOD_STEP8;
			}
		} else if (now > d->timeout_ms) {
			d->tune_state = DEMOD_STEP6; /* goto check for diversity input connection */
		}
		break;

	case DEMOD_STEP6: /* 36: if there is an input (diversity); there is no diversity frontend on this board */
		viterbi_state(d, true); /* start viterbi chandec */
		set_isdbt_loop_params(d, LOOP_TUNE2);
		d->tune_state = DEMOD_STOP; /* else we are done here; step 8 will close the loops and exit */
		d->status = STATUS_TUNE_FAILED;
		break;

	case DEMOD_STEP7: /* 37 */
		locks = read_lock(d);
		if (locks & (1 << 10)) { /* lmod4_lock */
			ret = 14; /* wait for 14 symbols */
			d->tune_state = DEMOD_STEP8;
		} else if (now > d->timeout_ms) {
			d->tune_state = DEMOD_STEP6; /* goto check for diversity input connection */
		}
		break;

	case DEMOD_STEP8: /* 38 */
		viterbi_state(d, true); /* start viterbi chandec */
		set_isdbt_loop_params(d, LOOP_TUNE2);

		d->tune_state = DEMOD_STEP9;
		d->status = STATUS_LOCKED;
		break;

	case DEMOD_STEP9: /* 39 */
		if ((dib_read_word(d, 1291) >> 9) & 0x1) { /* fe capable of deinterleaving: esram */
			/* defines timeout for mpeg lock depending on interleaver length of longest layer */
			for (i = 0; i < 3; i++) {
				if ((u16)c->layer[i].interleaving >= deeper_interleaver) {
					if (c->layer[i].segment_count > 0) { /* valid layer */
						/* Linux takes layer A's interleaving here whichever layer matched; kept */
						deeper_interleaver = c->layer[0].interleaving;
						d->longest_intl_layer = (u8)i;
					}
				}
			}

			switch (deeper_interleaver) {
			case 0:
				locks = 2; /* locks is the tmp local variable name */
				break;
			case 3:
				locks = 8;
				break;
			default:
				locks = 2 * deeper_interleaver;
				break;
			}

			if (d->diversity_onoff != 0) /* because of diversity sync */
				locks *= 2;

			d->timeout_ms = now + (u64)200 * locks; /* give the mpeg lock 800ms if sram is present */

			d->tune_state = DEMOD_STEP10;
		} else {
			d->tune_state = DEMOD_STOP;
		}
		break;

	case DEMOD_STEP10: /* 40 */
		locks = read_lock(d);
		if (locks & (1 << (7 - d->longest_intl_layer))) { /* mpeg lock: check the longest one */
			d->status = STATUS_DATA_LOCKED;
			d->tune_state = DEMOD_STOP;
		} else if (now > d->timeout_ms) {
			/* the longest interleaver's layer did not lock; see whether another one did in the meantime */
			if (locks & (0x7 << 5))
				d->status = STATUS_DATA_LOCKED;
			else
				d->status = STATUS_TUNE_FAILED;
			d->tune_state = DEMOD_STOP;
		}
		break;
	}

	/* tuning is finished - cleanup the demod */
	if (d->tune_state == DEMOD_STOP) /* (42) */
		ret = 0;

	if (ret > 0 && d->tune_state > DEMOD_STEP3)
		return ret * (int)d->symbol_duration;
	if (ret > 0 && ret < (int)d->symbol_duration)
		return (int)d->symbol_duration; /* at least one symbol */
	return ret;
}

static int read_status_bits(struct torne_dib *d)
{
	u16 lock = read_lock(d);
	int stat = 0;

	if ((lock >> 13) & 1)
		stat |= FE_HAS_SIGNAL;
	if ((lock >> 8) & 1) /* Equal */
		stat |= FE_HAS_CARRIER;
	if (((lock >> 1) & 0xf) == 0xf) /* TMCC_SYNC */
		stat |= FE_HAS_SYNC;
	if (((lock >> 12) & 1) && ((lock >> 5) & 7) != 0) /* FEC MPEG */
		stat |= FE_HAS_LOCK;
	if ((lock >> 12) & 1) {
		if (dib_read_word(d, 554) & 0x01) /* Viterbi Layer A */
			stat |= FE_HAS_VITERBI;
		if (dib_read_word(d, 555) & 0x01) /* Viterbi Layer B */
			stat |= FE_HAS_VITERBI;
		if (dib_read_word(d, 556) & 0x01) /* Viterbi Layer C */
			stat |= FE_HAS_VITERBI;
	}
	return stat;
}

/* reads the parameters the autosearch found back into the channel */
static void get_frontend(struct torne_dib *d)
{
	struct dib_channel *c = &d->c;
	u16 val;
	u16 i;

	c->bandwidth_hz = 6000000;

	/* called too early, this makes the tune either not lock or not sync; so do nothing until the frontend has TMCC sync */
	if (!(read_status_bits(d) & FE_HAS_SYNC))
		return;

	dib_read_word(d, 508); /* ISDB-Tsb mode: read and dropped */

	val = dib_read_word(d, 570);
	c->inversion = (val & 0x40) >> 6;
	switch ((val & 0x30) >> 4) {
	case 1:
		c->transmission_mode = TRANSMISSION_2K;
		break;
	case 2:
		c->transmission_mode = TRANSMISSION_4K;
		break;
	default:
		c->transmission_mode = TRANSMISSION_8K;
		break;
	}

	switch (val & 0x3) {
	case 0:
		c->guard_interval = GUARD_1_32;
		break;
	case 1:
		c->guard_interval = GUARD_1_16;
		break;
	case 2:
		c->guard_interval = GUARD_1_8;
		break;
	case 3:
		c->guard_interval = GUARD_1_4;
		break;
	}

	val = dib_read_word(d, 505);
	c->isdbt_partial_reception = val & 1;

	for (i = 0; i < 3; i++) {
		val = dib_read_word(d, 493 + i) & 0x0f;
		c->layer[i].segment_count = (u8)val;

		val = dib_read_word(d, 499 + i) & 0x3;
		/* interleaving can be 0, 1, 2 or 4 */
		if (val == 3)
			val = 4;
		c->layer[i].interleaving = (u8)val;

		val = dib_read_word(d, 481 + i);
		switch (val & 0x7) {
		case 1:
			c->layer[i].fec = FEC_1_2;
			break;
		case 2:
			c->layer[i].fec = FEC_2_3;
			break;
		case 3:
			c->layer[i].fec = FEC_3_4;
			break;
		case 5:
			c->layer[i].fec = FEC_5_6;
			break;
		default:
			c->layer[i].fec = FEC_7_8;
			break;
		}

		val = dib_read_word(d, 487 + i);
		switch (val & 0x3) {
		case 0:
			c->layer[i].modulation = MODULATION_DQPSK;
			break;
		case 1:
			c->layer[i].modulation = MODULATION_QPSK;
			break;
		case 2:
			c->layer[i].modulation = MODULATION_QAM16;
			break;
		default:
			c->layer[i].modulation = MODULATION_QAM64;
			break;
		}
	}
}

/* ================= DiB0090 tuner (dib0090.go) ================= */

#define ADC_TARGET (-220)
#define GAIN_ALPHA 5
#define WBD_ALPHA 6
#define INTERN_WAIT 10
#define CAP_VALUE_MIN 9
#define CAP_VALUE_MAX 40
#define HR_MIN 25
#define HR_MAX 40
#define POLY_MIN 0
#define POLY_MAX 8
#define WBD_MUX 0x781
#define LPF 100

static void tun_set_default_config(struct torne_dib *d, const u16 *n)
{
	for (;;) {
		u16 count = n[0];
		u8 reg;

		if (count == 0)
			return;
		reg = (u8)n[1];
		n += 2;
		for (; count > 0; count--) {
			tun_write_reg(d, reg, n[0]);
			n++;
			reg++;
		}
	}
}

static int tun_identify(struct torne_dib *d)
{
	u16 v = tun_read_reg(d, 0x1a);

	if (d->err)
		return d->err;

	d->t_id_p1g = 0;
	d->t_id_in_soc = 0;

	/* without PLL lock info */
	v &= ~KROSUS_PLL_LOCKED;

	d->t_id_version = (u8)(v & 0xff);
	d->t_id_product = (u8)((v >> 8) & 0xf);

	if (d->t_id_product != KROSUS) {
		LOG(d, TORNE_LOG_ERR, "DiB0090: unknown product (version register %#04x)", v);
		return -ENODEV;
	}

	if ((d->t_id_version & 0x3) == SOC) {
		LOG(d, TORNE_LOG_ERR, "DiB0090: SoC version %#02x belongs to a DiB7090 or DiB8096P, not supported", d->t_id_version);
		return -ENODEV;
	}

	switch ((d->t_id_version >> 5) & 0x7) {
	case MP001:
	case MP005:
	case MP008:
	case MP009:
		break;
	default:
		LOG(d, TORNE_LOG_ERR, "DiB0090: unknown package variant (version %#02x)", d->t_id_version);
		return -ENODEV;
	}

	switch (d->t_id_version & 0x1f) {
	case P1G21R2:
	case P1G:
		d->t_id_p1g = 1;
		break;
	case P1DEF:
	case P1C:
		break;
	case P1AB:
		LOG(d, TORNE_LOG_ERR, "DiB0090: P1-A/B detected: driver is deactivated - not available");
		return -ENODEV;
	default:
		LOG(d, TORNE_LOG_ERR, "DiB0090: unknown revision (version %#02x)", d->t_id_version);
		return -ENODEV;
	}
	return 0;
}

/* the tuner's reset and sleep inputs are the DiB8000's GPIO5 (active low) and GPIO0 */
static void tun_reset_line(struct torne_dib *d, int onoff)
{
	dib_set_gpio(d, 5, 0, (u8)bw(onoff == 0));
}

static void tun_sleep_line(struct torne_dib *d, int onoff)
{
	dib_set_gpio(d, 0, 0, (u8)onoff);
}

static void tun_reset_digital(struct torne_dib *d)
{
	u16 not_analog = DIB0090_CFG_ANALOG_OUTPUT == 0 ? 1 : 0;
	u16 pll_cfg;

	/* HARD_RESET */
	tun_sleep_line(d, 0);
	msleep_(d, 10);
	tun_reset_line(d, 1);
	msleep_(d, 10);
	tun_reset_line(d, 0);
	msleep_(d, 10);

	tun_write_reg(d, 0x24, EN_PLL | EN_CRYSTAL);

	tun_write_reg(d, 0x1b, EN_DIG_CLK | EN_PLL | EN_CRYSTAL); /* PLL, DIG_CLK and CRYSTAL remain */
	/* adcClkOutRatio=8->7, release reset */
	tun_write_reg(d, 0x20, (u16)((DIB0090_CFG_IO_ADC_CLOCK_RATIO - 1) << 11 | 0 << 10 | 1 << 9 | 1 << 8 | 0 << 4 | 0));
	if (DIB0090_CFG_CLKOUTDRIVE != 0)
		tun_write_reg(d, 0x23, (u16)(0 << 15 | not_analog << 14 | 2 << 10 | 1 << 9 | 0 << 8 |
					     DIB0090_CFG_CLKOUTDRIVE << 5 | DIB0090_CFG_CLKOUTTOBAMSE << 4 | 0 << 2 | 0));
	else
		tun_write_reg(d, 0x23, (u16)(0 << 15 | not_analog << 14 | 2 << 10 | 1 << 9 | 0 << 8 | 7 << 5 |
					     DIB0090_CFG_CLKOUTTOBAMSE << 4 | 0 << 2 | 0));

	/* read PLL current config */
	pll_cfg = tun_read_reg(d, 0x21);

	if (DIB0090_CFG_IO_PLL_BYPASS != 0) {
		pll_cfg |= (u16)DIB0090_CFG_IO_PLL_BYPASS << 15;
		tun_write_reg(d, 0x21, pll_cfg);
	}
}

static void tun_wakeup(struct torne_dib *d)
{
	tun_sleep_line(d, 0);

	/* enable dataTX in case we have been restarted in the wrong moment */
	tun_write_reg(d, 0x23, tun_read_reg(d, 0x23) | 1 << 14);
}

static void tun_sleep_tuner(struct torne_dib *d)
{
	tun_sleep_line(d, 1);
}

static void tun_dcc_freq(struct torne_dib *d, u8 fast)
{
	if (fast)
		tun_write_reg(d, 0x04, 0);
	else
		tun_write_reg(d, 0x04, 1);
}

static void tun_set_boost(struct torne_dib *d, u16 onoff)
{
	d->t_bb1_def &= 0xdfff;
	d->t_bb1_def |= onoff << 13;
}

static void tun_set_rframp_pwm(struct torne_dib *d, const u16 *cfg)
{
	d->t_rf_ramp = cfg;

	tun_write_reg(d, 0x2a, 0xffff);

	tun_write_regs(d, 0x2c, cfg + 3, 6);
	tun_write_regs(d, 0x3e, cfg + 9, 2);
}

static void tun_set_bbramp_pwm(struct torne_dib *d, const u16 *cfg)
{
	d->t_bb_ramp = cfg;

	tun_set_boost(d, bw(cfg[0] > 500)); /* we want the boost if the gain is higher that 50dB */

	tun_write_reg(d, 0x33, 0xffff);
	tun_write_regs(d, 0x35, cfg + 3, 4);
}

static void tun_pwm_gain_reset(struct torne_dib *d)
{
	const u16 *bb_ramp = bb_ramp_pwm_normal; /* default baseband config */
	const u16 *rf_ramp = NULL;
	u16 en_pwm_rf_mux = 1;

	/* reset the AGC */
	if (DIB0090_CFG_USE_PWM_AGC != 0) {
		switch (d->t_current_band) {
		case BAND_CBAND:
			rf_ramp = rf_ramp_pwm_cband;
			break;
		case BAND_VHF:
			rf_ramp = rf_ramp_pwm_vhf;
			break;
		case BAND_UHF:
			rf_ramp = rf_ramp_pwm_uhf;
			break;
		}
		if (rf_ramp)
			tun_set_rframp_pwm(d, rf_ramp);
		tun_set_bbramp_pwm(d, bb_ramp);

		/* activate the ramp generator using PWM control */
		if (rf_ramp && ((d->t_rf_ramp && d->t_rf_ramp[0] == 0) ||
				(d->t_current_band == BAND_CBAND && (d->t_id_version & 0x1f) <= P1DEF)))
			/* de-engage mux for direct gain reg control */
			en_pwm_rf_mux = 0;

		tun_write_reg(d, 0x32, en_pwm_rf_mux << 12 | en_pwm_rf_mux << 11);

		/* set fast servo cutoff to start AGC; 0 = 1KHz ; 1 = 50Hz ; 2 = 150Hz ; 3 = 50KHz ; 4 = servo fast */
		tun_write_reg(d, 0x04, 1);
		tun_write_reg(d, 0x39, 1 << 10); /* 0 gain by default */
	}
}

/* ---- software AGC ---- */

struct slope {
	s16 range_value;
	s16 slope;
};

static const struct slope dib0090_wbd_slopes[3] = {
	{ 66, 120 },  /* -64,-52: offset -   65 */
	{ 600, 170 }, /* -52,-35: 65     -  665 */
	{ 170, 250 }, /* -45,-10: 665    - 835 */
};

static u16 slopes_to_scale(const struct slope *slopes, int n, s16 val)
{
	u16 rest, ret = 0;
	int i;

	for (i = 0; i < n; i++) {
		if (val > slopes[i].range_value)
			rest = (u16)slopes[i].range_value;
		else
			rest = (u16)val;
		ret += (u16)(((int)rest * (int)slopes[i].slope) / (int)slopes[i].range_value);
		val -= (s16)rest;
	}
	return ret;
}

static s16 tun_wbd_to_db(struct torne_dib *d, u16 wbd)
{
	wbd &= 0x3ff;
	if (wbd < d->t_wbd_offset)
		wbd = 0;
	else
		wbd -= d->t_wbd_offset;
	/* -64dB is the floor */
	return (s16)(-640 + (s16)slopes_to_scale(dib0090_wbd_slopes, 3, (s16)wbd));
}

static void tun_wbd_target_for(struct torne_dib *d)
{
	u16 offset = 250;

	if (d->t_current_band == BAND_VHF)
		offset = DIB0090_CFG_WBD_VHF_OFFSET;
	if (d->t_current_band == BAND_CBAND)
		offset = DIB0090_CFG_WBD_CBAND_OFFSET;
	d->t_wbd_target = tun_wbd_to_db(d, (u16)(d->t_wbd_offset + offset));
}

/* a ramp entry, or 0 past the end (Linux reads whatever follows the table) */
static u16 ramp_at(const u16 *ramp, int index, int len)
{
	return index < len ? ramp[index] : 0;
}

static int ramp_len(const u16 *ramp)
{
	if (ramp == rf_ramp_pwm_cband || ramp == rf_ramp_pwm_vhf || ramp == rf_ramp_pwm_uhf)
		return 11;
	if (ramp == rf_ramp_pwm_cband_7090p)
		return 11;
	return 7; /* bb ramps */
}

static void tun_gain_apply(struct torne_dib *d, s16 gain_delta, s16 top_delta, bool force)
{
	u16 gain_reg[4], gain, rf, bb;
	const u16 *ramp;
	int at, i, rlen;
	u16 ref;
	static const u8 regs[4] = { 0x08, 0x0a, 0x0f, 0x01 };

	if (top_delta < -511)
		top_delta = -511;
	if (top_delta > 511)
		top_delta = 511;

	if (force) {
		top_delta = (s16)((int)top_delta * (1 << WBD_ALPHA));
		gain_delta = (s16)((int)gain_delta * (1 << GAIN_ALPHA));
	}

	if ((int)top_delta >= (int)(s16)(u16)(d->t_rf_ramp[0] << WBD_ALPHA) - (int)d->t_rf_gain_limit) /* overflow */
		d->t_rf_gain_limit = (s16)(u16)(d->t_rf_ramp[0] << WBD_ALPHA);
	else
		d->t_rf_gain_limit += top_delta;

	if (d->t_rf_gain_limit < 0) /* underflow */
		d->t_rf_gain_limit = 0;

	/* use gain as a temporary variable and correct current_gain */
	gain = (u16)(((u16)(d->t_rf_gain_limit >> WBD_ALPHA) + d->t_bb_ramp[0]) << GAIN_ALPHA);
	if ((int)gain_delta >= (int)(s16)gain - (int)d->t_current_gain) /* overflow */
		d->t_current_gain = (s16)gain;
	else
		d->t_current_gain += gain_delta;
	/* cannot be less than 0 (only if gain_delta is less than 0 we can have current_gain < 0) */
	if (d->t_current_gain < 0)
		d->t_current_gain = 0;

	/* now split total gain to rf and bb gain */
	gain = (u16)(d->t_current_gain >> GAIN_ALPHA);

	/* requested gain is bigger than rf gain limit - ACI/WBD adjustment */
	if (gain > (u16)(d->t_rf_gain_limit >> WBD_ALPHA)) {
		rf = (u16)(d->t_rf_gain_limit >> WBD_ALPHA);
		bb = gain - rf;
		if (bb > d->t_bb_ramp[0])
			bb = d->t_bb_ramp[0];
	} else { /* high signal level -> all gains put on RF */
		rf = gain;
		bb = 0;
	}

	d->t_gain[0] = rf;
	d->t_gain[1] = bb;

	/* software ramp: start with RF gains */
	ramp = d->t_rf_ramp;
	rlen = ramp_len(ramp);
	at = 1; /* point on RF LNA1 max gain */
	ref = rf;
	gain_reg[0] = gain_reg[1] = gain_reg[2] = gain_reg[3] = 0;
	for (i = 0; i < 7; i++) { /* go over all amplifiers => 5RF amps + 2 BB amps = 7 amps */
		int g0 = ramp_at(ramp, at, rlen), g1 = ramp_at(ramp, at + 1, rlen), g2 = ramp_at(ramp, at + 2, rlen);
		u16 v;

		if (g0 == 0 || (int)ref < g1 - g0) /* the amp is off, or starts to work from a higher gain */
			v = 0;
		else if ((int)ref >= g1) /* higher than the high working point of this amp */
			v = (u16)g2;
		else /* somewhere in its range */
			v = (u16)(((int)ref - (g1 - g0)) * g2 / g0);

		switch (i) {
		case 0: /* LNA 1 reg mapping */
			gain_reg[0] = v;
			break;
		case 1: /* LNA 2 reg mapping */
			gain_reg[0] |= v << 7;
			break;
		case 2: /* LNA 3 reg mapping */
			gain_reg[1] = v;
			break;
		case 3: /* LNA 4 reg mapping */
			gain_reg[1] |= v << 7;
			break;
		case 4: /* CBAND LNA reg mapping */
			gain_reg[2] = v | d->t_rf_lt_def;
			break;
		case 5: /* BB gain 1 reg mapping */
			gain_reg[3] = v << 3;
			break;
		case 6: /* BB gain 2 reg mapping */
			gain_reg[3] |= v << 8;
			break;
		}

		at += 3; /* go to next gain bloc */

		/* when RF is finished, start with BB */
		if (i == 4) {
			ramp = d->t_bb_ramp;
			rlen = ramp_len(ramp);
			at = 1; /* point on BB gain 1 max gain */
			ref = bb;
		}
	}
	gain_reg[3] |= d->t_bb1_def;
	gain_reg[3] |= (u16)((bb % 10) * 100 / 125);

	/* write the amplifier regs */
	for (i = 0; i < 4; i++) {
		u16 v = gain_reg[i];

		if (force || d->t_gain_reg[i] != v) {
			d->t_gain_reg[i] = v;
			tun_write_reg(d, regs[i], v);
		}
	}
}

static void tun_set_rframp(struct torne_dib *d, const u16 *cfg)
{
	d->t_rf_ramp = cfg;
}

static void tun_set_bbramp(struct torne_dib *d, const u16 *cfg)
{
	d->t_bb_ramp = cfg;
	tun_set_boost(d, bw(cfg[0] > 500)); /* we want the boost if the gain is higher that 50dB */
}

static u32 tun_get_slow_adc_val(struct torne_dib *d)
{
	return tun_read_reg(d, 0x1d);
}

/* one step of the software AGC, run from AGC_START until the tune state reaches AGC_STOP; returns the wait in ms */
static int tun_gain_control(struct torne_dib *d)
{
	int ret = 10;
	bool apply_gain_immediately = true;
	s16 wbd_error = 0, adc_error = 0;

	if (d->t_tune_state == AGC_START) {
		d->t_agc_freeze = 0;
		tun_write_reg(d, 0x04, 0x0);

		if (d->t_current_band == BAND_VHF && d->t_id_p1g == 0) {
			tun_set_rframp(d, rf_ramp_pwm_vhf);
			tun_set_bbramp(d, bb_ramp_pwm_normal);
		} else if (d->t_current_band == BAND_CBAND && d->t_id_p1g == 0) {
			tun_set_rframp(d, rf_ramp_pwm_cband);
			tun_set_bbramp(d, bb_ramp_pwm_normal);
		} else if ((d->t_current_band == BAND_CBAND || d->t_current_band == BAND_VHF) && d->t_id_p1g != 0) {
			tun_set_rframp(d, rf_ramp_pwm_cband_7090p);
			tun_set_bbramp(d, bb_ramp_pwm_normal_socs);
		} else {
			tun_set_rframp(d, rf_ramp_pwm_uhf);
			tun_set_bbramp(d, bb_ramp_pwm_normal);
		}

		tun_write_reg(d, 0x32, 0);
		tun_write_reg(d, 0x39, 0);

		tun_wbd_target_for(d);

		d->t_rf_gain_limit = (s16)(u16)(d->t_rf_ramp[0] << WBD_ALPHA);
		d->t_current_gain = (s16)(u16)((u16)((d->t_rf_ramp[0] + d->t_bb_ramp[0]) / 2) << GAIN_ALPHA);

		d->t_tune_state = AGC_STEP0;
	} else if (d->t_agc_freeze == 0) {
		int cnt = 1, i;
		s16 wbd = 0;

		if (d->t_tune_state == AGC_STEP0)
			cnt = 5;
		for (i = 0; i < cnt; i++)
			wbd += tun_wbd_to_db(d, (u16)tun_get_slow_adc_val(d));
		wbd /= (s16)cnt;
		wbd_error = (s16)(d->t_wbd_target - wbd);

		if (d->t_tune_state == AGC_STEP0) {
			if (wbd_error < 0 && d->t_rf_gain_limit > 0 && d->t_id_p1g == 0) {
				/* in case of CBAND tune reduce first the lt_gain2 before adjusting the RF gain */
				u16 ltg2 = (d->t_rf_lt_def >> 10) & 0x7;

				if (d->t_current_band == BAND_CBAND && ltg2 != 0) {
					ltg2 >>= 1;
					d->t_rf_lt_def &= ltg2 << 10; /* reduce in 3 steps from 7 to 0 */
				}
			} else {
				d->t_agc_step = 0;
				d->t_tune_state = AGC_STEP1;
			}
		} else {
			/* calc the adc power */
			s32 adc = dib_get_adc_power(d, 1);

			adc = (s32)(((s64)adc * 355774 + (1 << 20)) >> 21); /* included in [0:-700] */

			adc_error = (s16)(ADC_TARGET - adc);
			/* Linux adds 60 here for an ISDB-T layer in 16QAM or 64QAM; a tune starts from a cleared channel */

			if (d->t_tune_state == AGC_STEP1) { /* quickly go to the correct range of the ADC power */
				u8 step = d->t_agc_step;

				d->t_agc_step++;
				if (iabs(adc_error) < 50 || step > 5) {
					tun_write_reg(d, 0x02, 1 << 15 | 3 << 11 | 6 << 6 | 32);
					tun_write_reg(d, 0x04, 0x01); /* 0 = 1KHz ; 1 = 150Hz ; 2 = 50Hz ; 3 = 50KHz ; 4 = servo fast */
					d->t_tune_state = AGC_STOP;
				}
			} else {
				/* everything higher than or equal to AGC_STOP means tracking */
				ret = 100; /* 10ms interval */
				apply_gain_immediately = false;
			}
		}
	}

	/* apply gain */
	if (d->t_agc_freeze == 0)
		tun_gain_apply(d, adc_error, wbd_error, apply_gain_immediately);
	return ret;
}

static void tun_set_dc_servo(struct torne_dib *d, u8 dc_servo_cutoff)
{
	if (dc_servo_cutoff < 4)
		tun_write_reg(d, 0x04, dc_servo_cutoff);
}

static u16 tun_get_wbd_target(struct torne_dib *d)
{
	u32 f_mhz = d->c.frequency / 1000000;
	s32 current_temp = d->t_temperature, wbd_thot, wbd_tcold;
	int index = 0;
	const struct wbd_slope *wbd;

	while (f_mhz > d->t_current_wbd_table[index].max_freq)
		index++;
	wbd = &d->t_current_wbd_table[index];

	if (current_temp < 0)
		current_temp = 0;
	if (current_temp > 128)
		current_temp = 128;

	d->t_wbdmux &= ~(7 << 13);
	if (wbd->wbd_gain != 0)
		d->t_wbdmux |= (u16)wbd->wbd_gain << 13;
	else
		d->t_wbdmux |= 4 << 13;

	tun_write_reg(d, 0x10, d->t_wbdmux);

	/* unsigned arithmetic in C, stored signed */
	wbd_thot = (s32)((u32)wbd->offset_hot - (((u32)wbd->slope_hot * f_mhz) >> 6));
	wbd_tcold = (s32)((u32)wbd->offset_cold - (((u32)wbd->slope_cold * f_mhz) >> 6));

	wbd_tcold += ((wbd_thot - wbd_tcold) * current_temp) >> 7;

	d->t_wbd_target = tun_wbd_to_db(d, (u16)((s32)d->t_wbd_offset + wbd_tcold));

	return (u16)((s32)d->t_wbd_offset + wbd_tcold);
}

/* the baseband trims burnt in at the factory, or computed from a measurement when the fuses are blank */
static void tun_set_efuse(struct torne_dib *d)
{
	u8 c, h, n = 0;
	u16 e2 = tun_read_reg(d, 0x26);
	u16 e4 = tun_read_reg(d, 0x28);

	/* Linux compares the whole version byte with the revision codes here, so only a blank fuse set normally takes this branch */
	if (d->t_id_version == P1DEF || d->t_id_version == P1G || e2 == 0xffff) {
		u16 cal;

		tun_write_reg(d, 0x22, 0x10);
		cal = (tun_read_reg(d, 0x22) >> 6) & 0x3ff;

		if (cal < 670 || cal == 1023)
			cal = 850;
		n = (u8)(165 - (((int)cal * 10) >> 6));
		e2 = (u16)(3 << 12 | 34 << 6 | n);
		e4 = e2;
	}

	if (e2 != e4)
		e2 &= e4; /* remove the redundancy */

	if (e2 != 0xffff) {
		c = (u8)(e2 & 0x3f);
		n = (u8)((e2 >> 12) & 0xf);
		h = (u8)((e2 >> 6) & 0x3f);

		if (c >= CAP_VALUE_MAX || c <= CAP_VALUE_MIN)
			c = 32;
		else
			c += 14;
		if (h >= HR_MAX || h <= HR_MIN)
			h = 34;
		if (n >= POLY_MAX || n <= POLY_MIN)
			n = 3;

		tun_write_reg(d, 0x13, (u16)h << 10);
		e2 = (u16)((u16)n << 11 | (u16)(h >> 2) << 6 | c);
		tun_write_reg(d, 0x2, e2); /* load the BB_2 */
	}
}

static int tun_reset(struct torne_dib *d)
{
	int rc;

	tun_reset_digital(d);
	rc = tun_identify(d);
	if (rc)
		return rc;

	if ((tun_read_reg(d, 0x1a) >> 5) & 0x2)
		tun_write_reg(d, 0x1b, EN_IQADC | EN_BB | EN_BIAS | EN_DIG_CLK | EN_PLL | EN_CRYSTAL);
	else
		tun_write_reg(d, 0x1b, EN_DIG_CLK | EN_PLL | EN_CRYSTAL);

	tun_set_default_config(d, dib0090_defaults);

	if (d->t_id_p1g)
		tun_set_default_config(d, dib0090_p1g_additional_defaults);

	/* update the efuse: only available for Krosus > P1C */
	if ((d->t_id_version & 0x1f) >= P1DEF)
		tun_set_efuse(d);

	/* configure in function of the crystal */
	if (DIB0090_CFG_FORCE_CRYSTAL_MODE != 0)
		tun_write_reg(d, 0x14, DIB0090_CFG_FORCE_CRYSTAL_MODE & 3);
	else if (DIB0090_CFG_IO_CLOCK_KHZ >= 24000)
		tun_write_reg(d, 0x14, 1);
	else
		tun_write_reg(d, 0x14, 2);

	/* enable iq-offset-calibration and wbd-calibration when tuning next time */
	d->t_calibrate = DC_CAL | WBD_CAL | TEMP_CAL;

	return d->err;
}

static int register_tuner(struct torne_dib *d)
{
	d->t_current_wbd_table = dib8090_wbd_table;
	return tun_reset(d);
}

/* steps() macro of the DC calibration */
static s8 steps_(s8 u)
{
	return u > 15 ? (s8)(u - 16) : u;
}

static int tun_get_offset(struct torne_dib *d)
{
	int ret = INTERN_WAIT * 10;

	switch (d->t_tune_state) {
	case TUNER_STEP2:
		/* turns to positive */
		tun_write_reg(d, 0x1f, 0x7);
		d->t_tune_state = TUNER_STEP3;
		break;

	case TUNER_STEP3:
		d->t_adc_diff = (s16)tun_read_reg(d, 0x1d);

		/* turns to negative */
		tun_write_reg(d, 0x1f, 0x4);
		d->t_tune_state = TUNER_STEP4;
		break;

	case TUNER_STEP4:
		d->t_adc_diff = (s16)((int)d->t_adc_diff - (int)tun_read_reg(d, 0x1d));
		d->t_tune_state = TUNER_STEP5;
		ret = 0;
		break;
	}

	return ret;
}

static void tun_set_trim(struct torne_dib *d)
{
	const struct dc_calibration *dc = &d->t_dc[d->t_dc_next];
	u16 *val = &d->t_bb6;

	if (dc->addr == 0x07)
		val = &d->t_bb7;

	*val &= (u16)~(0x1f << dc->offset);
	*val |= (u16)((s32)d->t_step << dc->offset);

	tun_write_reg(d, dc->addr, *val);
}

/* CT_TUNER_STEP_0 of the DC calibration, which CT_TUNER_START falls through into */
static void tun_dc_step0(struct torne_dib *d)
{
	const struct dc_calibration *dc = &d->t_dc[d->t_dc_next];

	tun_write_reg(d, 0x01, dc->bb1);
	tun_write_reg(d, 0x07, d->t_bb7 | (u16)dc->i << 7);

	d->t_step = 0;
	d->t_min_adc_diff = 1023;
	d->t_tune_state = TUNER_STEP1;
}

static int tun_dc_offset_calibration(struct torne_dib *d)
{
	int ret = 0;

	switch (d->t_tune_state) {
	case TUNER_START: {
		u16 reg;

		/* force vcm2 = 0.8V */
		d->t_bb6 = 0;
		d->t_bb7 = 0x040d;

		/* the LNA AND LO are off */
		reg = tun_read_reg(d, 0x24) & 0x0ffb; /* shutdown lna and lo */
		tun_write_reg(d, 0x24, reg);

		d->t_wbdmux = tun_read_reg(d, 0x10);
		tun_write_reg(d, 0x10, (u16)((d->t_wbdmux & ~(0xff << 3)) | 0x7 << 3 | 0x3));
		tun_write_reg(d, 0x23, tun_read_reg(d, 0x23) & ~(1 << 14));

		d->t_dc = dc_table;
		if (d->t_id_p1g)
			d->t_dc = dc_p1g_table;
		d->t_dc_next = 0;
		tun_dc_step0(d);
		ret = 50;
		break;
	}

	case TUNER_STEP0:
		tun_dc_step0(d);
		ret = 50;
		break;

	case TUNER_STEP1:
		tun_set_trim(d);
		d->t_tune_state = TUNER_STEP2;
		break;

	case TUNER_STEP2:
	case TUNER_STEP3:
	case TUNER_STEP4:
		ret = tun_get_offset(d);
		break;

	case TUNER_STEP5: { /* found an offset */
		const struct dc_calibration *dc = &d->t_dc[d->t_dc_next];

		if (d->t_step == 0 && d->t_adc_diff < 0)
			d->t_min_adc_diff = -1023;

		/* first turn for this frequency */
		if (d->t_step == 0) {
			if (dc->pga != 0 && d->t_adc_diff < 0)
				d->t_step = 0x10;
			if (dc->pga == 0 && d->t_adc_diff > 0)
				d->t_step = 0x10;
		}

		/* look for a change of sign in adc_diff; min_adc_diff stores step N-1 */
		if (((u16)d->t_adc_diff & 0x8000) == ((u16)d->t_min_adc_diff & 0x8000) && steps_(d->t_step) < 15) {
			/* stop search when the delta the sign is changing and Steps = 15 and Step = 0 is force for continuance */
			d->t_step++;
			d->t_min_adc_diff = d->t_adc_diff;
			d->t_tune_state = TUNER_STEP1;
		} else {
			/* the minimum was what we have seen in the step before */
			if (iabs(d->t_adc_diff) > iabs(d->t_min_adc_diff))
				d->t_step--;

			tun_set_trim(d);

			d->t_dc_next++;
			if (d->t_dc[d->t_dc_next].addr == 0) /* done */
				d->t_tune_state = TUNER_STEP6;
			else
				d->t_tune_state = TUNER_STEP0;
		}
		break;
	}

	case TUNER_STEP6:
		tun_write_reg(d, 0x07, d->t_bb7 & ~0x0008);
		tun_write_reg(d, 0x1f, 0x7);
		d->t_tune_state = TUNER_START; /* reset done -> real tuning can now begin */
		d->t_calibrate &= ~DC_CAL;
		break;
	}
	return ret;
}

static int tun_wbd_calibration(struct torne_dib *d)
{
	u8 wbd_gain;
	int index = 0;
	const struct wbd_slope *wbd;

	switch (d->t_tune_state) {
	case TUNER_START:
		while (d->t_current_rf / 1000 > d->t_current_wbd_table[index].max_freq)
			index++;
		wbd = &d->t_current_wbd_table[index];
		if (wbd->wbd_gain != 0)
			wbd_gain = wbd->wbd_gain;
		else
			wbd_gain = 4; /* Linux lowers this to 2 only for LBAND/SBAND configurations that are never compiled in */

		if (wbd_gain == d->t_wbd_calibration_gain) { /* the WBD calibration has already been done */
			d->t_tune_state = TUNER_START;
			d->t_calibrate &= ~WBD_CAL;
			return 0;
		}

		tun_write_reg(d, 0x10, (u16)(0x1b81 | 1 << 10 | (u16)wbd_gain << 13 | 1 << 3));

		tun_write_reg(d, 0x24, (EN_UHF & 0x0fff) | 1 << 1);
		d->t_tune_state = TUNER_STEP0;
		d->t_wbd_calibration_gain = wbd_gain;
		return 90; /* wait for the WBDMUX to switch and for the ADC to sample */

	case TUNER_STEP0:
		d->t_wbd_offset = (u16)tun_get_slow_adc_val(d);
		d->t_tune_state = TUNER_START; /* reset done -> real tuning can now begin */
		d->t_calibrate &= ~WBD_CAL;
		break;
	}
	return 0;
}

static void tun_set_bandwidth(struct torne_dib *d)
{
	u16 tmp;
	u32 bandwidth_khz = d->c.bandwidth_hz / 1000;

	if (bandwidth_khz <= 5000)
		tmp = 3 << 14;
	else if (bandwidth_khz <= 6000)
		tmp = 2 << 14;
	else if (bandwidth_khz <= 7000)
		tmp = 1 << 14;
	else
		tmp = 0 << 14;

	d->t_bb1_def &= 0x3fff;
	d->t_bb1_def |= tmp;

	tun_write_reg(d, 0x01, d->t_bb1_def); /* be sure that we have the right bb-filter */

	tun_write_reg(d, 0x03, 0x6008);			    /* vcm3_trim = 1 ; filter2_gm1_trim = 8 ; filter2_cutoff_freq = 0 */
	tun_write_reg(d, 0x04, 0x1);			    /* 0 = 1KHz ; 1 = 50Hz ; 2 = 150Hz ; 3 = 50KHz ; 4 = servo fast */
	tun_write_reg(d, 0x02, 5 << 11 | 8 << 6 | (22 & 0x3f)); /* 22 = cap_value */
	tun_write_reg(d, 0x05, 0xabcd);
}

/* P1G silicon searches in hardware; earlier revisions binary-search the capacitor trim against the slow ADC */
static int tun_captrim_search(struct torne_dib *d)
{
	int ret = 0;
	u16 lo4 = 0xe900;

	switch (d->t_tune_state) {
	case TUNER_START:
		tun_write_reg(d, 0x10, 0x2B1);
		tun_write_reg(d, 0x1e, 0x0032);

		if (d->t_tuner_is_tuned == 0) {
			/* prepare a complete captrim */
			if (d->t_id_p1g == 0) {
				d->t_step = 64;
				d->t_captrim = 64;
				d->t_fcaptrim = 64;
			}

			d->t_current_rf = d->t_rf_request;
		} else { /* we are already tuned to this frequency - the configuration is correct */
			if (d->t_id_p1g == 0) {
				/* do a minimal captrim even if the frequency has not changed */
				d->t_step = 4;
				d->t_captrim = (s8)(tun_read_reg(d, 0x18) & 0x7f);
				d->t_fcaptrim = d->t_captrim;
			}
		}
		d->t_adc_diff = 3000;
		d->t_tune_state = TUNER_STEP0;
		break;

	case TUNER_STEP0:
		if (d->t_id_p1g) {
			u16 ratio = 31;

			tun_write_reg(d, 0x40, 3 << 7 | ratio << 2 | 1 << 1 | 1);
			tun_read_reg(d, 0x40);
			ret = 50;
		} else {
			d->t_step /= 2;
			tun_write_reg(d, 0x18, lo4 | (u16)d->t_captrim);
		}
		d->t_tune_state = TUNER_STEP1;
		break;

	case TUNER_STEP1:
		if (d->t_id_p1g) {
			tun_write_reg(d, 0x40, 0x18c | 0 << 1 | 0);
			tun_read_reg(d, 0x40);

			d->t_fcaptrim = (s8)(tun_read_reg(d, 0x18) & 0x7F);
			d->t_tune_state = TUNER_STEP3;
		} else {
			/* MERGE for all krosus before P1G */
			u16 adc = (u16)tun_get_slow_adc_val(d);
			s16 adc_target;
			s8 step_sign;

			if (d->t_rest == 0)
				adc_target = 200;
			else
				adc_target = 400;

			if ((int)adc >= (int)adc_target) {
				adc -= (u16)adc_target;
				step_sign = -1;
			} else {
				adc = (u16)adc_target - adc;
				step_sign = 1;
			}

			if ((int)adc < (int)d->t_adc_diff) {
				d->t_adc_diff = (s16)adc;
				d->t_fcaptrim = d->t_captrim;
			}

			d->t_captrim += step_sign * d->t_step;
			if (d->t_step >= 1)
				d->t_tune_state = TUNER_STEP0;
			else
				d->t_tune_state = TUNER_STEP2;

			ret = 25;
		}
		break;

	case TUNER_STEP2: /* this step is only used by krosus < P1G */
		/* write the final captrim config */
		tun_write_reg(d, 0x18, lo4 | (u16)d->t_fcaptrim);

		d->t_tune_state = TUNER_STEP3;
		break;

	case TUNER_STEP3:
		d->t_calibrate &= ~CAPTRIM_CAL;
		d->t_tune_state = TUNER_STEP0;
		break;
	}

	return ret;
}

static int tun_get_temperature(struct torne_dib *d)
{
	int ret = 15;

	switch (d->t_tune_state) {
	case TUNER_START:
		d->t_wbdmux = tun_read_reg(d, 0x10);
		tun_write_reg(d, 0x10, (u16)((d->t_wbdmux & ~(0xff << 3)) | 0x8 << 3));

		d->t_bias = tun_read_reg(d, 0x13);
		tun_write_reg(d, 0x13, d->t_bias | 0x3 << 8);

		d->t_tune_state = TUNER_STEP0;
		/* wait for the WBDMUX to switch and for the ADC to sample */
		break;

	case TUNER_STEP0:
		d->t_adc_diff = (s16)tun_get_slow_adc_val(d);
		tun_write_reg(d, 0x13, (u16)((d->t_bias & ~(0x3 << 8)) | 0x2 << 8));
		d->t_tune_state = TUNER_STEP1;
		break;

	case TUNER_STEP1: {
		s16 val = (s16)tun_get_slow_adc_val(d);

		d->t_temperature = (s16)(((int)(s16)(((int)val - (int)d->t_adc_diff) * 180) >> 8) + 55);

		d->t_tune_state = TUNER_STEP2;
		break;
	}

	case TUNER_STEP2:
		tun_write_reg(d, 0x13, d->t_bias);
		tun_write_reg(d, 0x10, d->t_wbdmux); /* write back original WBDMUX */

		d->t_tune_state = TUNER_START;
		d->t_calibrate &= ~TEMP_CAL;
		if (DIB0090_CFG_ANALOG_OUTPUT == 0)
			tun_write_reg(d, 0x23, tun_read_reg(d, 0x23) | 1 << 14);
		break;

	default:
		ret = 0;
		break;
	}
	return ret;
}

/* one step of the tuner's state machine; returns the wait in 0.1 ms units */
static u32 tun_tune(struct torne_dib *d)
{
	const struct tuning_entry *tune = d->t_current_tune_table;
	const struct pll_entry *pll = d->t_current_pll_table;
	struct dib_channel *c = &d->c;
	u16 lo5, lo6, den, tmp;
	u32 fb_div, rest, f_ref, vcof_khz;
	u32 ret = 10; /* 1ms is the default delay most of the time */

	/* in any case we first need to do a calibration if needed */
	if (d->t_tune_state == TUNER_START) {
		if (d->t_calibrate & (DC_CAL | TEMP_CAL | WBD_CAL)) {
			/* deactivate DataTX before some calibrations */
			tun_write_reg(d, 0x23, tun_read_reg(d, 0x23) & ~(1 << 14));
		} else if (DIB0090_CFG_ANALOG_OUTPUT == 0) {
			/* activate DataTX in case a calibration has been done before */
			tun_write_reg(d, 0x23, tun_read_reg(d, 0x23) | 1 << 14);
		}
	}

	if (d->t_calibrate & DC_CAL)
		return (u32)tun_dc_offset_calibration(d);
	if (d->t_calibrate & WBD_CAL) {
		if (d->t_current_rf == 0)
			d->t_current_rf = c->frequency / 1000;
		return (u32)tun_wbd_calibration(d);
	}
	if (d->t_calibrate & TEMP_CAL)
		return (u32)tun_get_temperature(d);
	if (d->t_calibrate & CAPTRIM_CAL)
		return (u32)tun_captrim_search(d);

	switch (d->t_tune_state) {
	case TUNER_START: {
		s32 offset;

		d->t_current_band = band_of_frequency(c->frequency / 1000);
		offset = DIB0090_CFG_FREQ_OFFSET_KHZ_VHF;
		if (d->t_current_band == BAND_UHF)
			offset = DIB0090_CFG_FREQ_OFFSET_KHZ_UHF;
		d->t_rf_request = c->frequency / 1000 + (u32)offset;

		/* ISDB-T 1-seg would shift the tuning frequency here; not ported */

		if (d->t_current_rf != d->t_rf_request || (int)d->t_current_standard != c->delivery_system) {
			const struct tuning_entry *tune_table;
			int tune_len, pll_len, tune_index = 0, pll_index = 0;
			const struct pll_entry *pll_table;

			d->t_tuner_is_tuned = 0;
			d->t_current_rf = 0;
			d->t_current_standard = 0;

			if (d->t_id_p1g) {
				tune_table = dib0090_p1g_tuning_table;
				tune_len = sizeof(dib0090_p1g_tuning_table) / sizeof(dib0090_p1g_tuning_table[0]);
			} else {
				tune_table = dib0090_tuning_table;
				tune_len = sizeof(dib0090_tuning_table) / sizeof(dib0090_tuning_table[0]);
			}

			tmp = (d->t_id_version >> 5) & 0x7;

			if (tmp == MP005 || tmp == MP009) {
				/* CBAND tuner version for VHF */
				if (d->t_current_band == BAND_FM || d->t_current_band == BAND_CBAND || d->t_current_band == BAND_VHF) {
					d->t_current_band = BAND_CBAND; /* force CBAND */

					if (d->t_id_p1g) {
						tune_table = dib0090_p1g_tuning_table_fm_vhf_on_cband;
						tune_len = sizeof(dib0090_p1g_tuning_table_fm_vhf_on_cband) /
							   sizeof(dib0090_p1g_tuning_table_fm_vhf_on_cband[0]);
					} else {
						tune_table = dib0090_tuning_table_fm_vhf_on_cband;
						tune_len = sizeof(dib0090_tuning_table_fm_vhf_on_cband) /
							   sizeof(dib0090_tuning_table_fm_vhf_on_cband[0]);
					}
				}
			}

			if (d->t_id_p1g) {
				pll_table = dib0090_p1g_pll_table;
				pll_len = sizeof(dib0090_p1g_pll_table) / sizeof(dib0090_p1g_pll_table[0]);
			} else {
				pll_table = dib0090_pll_table;
				pll_len = sizeof(dib0090_pll_table) / sizeof(dib0090_pll_table[0]);
			}

			/* look for the interval; Linux walks off the end of the table above its last entry, here it stops */
			while (d->t_rf_request > tune_table[tune_index].max_freq && tune_index < tune_len - 1)
				tune_index++;
			while (d->t_rf_request > pll_table[pll_index].max_freq && pll_index < pll_len - 1)
				pll_index++;
			tune = &tune_table[tune_index];
			pll = &pll_table[pll_index];

			d->t_current_tune_table = tune;
			d->t_current_pll_table = pll;

			tun_write_reg(d, 0x0b, 0xb800 | tune->switch_trim);

			vcof_khz = ((u32)pll->hfdiv * d->t_rf_request) * 2;

			f_ref = DIB0090_CFG_IO_CLOCK_KHZ;
			if (DIB0090_CFG_F_REF_CLOCK_RATIO != 0)
				f_ref /= DIB0090_CFG_F_REF_CLOCK_RATIO;

			fb_div = vcof_khz / pll->topresc / f_ref;
			rest = vcof_khz / pll->topresc - fb_div * f_ref;

			if (rest < LPF) {
				rest = 0;
			} else if (rest < 2 * LPF) {
				rest = 2 * LPF;
			} else if (rest > f_ref - LPF) {
				rest = 0;
				fb_div++;
			} else if (rest > f_ref - 2 * LPF) {
				rest = f_ref - 2 * LPF;
			}
			rest = (rest * 6528) / (f_ref / 10);
			d->t_rest = rest;

			/* external loop filter, otherwise:
			 * lo5 = (0 << 15) | (0 << 12) | (0 << 11) | (3 << 9) | (4 << 6) | (3 << 4) | 4; lo6 = 0x0e34 */
			if (rest == 0) {
				if (pll->vco_band != 0)
					lo5 = 0x049f;
				else
					lo5 = 0x041f;
			} else {
				if (pll->vco_band != 0)
					lo5 = 0x049e;
				else if (DIB0090_CFG_ANALOG_OUTPUT != 0)
					lo5 = 0x041d;
				else
					lo5 = 0x041c;
			}

			if (d->t_id_p1g) /* bias is done automatically in P1G */
				lo5 = 0x42c;

			lo5 |= (u16)pll->hfdiv_code << 11 | (u16)pll->vco_band << 7; /* bit 15 is the split to the slave */

			if (DIB0090_CFG_IO_PLL_INT_LOOP_FILT == 0) {
				if (d->t_id_p1g || rest == 0)
					lo6 = 0xfff8;
				else
					lo6 = 0xff28;
			} else {
				lo6 = (u16)(DIB0090_CFG_IO_PLL_INT_LOOP_FILT << 3);
			}

			den = 1;

			if (rest > 0) {
				lo6 |= 1 << 2 | 2;
				den = 255;
			}
			tun_write_reg(d, 0x15, (u16)fb_div);
			if (DIB0090_CFG_F_REF_CLOCK_RATIO != 0)
				tun_write_reg(d, 0x16, (u16)(den << 8 | DIB0090_CFG_F_REF_CLOCK_RATIO));
			else
				tun_write_reg(d, 0x16, (u16)(den << 8 | 1));
			tun_write_reg(d, 0x17, (u16)rest);
			tun_write_reg(d, 0x19, lo5);
			tun_write_reg(d, 0x1c, lo6);

			lo6 = tune->tuner_enable;
			if (DIB0090_CFG_ANALOG_OUTPUT != 0)
				lo6 = (lo6 & 0xff9f) | 0x2;

			tun_write_reg(d, 0x24, (u16)(lo6 | EN_LO | (u16)DIB0090_CFG_USE_PWM_AGC * EN_CRYSTAL));
		}

		d->t_current_rf = d->t_rf_request;
		d->t_current_standard = (u8)c->delivery_system;

		ret = 20;
		d->t_calibrate = CAPTRIM_CAL; /* captrim search now */
		break;
	}

	case TUNER_STEP0: {
		/* because of the captrim calibration, this step must stay the one that follows the captrim state machine */
		int index = 0;
		const struct wbd_slope *wbd;
		u16 gain = 4, i = 3;

		while (d->t_current_rf / 1000 > d->t_current_wbd_table[index].max_freq)
			index++;
		wbd = &d->t_current_wbd_table[index];

		tun_write_reg(d, 0x1e, 0x07ff);

		if (wbd->wbd_gain != 0)
			gain = wbd->wbd_gain;

		d->t_wbdmux = (u16)(gain << 13 | i << 11 | (WBD_MUX | (u16)DIB0090_CFG_USE_PWM_AGC << 1));
		tun_write_reg(d, 0x10, d->t_wbdmux);

		if (tune->tuner_enable == EN_CAB && d->t_id_p1g) {
			/* P1G: the cable band is selected */
			tun_write_reg(d, 0x09, tune->lna_bias);
			tun_write_reg(d, 0x0b, (u16)(0xb800 | (u16)tune->lna_tune << 6 | tune->switch_trim));
		} else {
			tun_write_reg(d, 0x09, (u16)((u16)tune->lna_tune << 5 | tune->lna_bias));
		}

		tun_write_reg(d, 0x0c, tune->v2i);
		tun_write_reg(d, 0x0d, tune->mix);
		tun_write_reg(d, 0x0e, tune->load);
		d->t_tune_state = TUNER_STEP1;
		break;
	}

	case TUNER_STEP1:
		/* initialize the lt gain register */
		d->t_rf_lt_def = 0x7c00;

		tun_set_bandwidth(d);
		d->t_tuner_is_tuned = 1;

		d->t_calibrate |= WBD_CAL;
		d->t_calibrate |= TEMP_CAL;
		d->t_tune_state = TUNER_STOP;
		break;

	default:
		ret = CALLBACK_TIME_NEVER;
		break;
	}
	return ret;
}

/* dib0090_set_params: the tune state machine run to its end */
static int tun_set_params(struct torne_dib *d)
{
	d->t_tune_state = TUNER_START;

	for (;;) {
		u32 ret = tun_tune(d);

		if (ret == CALLBACK_TIME_NEVER || d->err)
			break;

		/* dib0090_tune counts in 0.1ms, but Linux sleeps in whole jiffies and found that anything below 10ms can keep
		 * the tuner from locking; so it rounds up to 10ms steps */
		ret = 10 * (ret + 99) / 100;
		msleep_(d, ret);
		if (d->t_tune_state == TUNER_STOP)
			break;
	}
	return d->err;
}

/* ================= board glue (board.go) ================= */

/* dib8090_compute_pll_parameters: of the PLL ratios 17 to 20, the one whose ADC clock harmonics fall furthest from the channel */
static u8 dib8090_compute_pll_parameters(u32 frequency_hz)
{
	u8 optimal_pll_ratio = 20, pll_ratio;
	u32 max = 0;

	for (pll_ratio = 17; pll_ratio <= 20; pll_ratio++) {
		u32 freq_adc = (u32)(12 * (int)pll_ratio * (1 << 8) / 16);
		u32 ratio = ((frequency_hz / 1000) * (1 << 8) / 1000) / freq_adc;
		u32 rest = ((frequency_hz / 1000) * (1 << 8) / 1000) - ratio * freq_adc;

		if (rest > freq_adc / 2)
			rest = freq_adc - rest;
		if (rest > max && rest > 717) {
			optimal_pll_ratio = pll_ratio;
			max = rest;
		}
	}

	return optimal_pll_ratio;
}

/* dib8096_set_param_override, installed as the tuner's set_params */
static int set_param_override(struct torne_dib *d)
{
	struct dib_channel *c = &d->c;
	u8 band = band_of_frequency(c->frequency / 1000);
	u8 pll_ratio;
	u32 timf;

	switch (band) {
	case BAND_UHF:
		dib_set_gpio(d, 3, 0, 0);
		break;
	default:
		/* Linux warns that the frequency is out of the supported range and uses the VHF switch */
		dib_set_gpio(d, 3, 0, 1);
		break;
	}

	{
		int rc = tun_set_params(d);

		if (rc)
			return rc;
	}

	if (c->bandwidth_hz != 6000000) {
		LOG(d, TORNE_LOG_ERR, "DiB8096: only 6MHz bandwidth is supported");
		return -EINVAL;
	}

	/* update PLL if needed ratio */
	update_pll(d, &dib8090_pll_config_12mhz, c->bandwidth_hz / 1000, 0);

	/* get optimize PLL ratio to remove spurious */
	pll_ratio = dib8090_compute_pll_parameters(c->frequency);
	switch (pll_ratio) {
	case 17:
		timf = 21387946;
		break;
	case 18:
		timf = 20199727;
		break;
	case 19:
		timf = 19136583;
		break;
	default:
		timf = 18179756;
		break;
	}

	/* update ratio */
	update_pll(d, &dib8090_pll_config_12mhz, c->bandwidth_hz / 1000, pll_ratio);

	ctrl_timf(d, TIMF_SET, timf);

	if (band != BAND_CBAND) {
		/* dib0090_get_wbd_target is returning any possible temperature compensated wbd-target */
		u16 target = (u16)(((int)tun_get_wbd_target(d) * 8 * 18 / 33 + 1) / 2);

		set_wbd_ref(d, target);
	}

	if (band == BAND_CBAND) {
		/* tuning in CBAND - soft-AGC startup */
		d->t_tune_state = AGC_START;
		for (;;) {
			int wait = tun_gain_control(d);

			msleep_(d, wait);
			if (d->err)
				return d->err;
			switch (d->t_tune_state) {
			case AGC_STEP0:
				dib_set_gpio(d, 6, 0, 1);
				break;
			case AGC_STEP1:
				/* activate the external attenuator in case of very high input power */
				if ((u16)d->t_rf_gain_limit < 2000)
					dib_set_gpio(d, 6, 0, 0);
				break;
			}
			if (d->t_tune_state >= AGC_STOP)
				break;
		}

		/* switching to PWM AGC */
		tun_pwm_gain_reset(d);
		pwm_agc_reset(d);
		d->tune_state = DEMOD_START;
	} else {
		/* for everything else than CBAND we are using standard AGC */
		tun_pwm_gain_reset(d);
	}

	return d->err;
}

/* dib8000_set_frontend for a single frontend: tune the tuner, start the AGC, then run the demodulator's state
 * machine to the end, waiting between its steps as MIN_TUNE_STEP_MS says */
static int dib_set_frontend(struct torne_dib *d)
{
	struct dib_channel *c = &d->c;
	u64 give_up;
	bool active = true;
	int rc;

	if (c->frequency == 0) {
		LOG(d, TORNE_LOG_ERR, "DiB8000: no frequency to tune to");
		return -EINVAL;
	}

	if (c->bandwidth_hz == 0)
		c->bandwidth_hz = 6000000;

	c->delivery_system = SYSTEM_ISDBT;

	/* set output mode and diversity input */
	set_diversity_in(d, 1);
	rc = dib_set_output_mode(d, OUTMODE_HIGH_Z);
	if (rc)
		return rc;

	/* tune the tuner */
	rc = set_param_override(d);
	if (rc)
		return rc;

	d->tune_state = AGC_START;

	/* turn off the diversity of the last chip */
	set_diversity_in(d, 0);

	/* start up the AGC */
	for (;;) {
		int wait = agc_startup(d);

		if (wait == 0 || d->err)
			break;

		/* agc_startup counts in 0.1ms, but Linux sleeps in whole jiffies; round up to 10ms steps */
		wait = 10 * (wait + 99) / 100;
		msleep_(d, wait);
		if (d->tune_state == AGC_STOP)
			break;
	}
	if (d->err)
		return d->err;

	d->tune_state = DEMOD_START;

	give_up = now_ms(d) + DEFAULT_TUNE_TIMEOUT_MS;
	while (active) {
		u64 callback_ms = 0;
		bool have_callback = false;
		int delay = dib_tune(d);

		if (delay != 0) {
			callback_ms = now_ms(d) + ((u64)delay * 100 + 999) / 1000;
			have_callback = true;
		}

		/* we are in autosearch */
		if (d->channel_parameters_set == 0) {
			if (d->status == STATUS_DEMOD_SUCCESS || d->status == STATUS_FFT_SUCCESS) {
				/* read the channel parameters from the frontend that found them */
				get_frontend(d);
				d->channel_parameters_set = 1;
			}
		}

		/* tuning is done when the frontend is done, failed or locked */
		if (d->status == STATUS_TUNE_FAILED || d->status == STATUS_LOCKED || d->status == STATUS_DATA_LOCKED)
			active = d->tune_state != DEMOD_STOP;

		if (active && !have_callback) /* Linux: "strange callback time something went wrong" */
			active = false;

		if (active) {
			u64 now = now_ms(d);
			u64 wait = callback_ms > now ? callback_ms - now : 0;

			if (wait < MIN_TUNE_STEP_MS)
				wait = MIN_TUNE_STEP_MS;
			msleep_(d, (unsigned int)wait);
		}

		if (d->err)
			return d->err;
		/* Linux has no bound here; a chip that never raises its search interrupt would hold it forever */
		if (active && now_ms(d) > give_up) {
			LOG(d, TORNE_LOG_ERR, "DiB8000: tune did not finish within %d ms (state %d)", DEFAULT_TUNE_TIMEOUT_MS,
			    d->tune_state);
			return -ETIMEDOUT;
		}
	}

	/* set output mode */
	return dib_set_output_mode(d, d->cfg_output_mode);
}

/* ================= public API ================= */

void torne_dib_setup(struct torne_dib *d, struct torne_bridge *br)
{
	memset(d, 0, sizeof(*d));
	d->br = br;
	d->cfg_output_mode = DIB8000_CFG_OUTPUT_MODE;
	d->cfg_gpio_dir = DIB8000_CFG_GPIO_DIR;
	d->cfg_gpio_val = DIB8000_CFG_GPIO_VAL;
}

int torne_dib_init(struct torne_dib *d)
{
	int rc;

	d->err = 0;
	/* the DiB8000 wakes up needing its I2C address confirmed and its high-speed interface left alone, which is what
	 * register 1287 = 3 does in DiBcom's enumeration; with one demodulator the default address stays */
	{
		static const u8 enumerate[4] = { 0x05, 0x07, 0x00, 0x03 };

		rc = torne_i2c_write(d->br, TORNE_DIB_I2C_ADDRESS, enumerate, 4);
		if (rc)
			return rc;
	}

	/* dib8000_init */
	rc = dib_initialize(d);
	if (rc)
		return rc;

	/* dib0090_register */
	rc = register_tuner(d);
	if (rc)
		return rc;

	/* the frontend and tuner init a DVB frontend runs when it is opened */
	dib_wakeup(d);
	tun_wakeup(d);
	d->asleep = false;

	rc = dib_set_output_mode(d, d->cfg_output_mode);
	if (rc)
		return rc;
	return d->err;
}

static int mode_to_outmode(enum torne_dib_output mode)
{
	switch (mode) {
	case TORNE_OUT_HIGHZ:
		return OUTMODE_HIGH_Z;
	case TORNE_OUT_PARALLEL_GATED:
		return OUTMODE_PARALLEL_GATED;
	case TORNE_OUT_PARALLEL_CONT:
		return OUTMODE_PARALLEL_CONT;
	case TORNE_OUT_SERIAL:
		return OUTMODE_SERIAL;
	case TORNE_OUT_FIFO:
		return OUTMODE_FIFO;
	}
	return -1;
}

/* sets the transport stream output and makes it the mode each tune ends in (a tune holds it in high impedance) */
int torne_dib_set_output(struct torne_dib *d, enum torne_dib_output mode)
{
	int outmode = mode_to_outmode(mode);

	d->err = 0;
	if (outmode < 0)
		return -EINVAL;
	d->cfg_output_mode = outmode;
	return dib_set_output_mode(d, outmode);
}

/* drives one of the DiB8000's GPIO pins as an output (pins 0 and 5 belong to the tuner, 3 to its band switch) */
int torne_dib_set_gpio(struct torne_dib *d, u8 pin, bool high)
{
	d->err = 0;
	dib_set_gpio(d, pin, 0, (u8)bw(high));
	return d->err;
}

int torne_dib_tune(struct torne_dib *d, u32 frequency_hz)
{
	d->err = 0;

	if (frequency_hz < MIN_FREQUENCY_HZ || frequency_hz > MAX_FREQUENCY_HZ) {
		LOG(d, TORNE_LOG_ERR, "DiB8096: %u Hz is outside %u-%u Hz", frequency_hz, MIN_FREQUENCY_HZ, MAX_FREQUENCY_HZ);
		return -EINVAL;
	}
	if (d->asleep) {
		dib_wakeup(d);
		tun_wakeup(d);
		d->asleep = false;
	}

	/* a DVBv5 tune from a frequency alone starts from a cleared cache */
	memset(&d->c, 0, sizeof(d->c));
	d->c.delivery_system = SYSTEM_ISDBT;
	d->c.transmission_mode = TRANSMISSION_AUTO;
	d->c.guard_interval = GUARD_AUTO;
	d->c.inversion = INVERSION_AUTO;
	d->c.isdbt_layer_enabled = 7;
	{
		int i;

		for (i = 0; i < 3; i++) {
			d->c.layer[i].fec = FEC_AUTO;
			d->c.layer[i].modulation = MODULATION_QAM_AUTO;
		}
	}
	d->c.frequency = frequency_hz;

	return dib_set_frontend(d) ?: d->err;
}

/* linear interpolation in a descending table (interpolate_value) */
static u32 interpolate_value(u32 value, const struct linear_segment *segments, int length)
{
	int i;
	s32 dy;
	u32 dx;
	u64 tmp64;

	if (value >= segments[0].x)
		return (u32)segments[0].y;
	if (value < segments[length - 1].x)
		return (u32)segments[length - 1].y;

	for (i = 1; i < length - 1; i++) {
		/* if value is identical, no need to interpolate */
		if (value == segments[i].x)
			return (u32)segments[i].y;
		if (value > segments[i].x)
			break;
	}

	/* linear interpolation between the two (x,y) points */
	dy = segments[i - 1].y - segments[i].y;
	dx = segments[i - 1].x - segments[i].x;

	tmp64 = (u64)(value - segments[i].x);
	tmp64 *= (u64)(s64)dy;
	tmp64 = div_u64_(tmp64, dx);
	return (u32)((s32)segments[i].y + (s32)tmp64);
}

/* 16.16 fixed point signal over noise, or 0xffffffff when there is no noise reading */
static u32 shl32(u32 value, u32 count)
{
	return value << (count & 31);
}

static u32 get_snr(struct torne_dib *d)
{
	u16 val = dib_read_word(d, 542);
	u32 n = (u32)(val >> 6) & 0xff;
	u32 exp = (u32)(val & 0x3f);
	u32 s;

	if (exp & 0x20)
		exp -= 0x40;
	n = shl32(n, exp + 16);

	val = dib_read_word(d, 543);
	s = (u32)(val >> 6) & 0xff;
	exp = (u32)(val & 0x3f);
	if (exp & 0x20)
		exp -= 0x40;
	s = shl32(s, exp + 16);

	if (n > 0) {
		u32 t = (s / n) << 16;

		return t + ((s << 16) - n * t) / n;
	}
	return 0xffffffff;
}

/* 10 * log10(x) in 0.01 dB units for x >= 1, computed with integers: log2 by bit position plus iterated squaring of the mantissa */
static int db_x100(u32 x)
{
	u32 int_part = 0, mant;
	u64 frac = 0, log2_fp, v;
	int k;

	if (x == 0)
		return 0;
	while ((x >> int_part) > 1)
		int_part++;
	/* mantissa in [1, 2) as a 1.31 fixed point value */
	mant = int_part >= 31 ? x >> (int_part - 31) : x << (31 - int_part);
	v = mant;
	for (k = 0; k < 16; k++) {
		v = (v * v) >> 31;
		frac <<= 1;
		if (v >= ((u64)1 << 32)) {
			frac |= 1;
			v >>= 1;
		}
	}
	log2_fp = ((u64)int_part << 16) | frac; /* log2(x) in 16.16 */
	/* 10 * log10(x) * 100 = 1000 * log2(x) * log10(2) = log2(x) * 301.02999566 */
	return (int)(((log2_fp * 30103) / 100) >> 16);
}

int torne_dib_status(struct torne_dib *d, struct torne_dib_status *st)
{
	int stat;
	u16 strength;
	int db;

	d->err = 0;
	stat = read_status_bits(d);

	memset(st, 0, sizeof(*st));
	st->status = stat;
	st->locked = (stat & FE_HAS_LOCK) != 0;

	/* signal strength, in thousandths of a dBm: the inverted global AGC value */
	strength = (u16)(65535 - dib_read_word(d, 390));
	db = (int)interpolate_value(strength, strength_to_db_table, sizeof(strength_to_db_table) / sizeof(strength_to_db_table[0])) - 131000;
	st->strength_dbm_x1000 = db;
	st->has_strength = true;

	/* CNR requires lock. Linux takes 10*intlog10 of the 16.16 SNR's integer part. */
	if (st->locked) {
		u32 snr = get_snr(d) >> 16;

		if (snr != 0)
			st->cnr_x100 = db_x100(snr);
		st->has_cnr = true;
	}

	return d->err;
}

/* puts the tuner to sleep, then the demodulator (output high impedance, only the interface powered, ADCs off) */
int torne_dib_sleep(struct torne_dib *d)
{
	d->err = 0;
	tun_sleep_tuner(d);
	{
		int rc = dib_sleep_chip(d);

		if (rc)
			return rc;
	}
	d->asleep = true;
	return d->err;
}
