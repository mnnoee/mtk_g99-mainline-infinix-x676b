/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal smart speaker amplifier stubs for the MT6789 audio port.
 *
 * The vendor smart-PA framework (mtk-sp-spk-amp.c) is not part of
 * mainline. For the initial headphone-only playback path we only need
 * the control-name strings and no-op helpers so that the machine driver
 * can be built and probed.
 */

#ifndef _MTK_SP_SPK_AMP_H
#define _MTK_SP_SPK_AMP_H
#include <sound/soc.h>

#define MTK_SPK_NOT_SMARTPA_STR "MTK_SPK_NOT_SMARTPA"
#define MTK_SPK_RICHTEK_RT5509_STR "MTK_SPK_RICHTEK_RT5509"
#define MTK_SPK_MEDIATEK_MT6660_STR "MTK_SPK_MEDIATEK_MT6660"
#define MTK_SPK_RICHTEK_RT5512_STR "MTK_SPK_MEDIATEK_RT5512"
#define MTK_SPK_GOODIX_TFA98XX_STR "MTK_SPK_GOODIX_TFAXXXX"

#define MTK_SPK_I2S_0_STR "MTK_SPK_I2S_0"
#define MTK_SPK_I2S_1_STR "MTK_SPK_I2S_1"
#define MTK_SPK_I2S_2_STR "MTK_SPK_I2S_2"
#define MTK_SPK_I2S_3_STR "MTK_SPK_I2S_3"
#define MTK_SPK_I2S_5_STR "MTK_SPK_I2S_5"

enum mtk_spk_type {
	MTK_SPK_NOT_SMARTPA = 0,
	MTK_SPK_RICHTEK_RT5509,
	MTK_SPK_MEDIATEK_MT6660,
	MTK_SPK_GOODIX_TFA98XX,
	MTK_SPK_MEDIATEK_RT5512,
	MTK_SPK_TYPE_NUM
};

static inline int mtk_spk_get_type(void)
{
	return MTK_SPK_NOT_SMARTPA;
}

static inline void mtk_spk_set_type(int spk_type) { }

static inline int mtk_spk_get_i2s_out_type(void)
{
	return -1;
}

static inline int mtk_spk_get_i2s_in_type(void)
{
	return -1;
}

static inline int mtk_spk_update_info(struct snd_soc_card *card,
				      struct platform_device *pdev)
{
	return 0;
}

#endif
