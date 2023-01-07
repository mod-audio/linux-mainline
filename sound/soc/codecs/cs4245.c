// SPDX-License-Identifier: GPL-2.0-only
/*
 * cs4245.c -- CS4245 ALSA SoC audio driver
 *
 * Based on work from:
 * - Paul Handrigan <paul.handrigan@cirrus.com>
 * - Felipe Correa da Silva Sanches <juca@members.fsf.org>
 * - Rafael Guayer
 *
 * Author: Filipe Coelho <falktx@falktx.com>
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include "cs4245.h"

//----------------------------------------------------------------------

// #define DEBUG_CS4245

#ifdef  __MOD_DEVICES__
// GPIO macros
#define CHANNEL_LEFT    0
#define CHANNEL_RIGHT   1

#define GPIO_BYPASS     0
#define GPIO_PROCESS    1

static int headphone_volume = 0; // Headphone volume has a total of 16 steps, each corresponds to 3dB. Step 11 is 0dB.
static int input_left_gain_stage = 0;
static int input_right_gain_stage = 0;
static bool left_true_bypass = true;
static bool right_true_bypass = true;

static struct _modduo_gpios {
	struct gpio_desc *headphone_clk;
	struct gpio_desc *headphone_dir;
	struct gpio_desc *gain_stage_left1;
	struct gpio_desc *gain_stage_left2;
	struct gpio_desc *gain_stage_right1;
	struct gpio_desc *gain_stage_right2;
	struct gpio_desc *true_bypass_left;
	struct gpio_desc *true_bypass_right;
	bool initialized;
} *modduo_gpios;

//----------------------------------------------------------------------

static void enable_cpu_counters(void* data)
{
    struct i2c_client *i2c_client = (struct i2c_client*)data;
    dev_info(&i2c_client->dev, "[MOD Duo PerfCounter] enabling user-mode PMU access on CPU #%d\n", smp_processor_id());

    /* Enable user-mode access to counters. */
    asm volatile("mcr p15, 0, %0, c9, c14, 0\n\t" :: "r"(1));
    /* disable counter overflow interrupts (just in case)*/
    asm volatile("mcr p15, 0, %0, c9, c14, 2\n\t" :: "r"(0x8000000f));
    /* Program PMU and enable all counters */
    asm volatile("mcr p15, 0, %0, c9, c12, 0\t\n" :: "r"((31))); // 1|2|4|8|16
    asm volatile("mcr p15, 0, %0, c9, c12, 1\t\n" :: "r"(0x8000000f));
    /* Clear overflows */
    asm volatile("mcr p15, 0, %0, c9, c12, 3\t\n" :: "r"(0x8000000f));
}

static int modduo_init(struct i2c_client *i2c_client)
{
	int i;

	modduo_gpios = devm_kzalloc(&i2c_client->dev, sizeof(struct _modduo_gpios), GFP_KERNEL);
	if (modduo_gpios == NULL)
		return -ENOMEM;

	modduo_gpios->headphone_clk     = devm_gpiod_get(&i2c_client->dev, "headphone_clk",     GPIOD_OUT_HIGH);
	modduo_gpios->headphone_dir     = devm_gpiod_get(&i2c_client->dev, "headphone_dir",     GPIOD_OUT_HIGH);
	modduo_gpios->gain_stage_left1  = devm_gpiod_get(&i2c_client->dev, "gain_stage_left1",  GPIOD_OUT_HIGH);
	modduo_gpios->gain_stage_left2  = devm_gpiod_get(&i2c_client->dev, "gain_stage_left2",  GPIOD_OUT_HIGH);
	modduo_gpios->gain_stage_right1 = devm_gpiod_get(&i2c_client->dev, "gain_stage_right1", GPIOD_OUT_HIGH);
	modduo_gpios->gain_stage_right2 = devm_gpiod_get(&i2c_client->dev, "gain_stage_right2", GPIOD_OUT_HIGH);

	// bypass is inverted
	modduo_gpios->true_bypass_left  = devm_gpiod_get(&i2c_client->dev, "true_bypass_left",  GPIOD_OUT_LOW);
	modduo_gpios->true_bypass_right = devm_gpiod_get(&i2c_client->dev, "true_bypass_right", GPIOD_OUT_LOW);

	if (IS_ERR(modduo_gpios->headphone_clk) || !modduo_gpios->headphone_clk) {
		modduo_gpios->initialized = false;
		return 0;
	}

	// put headphone volume to lowest setting, so we know where we are
	gpiod_set_value(modduo_gpios->headphone_dir, 0);

	for (i=0; i < 16; i++) {
		// toggle clock in order to sample the volume pin upon clock's rising edge
		gpiod_set_value(modduo_gpios->headphone_clk, 1);
		gpiod_set_value(modduo_gpios->headphone_clk, 0);
	}

	// initialize gpios
	// FIXME does this mean lowest gain stage? need to confirm
	gpiod_set_value(modduo_gpios->gain_stage_left1, 1);
	gpiod_set_value(modduo_gpios->gain_stage_left2, 1);
	gpiod_set_value(modduo_gpios->gain_stage_right1, 1);
	gpiod_set_value(modduo_gpios->gain_stage_right2, 1);

	modduo_gpios->initialized = true;

	on_each_cpu(enable_cpu_counters, i2c_client, 1);

	return 0;
}

/* This routine flips the GPIO pins to send the volume adjustment
   message to the actual headphone gain-control chip (LM4811) */
static void set_headphone_volume(int new_volume)
{
	int i;
	int steps = abs(new_volume - headphone_volume);

	if (modduo_gpios->initialized) {
		// select volume adjustment direction
		gpiod_set_value(modduo_gpios->headphone_dir, new_volume > headphone_volume ? 1 : 0);

		for (i=0; i < steps; i++) {
			// toggle clock in order to sample the volume pin upon clock's rising edge
			gpiod_set_value(modduo_gpios->headphone_clk, 1);
			gpiod_set_value(modduo_gpios->headphone_clk, 0);
		}
	}

	headphone_volume = new_volume;
}

static void set_gain_stage(int channel, int state)
{
	struct gpio_desc *gpio1, *gpio2;

	switch (channel) {
	case CHANNEL_LEFT:
		gpio1 = modduo_gpios->gain_stage_left1;
		gpio2 = modduo_gpios->gain_stage_left2;
		input_left_gain_stage = state;
		break;
	case CHANNEL_RIGHT:
		gpio1 = modduo_gpios->gain_stage_right1;
		gpio2 = modduo_gpios->gain_stage_right2;
		input_right_gain_stage = state;
		break;
	default:
		return;
	}

	if (!modduo_gpios->initialized)
		return;

	switch (state) {
	case 0:
		gpiod_set_value(gpio1, 1);
		gpiod_set_value(gpio2, 1);
		break;
	case 1:
		gpiod_set_value(gpio1, 1);
		gpiod_set_value(gpio2, 0);
		break;
	case 2:
		gpiod_set_value(gpio1, 0);
		gpiod_set_value(gpio2, 1);
		break;
	case 3:
		gpiod_set_value(gpio1, 0);
		gpiod_set_value(gpio2, 0);
		break;
	}
}

/* state == bypass:
 * No audio processing.
 * Input is connected directly to output, bypassing the codec.
 *
 * state == process:
 * INPUT => CODEC => OUTPUT
 */
static void set_true_bypass(int channel, bool state)
{
	switch (channel) {
	case CHANNEL_LEFT:
		if (modduo_gpios->initialized)
			gpiod_set_value(modduo_gpios->true_bypass_left, state ? GPIO_BYPASS : GPIO_PROCESS);
		left_true_bypass = state;
		break;
	case CHANNEL_RIGHT:
		if (modduo_gpios->initialized)
			gpiod_set_value(modduo_gpios->true_bypass_right, state ? GPIO_BYPASS : GPIO_PROCESS);
		right_true_bypass = state;
		break;
	}
}

//----------------------------------------------------------------------

static int headphone_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 15;
	return 0;
}

static int headphone_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = headphone_volume;
	return 0;
}

static int headphone_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (headphone_volume != ucontrol->value.integer.value[0]) {
		set_headphone_volume(ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}

//----------------------------------------------------------------------

static int input_gain_stage_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 3;
	return 0;
}

static int input_left_gain_stage_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = input_left_gain_stage;
	return 0;
}

static int input_right_gain_stage_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = input_right_gain_stage;
	return 0;
}

static int input_left_gain_stage_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (input_left_gain_stage != ucontrol->value.integer.value[0]) {
		set_gain_stage(CHANNEL_LEFT, ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}

static int input_right_gain_stage_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (input_right_gain_stage != ucontrol->value.integer.value[0]) {
		set_gain_stage(CHANNEL_RIGHT, ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}

//----------------------------------------------------------------------

static int true_bypass_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int left_true_bypass_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = left_true_bypass;
	return 0;
}

static int right_true_bypass_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = right_true_bypass;
	return 0;
}

static int left_true_bypass_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (left_true_bypass != ucontrol->value.integer.value[0]) {
		set_true_bypass(CHANNEL_LEFT, ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}

static int right_true_bypass_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (right_true_bypass != ucontrol->value.integer.value[0]) {
		set_true_bypass(CHANNEL_RIGHT, ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}

#endif // __MOD_DEVICES__

//----------------------------------------------------------------------

struct cs4245_private {
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	u8 format;
	u32 sysclk;
};

static const struct reg_default cs4245_reg_defaults[] = {
	{ CS4245_PWRCTL, 0x0F },
	{ CS4245_DAC_CTL, 0x08 },
	{ CS4245_ADC_CTL, 0x00 },
	{ CS4245_MCLK_FREQ, 0x00 },
	{ CS4245_SIG_SEL, 0x40 },
	{ CS4245_CHB_PGA_CTL, 0x00 },
	{ CS4245_CHA_PGA_CTL, 0x00 },
	{ CS4245_ADC_CTL2, 0x19 },
	{ CS4245_DAC_CHB_VOL, 0x00 },
	{ CS4245_DAC_CHA_VOL, 0x00 },
	{ CS4245_DAC_CTL2, 0xC0 },
	{ CS4245_INT_STATUS, 0x00 },
	{ CS4245_INT_MASK, 0x00 },
	{ CS4245_STATUS_MODE_MSB, 0x00 },
	{ CS4245_STATUS_MODE_LSB, 0x00 },
};

#ifdef DEBUG_CS4245
static void cs4245_printk_register_values(struct cs4245_private *cs4245, const char* const where)
{
	int reg_val[12];

	if (regmap_read(cs4245->regmap, CS4245_CHIP_ID, reg_val +  0) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_PWRCTL, reg_val +  1) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_DAC_CTL, reg_val +  2) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_ADC_CTL, reg_val +  3) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_MCLK_FREQ, reg_val +  4) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_SIG_SEL, reg_val +  5) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_CHB_PGA_CTL, reg_val +  6) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_CHA_PGA_CTL, reg_val +  7) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_ADC_CTL2, reg_val +  8) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_DAC_CHB_VOL, reg_val +  9) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_DAC_CHA_VOL, reg_val + 10) == 0 &&
	    regmap_read(cs4245->regmap, CS4245_DAC_CTL2, reg_val + 11) == 0)
	{
		printk("[CS4245] %s | Register Values:\n", where);
		printk("[CS4245] %s | CHIP ID: 0x%X.\n", where, reg_val[0]);
		printk("[CS4245] %s | POWER CTRL: 0x%X.\n", where, reg_val[1]);
		printk("[CS4245] %s | DAC CTRL 1: 0x%X.\n", where, reg_val[2]);
		printk("[CS4245] %s | ADC CTRL: 0x%X.\n", where, reg_val[3]);
		printk("[CS4245] %s | MCLK FREQ: 0x%X.\n", where, reg_val[4]);
		printk("[CS4245] %s | SIGNAL SEL: 0x%X.\n", where, reg_val[5]);
		printk("[CS4245] %s | PGA B CTRL: 0x%X.\n", where, reg_val[6]);
		printk("[CS4245] %s | PGA A CTRL: 0x%X.\n", where, reg_val[7]);
		printk("[CS4245] %s | ANALOG IN: 0x%X.\n", where, reg_val[8]);
		printk("[CS4245] %s | DAC B CTRL: 0x%X.\n", where, reg_val[9]);
		printk("[CS4245] %s | DAC A CTRL: 0x%X.\n", where, reg_val[10]);
		printk("[CS4245] %s | DAC CTRL 2: 0x%X.\n", where, reg_val[11]);
	}
	else
	{
		printk("[CS4245] %s | registers fail\n", where);
	}

	return;
}
#endif

static bool cs4245_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS4245_CHIP_ID ... CS4245_MAX_REGISTER:
		return true;
	default:
		return false;
	}
}

static bool cs4245_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS4245_INT_STATUS:
		return true;
	default:
		return false;
	}
}

static DECLARE_TLV_DB_SCALE(pga_tlv, -1200, 50, 0);

static DECLARE_TLV_DB_SCALE(dac_tlv, -12750, 50, 0);

#ifdef __MOD_DEVICES__
static const unsigned int gain_stages_tlv[] = {
    TLV_DB_RANGE_HEAD(4),
    0, 0, TLV_DB_SCALE_ITEM(0,  0, 0),
    1, 1, TLV_DB_SCALE_ITEM(6.0, 0, 0),
    2, 2, TLV_DB_SCALE_ITEM(15.0, 0, 0),
    3, 3, TLV_DB_SCALE_ITEM(20.4, 0, 0),
};
#else
static const char * const digital_input_mux_text[] = {
	"SDIN1", "SDIN2"
};

static SOC_ENUM_SINGLE_DECL(digital_input_mux_enum, CS4245_SIG_SEL, 7,
		digital_input_mux_text);

static const struct snd_kcontrol_new digital_input_mux =
	SOC_DAPM_ENUM("Digital Input Mux", digital_input_mux_enum);

static const char * const mic_linein_text[] = {
	"MIC", "LINEIN"
};

static SOC_ENUM_SINGLE_DECL(mic_linein_enum, CS4245_ADC_CTL2, 0,
		mic_linein_text);

static const struct snd_kcontrol_new mic_linein_mux =
	SOC_DAPM_ENUM("ADC Input Capture Mux", mic_linein_enum);

static const struct snd_kcontrol_new loopback_ctl =
	SOC_DAPM_SINGLE("Switch", CS4245_SIG_SEL, 1, 1, 0);

static const struct snd_kcontrol_new dac_switch =
	SOC_DAPM_SINGLE("Switch", CS4245_PWRCTL, 1, 1, 0);
#endif

static const struct snd_kcontrol_new cs4245_snd_controls[] = {

	SOC_DOUBLE_R_TLV("DAC Volume", CS4245_DAC_CHA_VOL,
		      CS4245_DAC_CHB_VOL, 0, 0xFF, 1, dac_tlv),
	SOC_DOUBLE_R_SX_TLV("PGA Gain", CS4245_CHA_PGA_CTL,
			      CS4245_CHB_PGA_CTL, 0, 0x28, 0x30, pga_tlv),
#ifndef __MOD_DEVICES__
	SOC_SINGLE("De-emp 44.1kHz Switch", CS4245_DAC_CTL, 1,
				1, 0),
	SOC_SINGLE("DAC INV Switch", CS4245_DAC_CTL2, 5,
				1, 0),
	SOC_SINGLE("DAC Zero Cross Switch", CS4245_DAC_CTL2, 6,
				1, 0),
	SOC_SINGLE("DAC Soft Ramp Switch", CS4245_DAC_CTL2, 7,
				1, 0),
	SOC_SINGLE("ADC HPF Switch", CS4245_ADC_CTL, 1,
				1, 0),
	SOC_SINGLE("ADC Zero Cross Switch", CS4245_ADC_CTL2, 3,
				1, 1),
	SOC_SINGLE("ADC Soft Ramp Switch", CS4245_ADC_CTL2, 7,
				1, 0),
#else
	SOC_SINGLE("AUX OUT MUX", CS4245_SIG_SEL, 5, 3, 0),
	SOC_SINGLE("LOOPBACK", CS4245_SIG_SEL, 1, 1, 0),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Headphone Playback Volume",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = headphone_info,
		.get = headphone_get,
		.put = headphone_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Left Gain Stage",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = input_gain_stage_info,
		.get = input_left_gain_stage_get,
		.put = input_left_gain_stage_put,
		.tlv.p = gain_stages_tlv
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Right Gain Stage",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = input_gain_stage_info,
		.get = input_right_gain_stage_get,
		.put = input_right_gain_stage_put,
		.tlv.p = gain_stages_tlv
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Left True-Bypass",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = true_bypass_info,
		.get = left_true_bypass_get,
		.put = left_true_bypass_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Right True-Bypass",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = true_bypass_info,
		.get = right_true_bypass_get,
		.put = right_true_bypass_put
	},
#endif
};

#ifndef __MOD_DEVICES__
static const struct snd_soc_dapm_widget cs4245_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("LINEINL"),
	SND_SOC_DAPM_INPUT("LINEINR"),
	SND_SOC_DAPM_INPUT("MICL"),
	SND_SOC_DAPM_INPUT("MICR"),

	SND_SOC_DAPM_AIF_OUT("DOUT", NULL,  0,
			SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_MUX("ADC Mux", SND_SOC_NOPM, 0, 0, &mic_linein_mux),

	SND_SOC_DAPM_ADC("ADC", NULL, CS4245_PWRCTL, 2, 1),
	SND_SOC_DAPM_PGA("Pre-amp MIC", CS4245_PWRCTL, 3,
			1, NULL, 0),

	SND_SOC_DAPM_MUX("Input Mux", SND_SOC_NOPM,
			 0, 0, &digital_input_mux),

	SND_SOC_DAPM_MIXER("SDIN1 Input Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SDIN2 Input Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SWITCH("Loopback", SND_SOC_NOPM, 0, 0,
			&loopback_ctl),
	SND_SOC_DAPM_SWITCH("DAC", CS4245_PWRCTL, 1, 1,
			&dac_switch),

	SND_SOC_DAPM_AIF_IN("DIN1", NULL,  0,
			SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("DIN2", NULL,  0,
			SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_OUTPUT("LINEOUTL"),
	SND_SOC_DAPM_OUTPUT("LINEOUTR"),
};

static const struct snd_soc_dapm_route cs4245_audio_map[] = {
	{"DIN1", NULL, "DAI1 Playback"},
	{"DIN2", NULL, "DAI2 Playback"},
	{"SDIN1 Input Mixer", NULL, "DIN1"},
	{"SDIN2 Input Mixer", NULL, "DIN2"},
	{"Input Mux", "SDIN1", "SDIN1 Input Mixer"},
	{"Input Mux", "SDIN2", "SDIN2 Input Mixer"},
	{"DAC", "Switch", "Input Mux"},
	{"LINEOUTL", NULL, "DAC"},
	{"LINEOUTR", NULL, "DAC"},

	{"Pre-amp MIC", NULL, "MICL"},
	{"Pre-amp MIC", NULL, "MICR"},
	{"ADC Mux", "MIC", "Pre-amp MIC"},
	{"ADC Mux", "LINEIN", "LINEINL"},
	{"ADC Mux", "LINEIN", "LINEINR"},
	{"ADC", NULL, "ADC Mux"},
	{"DOUT", NULL, "ADC"},
	{"DAI1 Capture", NULL, "DOUT"},
	{"DAI2 Capture", NULL, "DOUT"},

	/* Loopback */
	{"Loopback", "Switch", "ADC"},
	{"DAC", NULL, "Loopback"},
};
#endif

struct cs4245_clk_para {
	u32 mclk;
	u32 rate;
	u8 fm_mode; /* values 1, 2, or 4 */
	u8 mclkdiv;
};

static const struct cs4245_clk_para clk_map_table[] = {
	/*32k*/
	{8192000, 32000, 0, 0},
	{12288000, 32000, 0, 1},
	{16384000, 32000, 0, 2},
	{24576000, 32000, 0, 3},
	{32768000, 32000, 0, 4},

	/*44.1k*/
	{11289600, 44100, 0, 0},
	{16934400, 44100, 0, 1},
	{22579200, 44100, 0, 2},
	{33868000, 44100, 0, 3},
	{45158400, 44100, 0, 4},

	/*48k*/
	{12288000, 48000, 0, 0},
	{18432000, 48000, 0, 1},
	{24576000, 48000, 0, 2},
	{36864000, 48000, 0, 3},
	{49152000, 48000, 0, 4},

	/*64k*/
	{8192000, 64000, 1, 0},
	{12288000, 64000, 1, 1},
	{16934400, 64000, 1, 2},
	{24576000, 64000, 1, 3},
	{32768000, 64000, 1, 4},

	/* 88.2k */
	{11289600, 88200, 1, 0},
	{16934400, 88200, 1, 1},
	{22579200, 88200, 1, 2},
	{33868000, 88200, 1, 3},
	{45158400, 88200, 1, 4},

	/* 96k */
	{12288000, 96000, 1, 0},
	{18432000, 96000, 1, 1},
	{24576000, 96000, 1, 2},
	{36864000, 96000, 1, 3},
	{49152000, 96000, 1, 4},

	/* 128k */
	{8192000, 128000, 2, 0},
	{12288000, 128000, 2, 1},
	{16934400, 128000, 2, 2},
	{24576000, 128000, 2, 3},
	{32768000, 128000, 2, 4},

	/* 176.4k */
	{11289600, 176400, 2, 0},
	{16934400, 176400, 2, 1},
	{22579200, 176400, 2, 2},
	{33868000, 176400, 2, 3},
	{49152000, 176400, 2, 4},

	/* 192k */
	{12288000, 192000, 2, 0},
	{18432000, 192000, 2, 1},
	{24576000, 192000, 2, 2},
	{36864000, 192000, 2, 3},
	{49152000, 192000, 2, 4},
};

static int cs4245_get_clk_index(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clk_map_table); i++) {
		if (clk_map_table[i].rate == rate &&
				clk_map_table[i].mclk == mclk)
			return i;
	}
	return -EINVAL;
}

static int cs4245_set_sysclk(struct snd_soc_dai *codec_dai, int clk_id,
			unsigned int freq, int dir)
{
	struct snd_soc_component *component = codec_dai->component;
	struct cs4245_private *cs4245 = snd_soc_component_get_drvdata(component);
	int i;

	if (freq == 0) {
		dev_info(component->dev, "Ignoring freq 0\n");
		return 0;
	}

	if (clk_id != 0) {
		dev_err(component->dev, "Invalid clk_id %d\n", clk_id);
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(clk_map_table); i++) {
		if (clk_map_table[i].mclk == freq) {
			cs4245->sysclk = freq;
			return 0;
		}
	}
	cs4245->sysclk = 0;
	dev_err(component->dev, "Invalid freq parameter %d\n", freq);
	return -EINVAL;
}

static int cs4245_set_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_component *component = codec_dai->component;
	struct cs4245_private *cs4245 = snd_soc_component_get_drvdata(component);
	u8 iface = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		snd_soc_component_update_bits(component, CS4245_ADC_CTL,
				CS4245_ADC_MASTER,
				CS4245_ADC_MASTER);
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		snd_soc_component_update_bits(component, CS4245_ADC_CTL,
				CS4245_ADC_MASTER,
				0);
		break;
	default:
		return -EINVAL;
	}

	 /* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		iface |= SND_SOC_DAIFMT_I2S;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		iface |= SND_SOC_DAIFMT_RIGHT_J;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface |= SND_SOC_DAIFMT_LEFT_J;
		break;
	default:
		return -EINVAL;
	}

	cs4245->format = iface;

#ifdef DEBUG_CS4245
	cs4245_printk_register_values(cs4245, "set_fmt");
#endif

	return 0;
}

static int cs4245_digital_mute(struct snd_soc_dai *dai, int mute, int direction)
{
	struct snd_soc_component *component = dai->component;

	if (mute) {
		snd_soc_component_update_bits(component, CS4245_DAC_CTL,
			CS4245_DAC_CTL_MUTE,
			CS4245_DAC_CTL_MUTE);
		snd_soc_component_update_bits(component, CS4245_ADC_CTL,
			CS4245_ADC_CTL_MUTE,
			CS4245_ADC_CTL_MUTE);
	} else {
		snd_soc_component_update_bits(component, CS4245_DAC_CTL,
			CS4245_DAC_CTL_MUTE,
			0);
		snd_soc_component_update_bits(component, CS4245_ADC_CTL,
			CS4245_ADC_CTL_MUTE,
			0);
	}
	return 0;
}

static int cs4245_pcm_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct cs4245_private *cs4245 = snd_soc_component_get_drvdata(component);
	int index;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
		((cs4245->format & SND_SOC_DAIFMT_FORMAT_MASK)
		== SND_SOC_DAIFMT_RIGHT_J))
		return -EINVAL;

	index = cs4245_get_clk_index(cs4245->sysclk, params_rate(params));
	if (index >= 0) {
		snd_soc_component_update_bits(component, CS4245_ADC_CTL,
			CS4245_ADC_FM, clk_map_table[index].fm_mode << 6);
		snd_soc_component_update_bits(component, CS4245_MCLK_FREQ,
			CS4245_MCLK_FREQ_MASK, clk_map_table[index].mclkdiv << 4);

	} else {
		dev_err(component->dev, "can't get correct mclk\n");
		return -EINVAL;
	}

	switch (cs4245->format & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		snd_soc_component_update_bits(component, CS4245_DAC_CTL,
			CS4245_DAC_CTL_DIF, (1 << 4));
		snd_soc_component_update_bits(component, CS4245_ADC_CTL,
			CS4245_ADC_DIF, (1 << 4));
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		if (params_width(params) == 16) {
			snd_soc_component_update_bits(component, CS4245_DAC_CTL,
				CS4245_DAC_CTL_DIF, (2 << 4));
		} else {
			snd_soc_component_update_bits(component, CS4245_DAC_CTL,
				CS4245_DAC_CTL_DIF, (3 << 4));
		}
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		snd_soc_component_update_bits(component, CS4245_DAC_CTL,
			CS4245_DAC_CTL_DIF, 0);
		snd_soc_component_update_bits(component, CS4245_ADC_CTL,
			CS4245_ADC_DIF, 0);
		break;
	default:
		return -EINVAL;
	}

#ifdef DEBUG_CS4245
	cs4245_printk_register_values(cs4245, "hw_params");
#endif

	return 0;
}

#ifndef __MOD_DEVICES__
static int cs4245_set_bias_level(struct snd_soc_component *component,
					enum snd_soc_bias_level level)
{
	switch (level) {
	case SND_SOC_BIAS_ON:
		break;
	case SND_SOC_BIAS_PREPARE:
		snd_soc_component_update_bits(component, CS4245_PWRCTL,
			CS4245_PWRCTL_PDN, 0);
		break;
	case SND_SOC_BIAS_STANDBY:
		snd_soc_component_update_bits(component, CS4245_PWRCTL,
			CS4245_PWRCTL_PDN,
			CS4245_PWRCTL_PDN);
		break;
	case SND_SOC_BIAS_OFF:
		snd_soc_component_update_bits(component, CS4245_PWRCTL,
			CS4245_PWRCTL_PDN,
			CS4245_PWRCTL_PDN);
		break;
	}

#ifdef DEBUG_CS4245
	{
		struct cs4245_private *cs4245 = snd_soc_component_get_drvdata(component);
		cs4245_printk_register_values(cs4245, "set_bias_level");
	}
#endif

	return 0;
}
#endif // __MOD_DEVICES__

#ifndef __MOD_DEVICES__
#define CS4245_RATES (SNDRV_PCM_RATE_48000)
#define CS4245_FORMATS (SNDRV_PCM_FMTBIT_S24_LE)
#else
#define CS4245_RATES (SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | \
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_64000 | \
			SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | \
			SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000)

#define CS4245_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_U16_LE | \
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_U24_LE | \
			SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_U32_LE)
#endif

static const struct snd_soc_dai_ops cs4245_ops = {
	.hw_params	= cs4245_pcm_hw_params,
	.mute_stream = cs4245_digital_mute,
	.set_fmt = cs4245_set_fmt,
	.set_sysclk	= cs4245_set_sysclk,
};

static struct snd_soc_dai_driver cs4245_dai[] = {
	{
		.name = "cs4245-dai1",
		.playback = {
			.stream_name = "DAI1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4245_RATES,
			.formats = CS4245_FORMATS,
		},
		.capture = {
			.stream_name = "DAI1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4245_RATES,
			.formats = CS4245_FORMATS,
		},
		.ops = &cs4245_ops,
	},
	{
		.name = "cs4245-dai2",
		.playback = {
			.stream_name = "DAI2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4245_RATES,
			.formats = CS4245_FORMATS,
		},
		.capture = {
			.stream_name = "DAI2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4245_RATES,
			.formats = CS4245_FORMATS,
		},
		.ops = &cs4245_ops,
	},
};

static const struct snd_soc_component_driver soc_component_cs4245 = {
	.controls		= cs4245_snd_controls,
	.num_controls		= ARRAY_SIZE(cs4245_snd_controls),
#ifndef __MOD_DEVICES__
	.set_bias_level = cs4245_set_bias_level,
	.dapm_widgets		= cs4245_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(cs4245_dapm_widgets),
	.dapm_routes		= cs4245_audio_map,
	.num_dapm_routes	= ARRAY_SIZE(cs4245_audio_map),
#endif
	.idle_bias_on = 1,
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct regmap_config cs4245_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = CS4245_MAX_REGISTER,
	.reg_defaults = cs4245_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs4245_reg_defaults),
	.readable_reg = cs4245_readable_register,
	.volatile_reg = cs4245_volatile_register,
	.cache_type = REGCACHE_RBTREE,
};

static int cs4245_i2c_probe(struct i2c_client *i2c_client,
			     const struct i2c_device_id *id)
{
	struct cs4245_private *cs4245;
	int ret = 0;
	unsigned int devid = 0;
	unsigned int reg;

	cs4245 = devm_kzalloc(&i2c_client->dev, sizeof(struct cs4245_private),
			       GFP_KERNEL);
	if (cs4245 == NULL)
		return -ENOMEM;

	cs4245->regmap = devm_regmap_init_i2c(i2c_client, &cs4245_regmap);
	if (IS_ERR(cs4245->regmap)) {
		ret = PTR_ERR(cs4245->regmap);
		dev_err(&i2c_client->dev, "regmap_init() failed: %d\n", ret);
		return ret;
	}

	cs4245->reset_gpio = devm_gpiod_get_optional(&i2c_client->dev,
		"reset", GPIOD_OUT_LOW);
	if (IS_ERR(cs4245->reset_gpio))
		return PTR_ERR(cs4245->reset_gpio);

	if (cs4245->reset_gpio) {
		mdelay(1);
		gpiod_set_value_cansleep(cs4245->reset_gpio, 1);
	}

	i2c_set_clientdata(i2c_client, cs4245);

	ret = regmap_read(cs4245->regmap, CS4245_CHIP_ID, &reg);
	devid = reg & CS4245_CHIP_ID_MASK;
	if (devid != CS4245_CHIP_ID_VAL) {
		ret = -ENODEV;
		dev_err(&i2c_client->dev,
			"CS4245 Device ID (%X). Expected %X\n",
			devid, CS4245_CHIP_ID);
		return ret;
	}
	dev_info(&i2c_client->dev,
		"CS4245 Version %x\n",
			reg & CS4245_REV_ID_MASK);

#ifdef __MOD_DEVICES__
	// setup registers as needed for MOD Duo
	regmap_write(cs4245->regmap, CS4245_PWRCTL, CS4245_PWRCTL_PDN_MIC);
	regmap_write(cs4245->regmap, CS4245_DAC_CTL, 0x08 | CS4245_DAC_CTL_MUTE); /* reserved, muted */
	regmap_write(cs4245->regmap, CS4245_ADC_CTL, CS4245_ADC_CTL_MUTE); /* muted */
	regmap_write(cs4245->regmap, CS4245_SIG_SEL, 0x02); /* Digital Loopback */
	regmap_write(cs4245->regmap, CS4245_ADC_CTL2, 0x10 | 0x08 | 0x04); /* Soft Ramp, Zero Cross, Input Pair 4 */
	regmap_write(cs4245->regmap, CS4245_DAC_CTL2, 0x08 | 0x04); /* Soft Ramp, Zero Cross */
#endif

	ret = devm_snd_soc_register_component(&i2c_client->dev,
			&soc_component_cs4245, cs4245_dai,
			ARRAY_SIZE(cs4245_dai));

#ifdef __MOD_DEVICES__
	if (ret == 0)
		ret = modduo_init(i2c_client);
#endif

	return ret;
}

static const struct of_device_id cs4245_of_match[] = {
	{ .compatible = "cirrus,cs4245", },
	{ }
};
MODULE_DEVICE_TABLE(of, cs4245_of_match);

static const struct i2c_device_id cs4245_id[] = {
	{ "cs4245", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cs4245_id);

static struct i2c_driver cs4245_i2c_driver = {
	.driver = {
		.name = "cs4245",
		.of_match_table = cs4245_of_match,
	},
	.id_table = cs4245_id,
	.probe = cs4245_i2c_probe,
};

module_i2c_driver(cs4245_i2c_driver);

MODULE_DESCRIPTION("ASoC CS4245 driver");
MODULE_AUTHOR("Filipe Coelho <falktx@falktx.com>");
MODULE_LICENSE("GPL");
