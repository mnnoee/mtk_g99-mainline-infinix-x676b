// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6358/MT6366 PMIC battery monitor.
 *
 * Reads the battery voltage from the PMIC NAG ADC (continuous battery
 * voltage monitor) and estimates the state of charge from the open
 * circuit voltage. Charging status is taken from the charger power
 * supply referenced by the "power-supplies" device tree property.
 *
 * The NAG ADC register layout and the conversion formula are taken from
 * the vendor MT6358 fuel gauge driver (drivers/power/supply/mt6358-gauge.c).
 */

#include <linux/devm-helpers.h>
#include <linux/delay.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define MT6358_NAG_CTRL			0x1248
#define MT6358_NAG_EN			BIT(0)
#define MT6358_NAG_VBAT1_SEL		BIT(2)
#define MT6358_NAG_PRD_SHIFT		3
#define MT6358_NAG_PRD_MASK		GENMASK(6, 3)
#define MT6358_NAG_PERIOD		5

#define MT6358_ADC_OUT_NAG		0x10d0
#define MT6358_NAG_VALID		BIT(15)
#define MT6358_NAG_VAL_MASK		GENMASK(14, 0)

/*
 * The vendor formula returns tenths of a millivolt:
 *   raw * 1800 (VREF, mV) * 10 * 3 / 32768
 * Drop the *10 to get real millivolts.
 */
#define MT6358_NAG_SCALE_NUM		(1800ULL * 3)
#define MT6358_NAG_SCALE_DEN		32768

#define MT6358_BAT_RETRIES		8

struct mt6358_bat {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
};

/*
 * Rough single cell Li-ion OCV table, used only to give userspace a
 * capacity estimate until a real fuel gauge is available.
 */
static const struct {
	int mv;
	int pct;
} mt6358_bat_ocv[] = {
	{ 4200, 100 },
	{ 4100, 92 },
	{ 4000, 82 },
	{ 3900, 68 },
	{ 3800, 52 },
	{ 3700, 36 },
	{ 3600, 20 },
	{ 3500, 10 },
	{ 3400, 4 },
	{ 3300, 0 },
};

static int mt6358_bat_raw_to_mv(unsigned int raw)
{
	u64 val = (u64)(raw & MT6358_NAG_VAL_MASK) * MT6358_NAG_SCALE_NUM;

	return DIV_ROUND_CLOSEST_ULL(val, MT6358_NAG_SCALE_DEN);
}

static int mt6358_bat_read_mv(struct mt6358_bat *bat, int *mv)
{
	unsigned int val;
	int i, ret;

	ret = regmap_update_bits(bat->regmap, MT6358_NAG_CTRL,
				 MT6358_NAG_PRD_MASK | MT6358_NAG_VBAT1_SEL |
				 MT6358_NAG_EN,
				 (MT6358_NAG_PERIOD << MT6358_NAG_PRD_SHIFT) |
				 MT6358_NAG_EN);
	if (ret)
		return ret;

	for (i = 0; i < MT6358_BAT_RETRIES; i++) {
		ret = regmap_read(bat->regmap, MT6358_ADC_OUT_NAG, &val);
		if (ret)
			return ret;

		if (val & MT6358_NAG_VALID) {
			*mv = mt6358_bat_raw_to_mv(val);
			return 0;
		}

		usleep_range(20000, 30000);
	}

	return -ETIMEDOUT;
}

static int mt6358_bat_capacity(int mv)
{
	int i;

	if (mv >= mt6358_bat_ocv[0].mv)
		return 100;
	if (mv <= mt6358_bat_ocv[ARRAY_SIZE(mt6358_bat_ocv) - 1].mv)
		return 0;

	for (i = 1; i < ARRAY_SIZE(mt6358_bat_ocv); i++) {
		if (mv >= mt6358_bat_ocv[i].mv) {
			int hi = mt6358_bat_ocv[i - 1].mv;
			int lo = mt6358_bat_ocv[i].mv;
			int hi_pct = mt6358_bat_ocv[i - 1].pct;
			int lo_pct = mt6358_bat_ocv[i].pct;

			return lo_pct + DIV_ROUND_CLOSEST((mv - lo) *
					(hi_pct - lo_pct), hi - lo);
		}
	}

	return 0;
}

static struct power_supply *mt6358_bat_get_charger(struct mt6358_bat *bat)
{
	struct power_supply *psy;
	struct fwnode_handle *fwnode;

	fwnode = dev_fwnode(bat->dev);
	if (fwnode) {
		psy = power_supply_get_by_reference(fwnode, "mediatek,charger");
		if (!IS_ERR_OR_NULL(psy))
			return psy;
	}

	/* Fallback: the RT9470D/RT9471 lives on i2c-5 at address 0x53 */
	return power_supply_get_by_name("rt9471-5-0053");
}

static int mt6358_bat_get_charger_status(struct mt6358_bat *bat)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	psy = mt6358_bat_get_charger(bat);
	if (IS_ERR_OR_NULL(psy))
		return POWER_SUPPLY_STATUS_UNKNOWN;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS, &val);
	power_supply_put(psy);

	return ret ? POWER_SUPPLY_STATUS_UNKNOWN : val.intval;
}

static int mt6358_bat_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct mt6358_bat *bat = power_supply_get_drvdata(psy);
	int mv, ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = mt6358_bat_get_charger_status(bat);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MT6358 PMIC";
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = mt6358_bat_read_mv(bat, &mv);
		if (ret)
			return ret;
		val->intval = mv * 1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = mt6358_bat_read_mv(bat, &mv);
		if (ret)
			return ret;
		val->intval = mt6358_bat_capacity(mv);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property mt6358_bat_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc mt6358_bat_desc = {
	.name = "mt6358_battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6358_bat_props,
	.num_properties = ARRAY_SIZE(mt6358_bat_props),
	.get_property = mt6358_bat_get_property,
};

static int mt6358_bat_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config cfg = {};
	struct mt6358_bat *bat;

	bat = devm_kzalloc(&pdev->dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->dev = &pdev->dev;
	bat->regmap = mt6397->regmap;

	cfg.drv_data = bat;
	cfg.fwnode = dev_fwnode(&pdev->dev);

	bat->psy = devm_power_supply_register(&pdev->dev, &mt6358_bat_desc,
					      &cfg);
	if (IS_ERR(bat->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(bat->psy),
				     "Failed to register battery\n");

	platform_set_drvdata(pdev, bat);
	return 0;
}

static const struct of_device_id mt6358_bat_of_match[] = {
	{ .compatible = "mediatek,mt6358-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6358_bat_of_match);

static struct platform_driver mt6358_bat_driver = {
	.probe = mt6358_bat_probe,
	.driver = {
		.name = "mt6358-battery",
		.of_match_table = mt6358_bat_of_match,
	},
};
module_platform_driver(mt6358_bat_driver);

MODULE_AUTHOR("X676B mainline port");
MODULE_DESCRIPTION("MT6358 PMIC battery monitor");
MODULE_LICENSE("GPL");
