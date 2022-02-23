// SPDX-License-Identifier: GPL-2.0-only
/*
 * cs4265.c -- CS4265 ALSA SoC audio driver
 *
 * Copyright 2014 Cirrus Logic, Inc.
 *
 * Author: Paul Handrigan <paul.handrigan@cirrus.com>
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include "cs4265.h"

//----------------------------------------------------------------------

#ifdef  __MOD_DEVICES__

#include <linux/interrupt.h>

// GPIO macros
#define CHANNEL_LEFT    0
#define CHANNEL_RIGHT   1

#define GPIO_BYPASS     0
#define GPIO_PROCESS    1

// tip means enable2, ring enable1
#define EXP_PEDAL_SIGNAL_ON_TIP  false
#define EXP_PEDAL_SIGNAL_ON_RING true

static int headphone_volume = 0; // Headphone volume has a total of 16 steps, each corresponds to 3dB. Step 11 is 0dB.
static int input_left_gain_stage = 0;
static int input_right_gain_stage = 0;
#ifdef _MOD_DEVICE_DUOX
static bool left_true_bypass = true;
static bool right_true_bypass = true;
static bool headphone_cv_mode = false; // true means CV mode, false is headphone (CV output mode)
static bool cv_exp_pedal_mode = false; // true means expression pedal mode, false is CV mode (CV input mode)
static bool exp_pedal_mode = EXP_PEDAL_SIGNAL_ON_TIP;
#endif

static struct _modduox_gpios {
	struct gpio_desc *headphone_cv_mode;
	struct gpio_desc *headphone_clk;
	struct gpio_desc *headphone_dir;
	struct gpio_desc *gain_stage_left1;
	struct gpio_desc *gain_stage_left2;
	struct gpio_desc *gain_stage_right1;
	struct gpio_desc *gain_stage_right2;
#ifdef _MOD_DEVICE_DUOX
	struct gpio_desc *true_bypass_left;
	struct gpio_desc *true_bypass_right;
	struct gpio_desc *exp_enable1;
	struct gpio_desc *exp_enable2;
	struct gpio_desc *exp_flag1;
	struct gpio_desc *exp_flag2;
	int irqFlag1, irqFlag2;
#endif
	bool initialized;
} *modduox_gpios;

#ifdef _MOD_DEVICE_DUOX
static void set_cv_exp_pedal_mode(int mode);

static irqreturn_t exp_flag_irq_handler(int irq, void *dev_id)
{
	printk(KERN_INFO "MOD Devices: Expression Pedal flag IRQ %u triggered! (values are %d %d)\n",
	       irq,
	       gpiod_get_value(modduox_gpios->exp_flag1),
	       gpiod_get_value(modduox_gpios->exp_flag2));
	set_cv_exp_pedal_mode(0);
	return IRQ_HANDLED;
}
#endif

static int moddevices_init(struct i2c_client *i2c_client)
{
	int i;

	modduox_gpios = devm_kzalloc(&i2c_client->dev, sizeof(struct _modduox_gpios), GFP_KERNEL);
	if (modduox_gpios == NULL)
		return -ENOMEM;

	modduox_gpios->headphone_clk     = devm_gpiod_get(&i2c_client->dev, "headphone_clk",     GPIOD_OUT_HIGH);
	modduox_gpios->headphone_dir     = devm_gpiod_get(&i2c_client->dev, "headphone_dir",     GPIOD_OUT_HIGH);
	modduox_gpios->gain_stage_left1  = devm_gpiod_get(&i2c_client->dev, "gain_stage_left1",  GPIOD_OUT_HIGH);
	modduox_gpios->gain_stage_left2  = devm_gpiod_get(&i2c_client->dev, "gain_stage_left2",  GPIOD_OUT_HIGH);
	modduox_gpios->gain_stage_right1 = devm_gpiod_get(&i2c_client->dev, "gain_stage_right1", GPIOD_OUT_HIGH);
	modduox_gpios->gain_stage_right2 = devm_gpiod_get(&i2c_client->dev, "gain_stage_right2", GPIOD_OUT_HIGH);
#ifdef _MOD_DEVICE_DUOX
	modduox_gpios->headphone_cv_mode = devm_gpiod_get(&i2c_client->dev, "headphone_cv_mode", GPIOD_OUT_HIGH);
	modduox_gpios->exp_enable1       = devm_gpiod_get(&i2c_client->dev, "exp_enable1",       GPIOD_OUT_HIGH);
	modduox_gpios->exp_enable2       = devm_gpiod_get(&i2c_client->dev, "exp_enable2",       GPIOD_OUT_HIGH);
	modduox_gpios->exp_flag1         = devm_gpiod_get(&i2c_client->dev, "exp_flag1",         GPIOD_IN);
	modduox_gpios->exp_flag2         = devm_gpiod_get(&i2c_client->dev, "exp_flag2",         GPIOD_IN);

	// bypass is inverted
	modduox_gpios->true_bypass_left  = devm_gpiod_get(&i2c_client->dev, "true_bypass_left",  GPIOD_OUT_LOW);
	modduox_gpios->true_bypass_right = devm_gpiod_get(&i2c_client->dev, "true_bypass_right", GPIOD_OUT_LOW);
#endif

	if (IS_ERR(modduox_gpios->gain_stage_left1) || !modduox_gpios->gain_stage_left2) {
		modduox_gpios->initialized = false;
		return 0;
	}

	// put headphone volume to lowest setting, so we know where we are
	gpiod_set_value(modduox_gpios->headphone_dir, 0);

	for (i=0; i < 16; i++) {
		// toggle clock in order to sample the volume pin upon clock's rising edge
		gpiod_set_value(modduox_gpios->headphone_clk, 1);
		gpiod_set_value(modduox_gpios->headphone_clk, 0);
	}

	// initialize gpios
#ifdef _MOD_DEVICE_DUOX
	gpiod_set_value(modduox_gpios->headphone_cv_mode, headphone_cv_mode ? 1 : 0);
	gpiod_set_value(modduox_gpios->exp_enable1, 0);
	gpiod_set_value(modduox_gpios->exp_enable2, 0);
#endif

	// FIXME does this mean lowest gain stage? need to confirm
	gpiod_set_value(modduox_gpios->gain_stage_left1, 1);
	gpiod_set_value(modduox_gpios->gain_stage_left2, 1);
	gpiod_set_value(modduox_gpios->gain_stage_right1, 1);
	gpiod_set_value(modduox_gpios->gain_stage_right2, 1);

	modduox_gpios->initialized = true;

#ifdef _MOD_DEVICE_DUOX
	modduox_gpios->irqFlag1 = gpiod_to_irq(modduox_gpios->exp_flag1);
	modduox_gpios->irqFlag2 = gpiod_to_irq(modduox_gpios->exp_flag2);

	if (modduox_gpios->irqFlag1 > 0 && modduox_gpios->irqFlag2 > 0)
	{
		if (request_irq(modduox_gpios->irqFlag1, exp_flag_irq_handler, IRQF_TRIGGER_RISING, "exp_flag1_handler", NULL) != 0 ||
		    request_irq(modduox_gpios->irqFlag2, exp_flag_irq_handler, IRQF_TRIGGER_RISING, "exp_flag2_handler", NULL) != 0)
		{
			modduox_gpios->irqFlag1 = 0;
			modduox_gpios->irqFlag2 = 0;
		}
	}

	if (modduox_gpios->irqFlag1 > 0 && modduox_gpios->irqFlag2 > 0)
		printk("MOD Devices: Expression Pedal flag IRQ setup ok!\n");
	else
		printk("MOD Devices: Expression Pedal flag IRQ failed!\n");
#endif

	return 0;
}

/* This routine flips the GPIO pins to send the volume adjustment
   message to the actual headphone gain-control chip (LM4811) */
static void set_headphone_volume(int new_volume)
{
	int i;
	int steps = abs(new_volume - headphone_volume);

	if (modduox_gpios->initialized) {
		// select volume adjustment direction
		gpiod_set_value(modduox_gpios->headphone_dir, new_volume > headphone_volume ? 1 : 0);

		for (i=0; i < steps; i++) {
			// toggle clock in order to sample the volume pin upon clock's rising edge
			gpiod_set_value(modduox_gpios->headphone_clk, 1);
			gpiod_set_value(modduox_gpios->headphone_clk, 0);
		}
	}

	headphone_volume = new_volume;
}

static void set_gain_stage(int channel, int state)
{
	struct gpio_desc *gpio1, *gpio2;

	switch (channel) {
	case CHANNEL_LEFT:
		gpio1 = modduox_gpios->gain_stage_left1;
		gpio2 = modduox_gpios->gain_stage_left2;
		input_left_gain_stage = state;
		break;
	case CHANNEL_RIGHT:
		gpio1 = modduox_gpios->gain_stage_right1;
		gpio2 = modduox_gpios->gain_stage_right2;
		input_right_gain_stage = state;
		break;
	default:
		return;
	}

	if (!modduox_gpios->initialized)
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

#ifdef _MOD_DEVICE_DUOX
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
		if (modduox_gpios->initialized)
			gpiod_set_value(modduox_gpios->true_bypass_left, state ? GPIO_BYPASS : GPIO_PROCESS);
		left_true_bypass = state;
		break;
	case CHANNEL_RIGHT:
		if (modduox_gpios->initialized)
			gpiod_set_value(modduox_gpios->true_bypass_right, state ? GPIO_BYPASS : GPIO_PROCESS);
		right_true_bypass = state;
		break;
	}
}

/* switch thingies
 */
static void set_headphone_cv_mode(int mode)
{
	switch (mode) {
	case 0:
	case 1:
		if (modduox_gpios->initialized)
			gpiod_set_value(modduox_gpios->headphone_cv_mode, mode);
		headphone_cv_mode = mode != 0;
		break;
	default:
		break;
	}
}

static void set_exp_pedal_mode(int mode)
{
	switch (mode) {
	case 0:
	case 1:
		if (cv_exp_pedal_mode && modduox_gpios->initialized)
		{
			if (modduox_gpios->irqFlag1 <= 0 || modduox_gpios->irqFlag2 <= 0)
			{
				printk("MOD Devices: set_exp_pedal_mode(%i) call ignored, as Expression Pedal flag IRQ failed before\n", mode);
			}
			else if (mode == (int)EXP_PEDAL_SIGNAL_ON_TIP)
			{
				gpiod_set_value(modduox_gpios->exp_enable1, 0);
				gpiod_set_value(modduox_gpios->exp_enable2, 1);
			}
			else
			{
				gpiod_set_value(modduox_gpios->exp_enable2, 0);
				gpiod_set_value(modduox_gpios->exp_enable1, 1);
			}
		}
		exp_pedal_mode = mode != 0;
		break;
	default:
		break;
	}
}

static void set_cv_exp_pedal_mode(int mode)
{
	switch (mode) {
	case 0: // cv mode
		cv_exp_pedal_mode = false;
		if (modduox_gpios->initialized) {
			gpiod_set_value(modduox_gpios->exp_enable1, 0);
			gpiod_set_value(modduox_gpios->exp_enable2, 0);
		}
		break;

	case 1: // exp.pedal mode
		cv_exp_pedal_mode = true;
		set_exp_pedal_mode(exp_pedal_mode);
		break;

	default:
		break;
	}
}
#endif // _MOD_DEVICE_DUOX

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

#ifdef _MOD_DEVICE_DUOX
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

//----------------------------------------------------------------------

static int headphone_cv_mode_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int headphone_cv_mode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = headphone_cv_mode;
	return 0;
}

static int headphone_cv_mode_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (headphone_cv_mode != ucontrol->value.integer.value[0]) {
		set_headphone_cv_mode(ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}

//----------------------------------------------------------------------

static int cv_exp_pedal_mode_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int cv_exp_pedal_mode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = cv_exp_pedal_mode;
	return 0;
}

static int cv_exp_pedal_mode_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (cv_exp_pedal_mode != ucontrol->value.integer.value[0]) {
		set_cv_exp_pedal_mode(ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}

//----------------------------------------------------------------------

static int exp_pedal_mode_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int exp_pedal_mode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = exp_pedal_mode;
	return 0;
}

static int exp_pedal_mode_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	int changed = 0;
	if (exp_pedal_mode != ucontrol->value.integer.value[0]) {
		set_exp_pedal_mode(ucontrol->value.integer.value[0]);
		changed = 1;
	}
	return changed;
}
#endif //  _MOD_DEVICE_DUOX
#endif // __MOD_DEVICES__

//----------------------------------------------------------------------


struct cs4265_private {
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	u8 format;
	u32 sysclk;
};

static const struct reg_default cs4265_reg_defaults[] = {
	{ CS4265_PWRCTL, 0x0F },
	{ CS4265_DAC_CTL, 0x08 },
	{ CS4265_ADC_CTL, 0x00 },
	{ CS4265_MCLK_FREQ, 0x00 },
	{ CS4265_SIG_SEL, 0x40 },
	{ CS4265_CHB_PGA_CTL, 0x00 },
	{ CS4265_CHA_PGA_CTL, 0x00 },
	{ CS4265_ADC_CTL2, 0x19 },
	{ CS4265_DAC_CHB_VOL, 0x00 },
	{ CS4265_DAC_CHA_VOL, 0x00 },
	{ CS4265_DAC_CTL2, 0xC0 },
	{ CS4265_SPDIF_CTL1, 0x00 },
	{ CS4265_SPDIF_CTL2, 0x00 },
	{ CS4265_INT_MASK, 0x00 },
	{ CS4265_STATUS_MODE_MSB, 0x00 },
	{ CS4265_STATUS_MODE_LSB, 0x00 },
};

static bool cs4265_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS4265_CHIP_ID ... CS4265_MAX_REGISTER:
		return true;
	default:
		return false;
	}
}

static bool cs4265_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS4265_INT_STATUS:
		return true;
	default:
		return false;
	}
}

static DECLARE_TLV_DB_SCALE(pga_tlv, -1200, 50, 0);

static DECLARE_TLV_DB_SCALE(dac_tlv, -12750, 50, 0);

#ifndef _MOD_DEVICE_DWARF
static const char * const digital_input_mux_text[] = {
	"SDIN1", "SDIN2"
};

static SOC_ENUM_SINGLE_DECL(digital_input_mux_enum, CS4265_SIG_SEL, 7,
		digital_input_mux_text);

static const struct snd_kcontrol_new digital_input_mux =
	SOC_DAPM_ENUM("Digital Input Mux", digital_input_mux_enum);

static const char * const mic_linein_text[] = {
	"MIC", "LINEIN"
};

static SOC_ENUM_SINGLE_DECL(mic_linein_enum, CS4265_ADC_CTL2, 0,
		mic_linein_text);

static const char * const cam_mode_text[] = {
	"One Byte", "Two Byte"
};

static SOC_ENUM_SINGLE_DECL(cam_mode_enum, CS4265_SPDIF_CTL1, 5,
		cam_mode_text);

static const char * const cam_mono_stereo_text[] = {
	"Stereo", "Mono"
};

static SOC_ENUM_SINGLE_DECL(spdif_mono_stereo_enum, CS4265_SPDIF_CTL2, 2,
		cam_mono_stereo_text);

static const char * const mono_select_text[] = {
	"Channel A", "Channel B"
};

static SOC_ENUM_SINGLE_DECL(spdif_mono_select_enum, CS4265_SPDIF_CTL2, 0,
		mono_select_text);

static const struct snd_kcontrol_new mic_linein_mux =
	SOC_DAPM_ENUM("ADC Input Capture Mux", mic_linein_enum);

static const struct snd_kcontrol_new loopback_ctl =
	SOC_DAPM_SINGLE("Switch", CS4265_SIG_SEL, 1, 1, 0);

static const struct snd_kcontrol_new spdif_switch =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 0, 0);

static const struct snd_kcontrol_new dac_switch =
	SOC_DAPM_SINGLE("Switch", CS4265_PWRCTL, 1, 1, 0);
#endif // ! _MOD_DEVICE_DWARF

#ifdef __MOD_DEVICES__
static const unsigned int gain_stages_tlv[] = {
    TLV_DB_RANGE_HEAD(4),
    0, 0, TLV_DB_SCALE_ITEM(0,  0, 0),
    1, 1, TLV_DB_SCALE_ITEM(6.0, 0, 0),
    2, 2, TLV_DB_SCALE_ITEM(15.0, 0, 0),
    3, 3, TLV_DB_SCALE_ITEM(20.4, 0, 0),
};
#endif

static const struct snd_kcontrol_new cs4265_snd_controls[] = {

#ifdef __MOD_DEVICES__
	SOC_DOUBLE_R_SX_TLV("PGA Gain", CS4265_CHA_PGA_CTL,
			      CS4265_CHB_PGA_CTL, 0, 0x28, 0x30, pga_tlv),
#else
	SOC_DOUBLE_R_SX_TLV("PGA Volume", CS4265_CHA_PGA_CTL,
			      CS4265_CHB_PGA_CTL, 0, 0x28, 0x30, pga_tlv),
#endif
	SOC_DOUBLE_R_TLV("DAC Volume", CS4265_DAC_CHA_VOL,
		      CS4265_DAC_CHB_VOL, 0, 0xFF, 1, dac_tlv),
#ifndef _MOD_DEVICE_DWARF
	SOC_SINGLE("De-emp 44.1kHz Switch", CS4265_DAC_CTL, 1,
				1, 0),
	SOC_SINGLE("DAC INV Switch", CS4265_DAC_CTL2, 5,
				1, 0),
	SOC_SINGLE("DAC Zero Cross Switch", CS4265_DAC_CTL2, 6,
				1, 0),
	SOC_SINGLE("DAC Soft Ramp Switch", CS4265_DAC_CTL2, 7,
				1, 0),
	SOC_SINGLE("ADC HPF Switch", CS4265_ADC_CTL, 1,
				1, 0),
	SOC_SINGLE("ADC Zero Cross Switch", CS4265_ADC_CTL2, 3,
				1, 1),
	SOC_SINGLE("ADC Soft Ramp Switch", CS4265_ADC_CTL2, 7,
				1, 0),
	SOC_SINGLE("E to F Buffer Disable Switch", CS4265_SPDIF_CTL1,
				6, 1, 0),
	SOC_ENUM("C Data Access", cam_mode_enum),
	SOC_SINGLE("Validity Bit Control Switch", CS4265_SPDIF_CTL2,
				3, 1, 0),
	SOC_ENUM("SPDIF Mono/Stereo", spdif_mono_stereo_enum),
	SOC_SINGLE("MMTLR Data Switch", CS4265_SPDIF_CTL2, 0, 1, 0),
	SOC_ENUM("Mono Channel Select", spdif_mono_select_enum),
	SND_SOC_BYTES("C Data Buffer", CS4265_C_DATA_BUFF, 24),
#else
	SOC_SINGLE("LOOPBACK", CS4265_SIG_SEL, 1, 1, 0),
#endif // _MOD_DEVICE_DWARF
#ifdef __MOD_DEVICES__
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
#ifdef _MOD_DEVICE_DUOX
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
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Headphone/CV Mode",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = headphone_cv_mode_info,
		.get = headphone_cv_mode_get,
		.put = headphone_cv_mode_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "CV/Exp.Pedal Mode",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = cv_exp_pedal_mode_info,
		.get = cv_exp_pedal_mode_get,
		.put = cv_exp_pedal_mode_put
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Exp.Pedal Mode",
		.index = 0,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = exp_pedal_mode_info,
		.get = exp_pedal_mode_get,
		.put = exp_pedal_mode_put
	},
#endif // _MOD_DEVICE_DUOX
#endif // __MOD_DEVICES__
};

#ifndef _MOD_DEVICE_DWARF
static const struct snd_soc_dapm_widget cs4265_dapm_widgets[] = {

	SND_SOC_DAPM_INPUT("LINEINL"),
	SND_SOC_DAPM_INPUT("LINEINR"),
	SND_SOC_DAPM_INPUT("MICL"),
	SND_SOC_DAPM_INPUT("MICR"),

	SND_SOC_DAPM_AIF_OUT("DOUT", NULL,  0,
			SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SPDIFOUT", NULL,  0,
			SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_MUX("ADC Mux", SND_SOC_NOPM, 0, 0, &mic_linein_mux),

	SND_SOC_DAPM_ADC("ADC", NULL, CS4265_PWRCTL, 2, 1),
	SND_SOC_DAPM_PGA("Pre-amp MIC", CS4265_PWRCTL, 3,
			1, NULL, 0),

	SND_SOC_DAPM_MUX("Input Mux", SND_SOC_NOPM,
			 0, 0, &digital_input_mux),

	SND_SOC_DAPM_MIXER("SDIN1 Input Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SDIN2 Input Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SPDIF Transmitter", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_SWITCH("Loopback", SND_SOC_NOPM, 0, 0,
			&loopback_ctl),
	SND_SOC_DAPM_SWITCH("SPDIF", CS4265_SPDIF_CTL2, 5, 1,
			&spdif_switch),
	SND_SOC_DAPM_SWITCH("DAC", CS4265_PWRCTL, 1, 1,
			&dac_switch),

	SND_SOC_DAPM_AIF_IN("DIN1", NULL,  0,
			SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("DIN2", NULL,  0,
			SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TXIN", NULL,  0,
			CS4265_SPDIF_CTL2, 5, 1),

	SND_SOC_DAPM_OUTPUT("LINEOUTL"),
	SND_SOC_DAPM_OUTPUT("LINEOUTR"),

};

static const struct snd_soc_dapm_route cs4265_audio_map[] = {

	{"DIN1", NULL, "DAI1 Playback"},
	{"DIN2", NULL, "DAI2 Playback"},
	{"SDIN1 Input Mixer", NULL, "DIN1"},
	{"SDIN2 Input Mixer", NULL, "DIN2"},
	{"Input Mux", "SDIN1", "SDIN1 Input Mixer"},
	{"Input Mux", "SDIN2", "SDIN2 Input Mixer"},
	{"DAC", "Switch", "Input Mux"},
	{"SPDIF", "Switch", "Input Mux"},
	{"LINEOUTL", NULL, "DAC"},
	{"LINEOUTR", NULL, "DAC"},
	{"SPDIFOUT", NULL, "SPDIF"},

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
#endif // ! _MOD_DEVICE_DWARF

struct cs4265_clk_para {
	u32 mclk;
	u32 rate;
	u8 fm_mode; /* values 1, 2, or 4 */
	u8 mclkdiv;
};

static const struct cs4265_clk_para clk_map_table[] = {
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

static int cs4265_get_clk_index(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clk_map_table); i++) {
		if (clk_map_table[i].rate == rate &&
				clk_map_table[i].mclk == mclk)
			return i;
	}
	return -EINVAL;
}

static int cs4265_set_sysclk(struct snd_soc_dai *codec_dai, int clk_id,
			unsigned int freq, int dir)
{
	struct snd_soc_component *component = codec_dai->component;
	struct cs4265_private *cs4265 = snd_soc_component_get_drvdata(component);
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
			cs4265->sysclk = freq;
			return 0;
		}
	}
	cs4265->sysclk = 0;
	dev_err(component->dev, "Invalid freq parameter %d\n", freq);
	return -EINVAL;
}

static int cs4265_set_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_component *component = codec_dai->component;
	struct cs4265_private *cs4265 = snd_soc_component_get_drvdata(component);
	u8 iface = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		snd_soc_component_update_bits(component, CS4265_ADC_CTL,
				CS4265_ADC_MASTER,
				CS4265_ADC_MASTER);
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		snd_soc_component_update_bits(component, CS4265_ADC_CTL,
				CS4265_ADC_MASTER,
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

	cs4265->format = iface;
	return 0;
}

static int cs4265_mute(struct snd_soc_dai *dai, int mute, int direction)
{
	struct snd_soc_component *component = dai->component;

	if (mute) {
		snd_soc_component_update_bits(component, CS4265_DAC_CTL,
			CS4265_DAC_CTL_MUTE,
			CS4265_DAC_CTL_MUTE);
		snd_soc_component_update_bits(component, CS4265_SPDIF_CTL2,
			CS4265_SPDIF_CTL2_MUTE,
			CS4265_SPDIF_CTL2_MUTE);
	} else {
		snd_soc_component_update_bits(component, CS4265_DAC_CTL,
			CS4265_DAC_CTL_MUTE,
			0);
		snd_soc_component_update_bits(component, CS4265_SPDIF_CTL2,
			CS4265_SPDIF_CTL2_MUTE,
			0);
	}
	return 0;
}

static int cs4265_pcm_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct cs4265_private *cs4265 = snd_soc_component_get_drvdata(component);
	int index;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
		((cs4265->format & SND_SOC_DAIFMT_FORMAT_MASK)
		== SND_SOC_DAIFMT_RIGHT_J))
		return -EINVAL;

	index = cs4265_get_clk_index(cs4265->sysclk, params_rate(params));
	if (index >= 0) {
		snd_soc_component_update_bits(component, CS4265_ADC_CTL,
			CS4265_ADC_FM, clk_map_table[index].fm_mode << 6);
		snd_soc_component_update_bits(component, CS4265_MCLK_FREQ,
			CS4265_MCLK_FREQ_MASK,
			clk_map_table[index].mclkdiv << 4);

	} else {
		dev_err(component->dev, "can't get correct mclk\n");
		return -EINVAL;
	}

	switch (cs4265->format & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		snd_soc_component_update_bits(component, CS4265_DAC_CTL,
			CS4265_DAC_CTL_DIF, (1 << 4));
		snd_soc_component_update_bits(component, CS4265_ADC_CTL,
			CS4265_ADC_DIF, (1 << 4));
		snd_soc_component_update_bits(component, CS4265_SPDIF_CTL2,
			CS4265_SPDIF_CTL2_DIF, (1 << 6));
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		if (params_width(params) == 16) {
			snd_soc_component_update_bits(component, CS4265_DAC_CTL,
				CS4265_DAC_CTL_DIF, (2 << 4));
			snd_soc_component_update_bits(component, CS4265_SPDIF_CTL2,
				CS4265_SPDIF_CTL2_DIF, (2 << 6));
		} else {
			snd_soc_component_update_bits(component, CS4265_DAC_CTL,
				CS4265_DAC_CTL_DIF, (3 << 4));
			snd_soc_component_update_bits(component, CS4265_SPDIF_CTL2,
				CS4265_SPDIF_CTL2_DIF, (3 << 6));
		}
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		snd_soc_component_update_bits(component, CS4265_DAC_CTL,
			CS4265_DAC_CTL_DIF, 0);
		snd_soc_component_update_bits(component, CS4265_ADC_CTL,
			CS4265_ADC_DIF, 0);
		snd_soc_component_update_bits(component, CS4265_SPDIF_CTL2,
			CS4265_SPDIF_CTL2_DIF, 0);

		break;
	default:
		return -EINVAL;
	}
	return 0;
}

#ifndef _MOD_DEVICE_DWARF
static int cs4265_set_bias_level(struct snd_soc_component *component,
					enum snd_soc_bias_level level)
{
	switch (level) {
	case SND_SOC_BIAS_ON:
		break;
	case SND_SOC_BIAS_PREPARE:
		snd_soc_component_update_bits(component, CS4265_PWRCTL,
			CS4265_PWRCTL_PDN, 0);
		break;
	case SND_SOC_BIAS_STANDBY:
		snd_soc_component_update_bits(component, CS4265_PWRCTL,
			CS4265_PWRCTL_PDN,
			CS4265_PWRCTL_PDN);
		break;
	case SND_SOC_BIAS_OFF:
		snd_soc_component_update_bits(component, CS4265_PWRCTL,
			CS4265_PWRCTL_PDN,
			CS4265_PWRCTL_PDN);
		break;
	}
	return 0;
}
#endif // ! _MOD_DEVICE_DWARF

#define CS4265_RATES (SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | \
			SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_64000 | \
			SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | \
			SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000)

#define CS4265_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_U16_LE | \
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_U24_LE | \
			SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_U32_LE)

static const struct snd_soc_dai_ops cs4265_ops = {
	.hw_params	= cs4265_pcm_hw_params,
	.mute_stream	= cs4265_mute,
	.set_fmt	= cs4265_set_fmt,
	.set_sysclk	= cs4265_set_sysclk,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver cs4265_dai[] = {
	{
		.name = "cs4265-dai1",
		.playback = {
			.stream_name = "DAI1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4265_RATES,
			.formats = CS4265_FORMATS,
		},
		.capture = {
			.stream_name = "DAI1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4265_RATES,
			.formats = CS4265_FORMATS,
		},
		.ops = &cs4265_ops,
	},
	{
		.name = "cs4265-dai2",
		.playback = {
			.stream_name = "DAI2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4265_RATES,
			.formats = CS4265_FORMATS,
		},
		.capture = {
			.stream_name = "DAI2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS4265_RATES,
			.formats = CS4265_FORMATS,
		},
		.ops = &cs4265_ops,
	},
};

static const struct snd_soc_component_driver soc_component_cs4265 = {
	.controls		= cs4265_snd_controls,
	.num_controls		= ARRAY_SIZE(cs4265_snd_controls),
#ifndef _MOD_DEVICE_DWARF
	.set_bias_level		= cs4265_set_bias_level,
	.dapm_widgets		= cs4265_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(cs4265_dapm_widgets),
	.dapm_routes		= cs4265_audio_map,
	.num_dapm_routes	= ARRAY_SIZE(cs4265_audio_map),
#endif
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct regmap_config cs4265_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = CS4265_MAX_REGISTER,
	.reg_defaults = cs4265_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs4265_reg_defaults),
	.readable_reg = cs4265_readable_register,
	.volatile_reg = cs4265_volatile_register,
	.cache_type = REGCACHE_RBTREE,
};

static int cs4265_i2c_probe(struct i2c_client *i2c_client)
{
	struct cs4265_private *cs4265;
	int ret;
	unsigned int devid = 0;
	unsigned int reg;

	cs4265 = devm_kzalloc(&i2c_client->dev, sizeof(struct cs4265_private),
			       GFP_KERNEL);
	if (cs4265 == NULL)
		return -ENOMEM;

	cs4265->regmap = devm_regmap_init_i2c(i2c_client, &cs4265_regmap);
	if (IS_ERR(cs4265->regmap)) {
		ret = PTR_ERR(cs4265->regmap);
		dev_err(&i2c_client->dev, "regmap_init() failed: %d\n", ret);
		return ret;
	}

	cs4265->reset_gpio = devm_gpiod_get_optional(&i2c_client->dev,
		"reset", GPIOD_OUT_LOW);
	if (IS_ERR(cs4265->reset_gpio))
		return PTR_ERR(cs4265->reset_gpio);

	if (cs4265->reset_gpio) {
		mdelay(1);
		gpiod_set_value_cansleep(cs4265->reset_gpio, 1);
	}

	i2c_set_clientdata(i2c_client, cs4265);

	ret = regmap_read(cs4265->regmap, CS4265_CHIP_ID, &reg);
	if (ret) {
		dev_err(&i2c_client->dev, "Failed to read chip ID: %d\n", ret);
		return ret;
	}

	devid = reg & CS4265_CHIP_ID_MASK;
	if (devid != CS4265_CHIP_ID_VAL) {
		ret = -ENODEV;
		dev_err(&i2c_client->dev,
			"CS4265 Part Number ID: 0x%x Expected: 0x%x\n",
			devid >> 4, CS4265_CHIP_ID_VAL >> 4);
		return ret;
	}
	dev_info(&i2c_client->dev,
		"CS4265 Version %x\n",
			reg & CS4265_REV_ID_MASK);

#ifdef _MOD_DEVICE_DWARF
	// setup registers as needed for MOD Dwarf
	regmap_write(cs4265->regmap, CS4265_PWRCTL, 0x08); /* turn on everything except mic */
	regmap_write(cs4265->regmap, CS4265_DAC_CTL, 0x08 | 0x00); /* reserved, everything on */
	regmap_write(cs4265->regmap, CS4265_ADC_CTL, 0x00); /* everything on */
	regmap_write(cs4265->regmap, CS4265_SIG_SEL, 0x40); /* reserved */
	regmap_write(cs4265->regmap, CS4265_ADC_CTL2, 0x10 | 0x08 | 0x01); /* Soft Ramp, Zero Cross, LINEIN */
	regmap_write(cs4265->regmap, CS4265_DAC_CTL2, 0x80 | 0x40); /* Soft Ramp, Zero Cross */
#else
	// regular stuff
	regmap_write(cs4265->regmap, CS4265_PWRCTL, 0x0F);
#endif

	ret = devm_snd_soc_register_component(&i2c_client->dev,
			&soc_component_cs4265, cs4265_dai,
			ARRAY_SIZE(cs4265_dai));

#ifdef __MOD_DEVICES__
	if (ret == 0)
		ret = moddevices_init(i2c_client);
#endif

	return ret;
}

static void cs4265_i2c_remove(struct i2c_client *i2c)
{
	struct cs4265_private *cs4265 = i2c_get_clientdata(i2c);

	if (cs4265->reset_gpio)
		gpiod_set_value_cansleep(cs4265->reset_gpio, 0);
}

static const struct of_device_id cs4265_of_match[] = {
	{ .compatible = "cirrus,cs4265", },
	{ }
};
MODULE_DEVICE_TABLE(of, cs4265_of_match);

static const struct i2c_device_id cs4265_id[] = {
	{ "cs4265", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cs4265_id);

static struct i2c_driver cs4265_i2c_driver = {
	.driver = {
		.name = "cs4265",
		.of_match_table = cs4265_of_match,
	},
	.id_table = cs4265_id,
	.probe_new = cs4265_i2c_probe,
	.remove =   cs4265_i2c_remove,
};

module_i2c_driver(cs4265_i2c_driver);

MODULE_DESCRIPTION("ASoC CS4265 driver");
MODULE_AUTHOR("Paul Handrigan, Cirrus Logic Inc, <paul.handrigan@cirrus.com>");
MODULE_LICENSE("GPL");
