/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Small compatibility helpers for the MT6789 AFE port.
 *
 * The vendor MediaTek audio tree has a few helpers in
 * sound/soc/mediatek/common that were never upstreamed (or were made
 * static in mainline). Provide the minimal versions needed by the
 * playback-only port here.
 */

#ifndef _MT6789_AUDIO_COMPAT_H
#define _MT6789_AUDIO_COMPAT_H

#include <linux/regmap.h>
#include "mt6789-afe-common.h"

static inline int mt6789_regmap_update_bits(struct regmap *map, int reg,
					    unsigned int mask,
					    unsigned int val, int shift)
{
	if (reg < 0 || WARN_ON_ONCE(shift < 0))
		return 0;

	return regmap_update_bits(map, reg, mask << shift, val << shift);
}

static inline int mt6789_irq_set_enable(struct mtk_base_afe *afe,
					const struct mtk_base_irq_data *irq_data,
					int afe_id)
{
	if (!afe || !irq_data)
		return -ENODEV;

	return regmap_update_bits(afe->regmap, irq_data->irq_en_reg,
				  BIT(irq_data->irq_en_shift),
				  BIT(irq_data->irq_en_shift));
}

static inline int mt6789_irq_set_disable(struct mtk_base_afe *afe,
					 const struct mtk_base_irq_data *irq_data,
					 int afe_id)
{
	if (!afe || !irq_data)
		return -ENODEV;

	return regmap_update_bits(afe->regmap, irq_data->irq_en_reg,
				  BIT(irq_data->irq_en_shift), 0);
}

static inline unsigned int mt6789_afe_need_triggered(struct mtk_base_afe_memif *memif)
{
	/* SCP/ADSP shared-memory paths are not supported in this port */
	return true;
}

static inline unsigned int mt6789_word_size_align(unsigned int in_size)
{
	/* MTK memif access needs 16-byte alignment */
	return in_size & 0xFFFFFFF0;
}

#endif
