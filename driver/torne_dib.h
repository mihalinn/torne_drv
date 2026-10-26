/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * torne_dib.h - DiBcom DiB8096 (DiB8000 ISDB-T demodulator + DiB0090 tuner) of the CECH-ZD1 J
 *
 * A port of coax's dib8096 package (Till0196), itself a port of the Linux dib8000, dib0090 and
 * dibx000_common drivers (Copyright (C) 2005-9 DiBcom, SA et al), restricted to 13-segment ISDB-T at 6 MHz.
 */
#ifndef TORNE_DIB_H
#define TORNE_DIB_H

#include "torne_core.h"

#define TORNE_DIB_I2C_ADDRESS 0x12

enum torne_dib_output {
	TORNE_OUT_HIGHZ = 0,
	TORNE_OUT_PARALLEL_GATED,
	TORNE_OUT_PARALLEL_CONT,
	TORNE_OUT_SERIAL,
	TORNE_OUT_FIFO,
};

/* FE_HAS_* bits of torne_dib_status.status */
#define TORNE_FE_HAS_SIGNAL 0x01
#define TORNE_FE_HAS_CARRIER 0x02
#define TORNE_FE_HAS_VITERBI 0x04
#define TORNE_FE_HAS_SYNC 0x08
#define TORNE_FE_HAS_LOCK 0x10

struct torne_dib_status {
	bool locked;
	int status;		 /* TORNE_FE_HAS_* bits */
	bool has_cnr;
	int cnr_x100;		 /* carrier to noise ratio in 0.01 dB units, valid once locked */
	bool has_strength;
	int strength_dbm_x1000;	 /* thousandths of a dBm, an estimate */
};

/* ---- internal types (public so that the state can be allocated by the caller) ---- */

struct dib_layer {
	int fec;
	int modulation;
	u8 interleaving;
	u8 segment_count;
};

struct dib_channel {
	int delivery_system;
	u32 frequency; /* Hz */
	u32 bandwidth_hz;
	int inversion;
	int transmission_mode;
	int guard_interval;
	u8 isdbt_partial_reception;
	u8 isdbt_sb_mode;
	u8 isdbt_layer_enabled;
	struct dib_layer layer[3];
};

struct agc_config {
	u8 band_caps;
	u16 setup;
	u16 inv_gain;
	u16 time_stabiliz;
	u8 alpha_level;
	u16 thlock;
	u8 wbd_inv;
	u16 wbd_ref;
	u8 wbd_sel;
	u8 wbd_alpha;
	u16 agc1_max, agc1_min, agc2_max, agc2_min;
	u8 agc1_pt1, agc1_pt2, agc1_pt3;
	u8 agc1_slope1, agc1_slope2;
	u8 agc2_pt1, agc2_pt2;
	u8 agc2_slope1, agc2_slope2;
	u8 alpha_mant, alpha_exp;
	u8 beta_mant, beta_exp;
	u8 perform_agc_softsplit;
	struct {
		u16 min, max, min_thres, max_thres;
	} split;
};

struct bandwidth_config {
	u32 internal;
	u32 sampling;
	u8 pll_prediv, pll_ratio, pll_range, pll_reset, pll_bypass;
	u8 enable_refdiv, bypclk_div, io_clk_en_core, adc_clk_src, modulo;
	u16 sad_cfg;
	u32 ifreq;
	u32 timf;
	u32 xtal_hz;
};

struct wbd_slope {
	u16 max_freq;
	u16 slope_cold, offset_cold, slope_hot, offset_hot;
	u8 wbd_gain;
};

struct tuning_entry {
	u32 max_freq;
	u8 switch_trim;
	u8 lna_tune;
	u16 lna_bias;
	u16 v2i;
	u16 mix;
	u16 load;
	u16 tuner_enable;
};

struct pll_entry {
	u32 max_freq;
	u8 vco_band, hfdiv_code, hfdiv, topresc;
};

struct dc_calibration {
	u8 addr, offset, pga;
	u16 bb1;
	u8 i;
};

struct linear_segment {
	u32 x;
	s32 y;
};

struct dib_master {
	u16 base_register;
	u16 selected_interface;
	u8 gate; /* 8-bit address the gate is open to, 0 when closed */
};

struct torne_dib {
	struct torne_bridge *br;
	int err; /* first bus error of an operation; later accesses are skipped */
	struct dib_channel c;
	bool asleep;

	/* ---- demodulator (struct dib8000_state) ---- */
	struct dib_master master;
	u16 wbd_ref;
	u8 current_band;
	const struct agc_config *current_agc;
	u32 timf, timf_default;
	u8 differential_constellation;
	int diversity_onoff;
	u16 revision;
	u8 isdbt_cfg_loaded;
	int tune_state;
	s32 status;
	u16 current_demod_bw;
	u16 seg_mask, seg_diff_mask, mode;
	u8 layer_b_nb_seg, layer_c_nb_seg;
	int channel_parameters_set;
	u16 autosearch_state;
	int found_nfft, found_guard;
	u8 symbol_duration;
	u64 timeout_ms;
	u8 longest_intl_layer;
	int output_mode;
	u16 cfg_gpio_dir, cfg_gpio_val;
	int cfg_output_mode;

	/* ---- tuner (struct dib0090_state) ---- */
	u8 t_current_band;
	int t_tune_state;
	u32 t_current_rf; /* kHz */
	u16 t_wbd_offset;
	s16 t_wbd_target;
	s16 t_rf_gain_limit;
	s16 t_current_gain;
	u8 t_agc_step;
	u8 t_agc_freeze;
	u16 t_gain[2];
	u16 t_gain_reg[4];
	const u16 *t_rf_ramp;
	const u16 *t_bb_ramp;
	u16 t_bb1_def;
	u16 t_rf_lt_def;
	s8 t_step;
	s16 t_adc_diff;
	s16 t_min_adc_diff;
	s8 t_captrim;
	s8 t_fcaptrim;
	const struct dc_calibration *t_dc;
	int t_dc_next;
	u16 t_bb6, t_bb7;
	const struct tuning_entry *t_current_tune_table;
	const struct pll_entry *t_current_pll_table;
	u8 t_tuner_is_tuned;
	u8 t_id_version, t_id_product, t_id_p1g, t_id_in_soc;
	u32 t_rf_request;
	u8 t_current_standard;
	u8 t_calibrate;
	u32 t_rest;
	u16 t_bias;
	s16 t_temperature;
	u8 t_wbd_calibration_gain;
	const struct wbd_slope *t_current_wbd_table;
	u16 t_wbdmux;
};

void torne_dib_setup(struct torne_dib *d, struct torne_bridge *br);
/* identifies and initializes the chips and leaves the TS output in the configured mode (dib8000_init + dib0090_register) */
int torne_dib_init(struct torne_dib *d);
int torne_dib_set_output(struct torne_dib *d, enum torne_dib_output mode);
int torne_dib_set_gpio(struct torne_dib *d, u8 pin, bool high);
/* tunes to the 6 MHz ISDB-T channel centered on frequency_hz and runs the demodulator's search to the end */
int torne_dib_tune(struct torne_dib *d, u32 frequency_hz);
int torne_dib_status(struct torne_dib *d, struct torne_dib_status *st);
int torne_dib_sleep(struct torne_dib *d);

#endif /* TORNE_DIB_H */
