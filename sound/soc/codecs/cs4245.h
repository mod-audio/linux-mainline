/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cs4265.h -- CS4245 ALSA SoC audio driver
 *
 * Copyright 2020 Filipe Coelho <falktx@falktx.com>
 *
 * Author: Filipe Coelho <falktx@falktx.com>
 */

#ifndef __CS4245_H__
#define __CS4245_H__

#define CS4245_CHIP_ID				0x1
#define CS4245_CHIP_ID_VAL			0xC0
#define CS4245_CHIP_ID_MASK			0xF0
#define CS4245_REV_ID_MASK			0x0F

#define CS4245_PWRCTL				0x02
#define CS4245_PWRCTL_PDN			0x01

#define CS4245_PWRCTL_PDN_DAC		0x02
#define CS4245_PWRCTL_PDN_ADC		0x04
#define CS4245_PWRCTL_PDN_MIC		0x08
#define CS4245_PWRCTL_FREEZE		0x80

#define CS4245_DAC_CTL				0x3
#define CS4245_DAC_MASTER			(1 << 0)
#define CS4245_DAC_CTL_MUTE			(1 << 2)
#define CS4245_DAC_CTL_DIF			(3 << 4)
#define CS4245_DAC_FM				(3 << 6)

#define CS4245_ADC_CTL				0x4
#define CS4245_ADC_MASTER			(1 << 0)
#define CS4245_ADC_CTL_MUTE		(1 << 2)
#define CS4245_ADC_DIF				(1 << 4)
#define CS4245_ADC_FM				(3 << 6)

#define CS4245_MCLK_FREQ			0x5
#define CS4245_MCLK_FREQ_MASK			(7 << 4)

#define CS4245_MCLK2_FREQ_MASK			(7 << 0)

#define CS4245_SIG_SEL				0x6

#define CS4245_CHB_PGA_CTL			0x7
#define CS4245_CHA_PGA_CTL			0x8

#define CS4245_ADC_CTL2				0x9

#define CS4245_DAC_CHB_VOL			0xA
#define CS4245_DAC_CHA_VOL			0xB

#define CS4245_DAC_CTL2				0xC

#define CS4245_INT_STATUS			0xD
#define CS4245_INT_MASK				0xE
#define CS4245_STATUS_MODE_MSB			0xF
#define CS4245_STATUS_MODE_LSB			0x10

#define CS4245_MAX_REGISTER			0x10

#endif
