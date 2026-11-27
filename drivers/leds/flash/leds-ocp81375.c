// SPDX-License-Identifier: GPL-2.0-only
/*
 * Orient-Chip OCP81375 dual LED flash driver.
 *
 * The OCP81375 is a dual 2 A high-side current source with a synchronous
 * boost converter, controlled over I2C.  It drives the two rear flash LEDs
 * of the Infinix X676B (MT6789): the main LED on LED1 and the auxiliary LED
 * on LED2.  Flash and torch levels are 7-bit current codes; the hardware
 * flash timeout is programmed to 400 ms, exactly like the vendor stack.
 *
 * Level mapping: the vendor HAL exposes 30 flash steps (32..1917 mA) and
 * 7 torch steps (32..422 mA).  The current register is linear over 128
 * codes with a full-scale of about 2 A, so a vendor step n maps to code
 * 2 + round(120 * n / 29).  Both channels use the same scale; torch levels
 * are limited to 6 to stay below the 500 mA torch limit of the project.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-flash.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>

#define OCP81375_REG_ENABLE		0x01
#define OCP81375_REG_IVFM		0x02
#define OCP81375_REG_FLASH_LED1		0x03
#define OCP81375_REG_FLASH_LED2		0x04
#define OCP81375_REG_TORCH_LED1		0x05
#define OCP81375_REG_TORCH_LED2		0x06
#define OCP81375_REG_BOOST		0x07
#define OCP81375_REG_TIMING		0x08
#define OCP81375_REG_TEMP		0x09
#define OCP81375_REG_FLAG1		0x0a
#define OCP81375_REG_FLAG2		0x0b
#define OCP81375_REG_DEVICE_ID		0x0c

#define OCP81375_EN_LED1		BIT(0)
#define OCP81375_EN_LED2		BIT(1)
#define OCP81375_EN_FLASH		BIT(2)
#define OCP81375_EN_GLOBAL		BIT(3)

#define OCP81375_LEVEL_MASK		0x7f
/* bit 7 makes LED2 follow LED1; clear it to drive the channels apart */
#define OCP81375_FOLLOW_MASK		0x80

#define OCP81375_MAX_FLASH_LEVEL	29
#define OCP81375_MAX_TORCH_LEVEL	6
#define OCP81375_MAX_CODE		OCP81375_LEVEL_MASK

/* 400 ms hardware flash timeout: 0x0f in the vendor stack */
#define OCP81375_TIMING_400MS		0x0f
#define OCP81375_BOOST_INIT		0x09

#define OCP81375_FLASH_TIMEOUT_US	400000

struct ocp81375;

struct ocp81375_led {
	struct led_classdev_flash fled_cdev;
	struct ocp81375 *chip;
	u8 num;			/* 0 = LED1, 1 = LED2 */
	u8 mode;		/* 0 = off, 1 = torch, 2 = flash */
	u32 flash_level;
	u32 timeout_us;
};

struct ocp81375 {
	struct i2c_client *client;
	struct mutex lock;
	struct gpio_desc *enable_gpio;
	struct ocp81375_led leds[2];
};

static inline struct ocp81375_led *fled_to_led(struct led_classdev_flash *fled)
{
	return container_of(fled, struct ocp81375_led, fled_cdev);
}

static inline struct ocp81375_led *led_cdev_to_led(struct led_classdev *led_cdev)
{
	return container_of(led_cdev, struct ocp81375_led, fled_cdev.led_cdev);
}

static int ocp81375_write(struct ocp81375 *chip, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(chip->client, reg, val);
}

static int ocp81375_read(struct ocp81375 *chip, u8 reg)
{
	return i2c_smbus_read_byte_data(chip->client, reg);
}

static int ocp81375_update_bits(struct ocp81375 *chip, u8 reg, u8 mask, u8 val)
{
	int old = ocp81375_read(chip, reg);

	if (old < 0)
		return old;

	return ocp81375_write(chip, reg, (old & ~mask) | (val & mask));
}

static u8 ocp81375_level_to_code(unsigned int level)
{
	if (level > OCP81375_MAX_FLASH_LEVEL)
		level = OCP81375_MAX_FLASH_LEVEL;

	return 2 + DIV_ROUND_CLOSEST(120 * level, OCP81375_MAX_FLASH_LEVEL);
}

/*
 * The enable register holds one bit per current source, a shared flash
 * mode bit and a global enable.  Compose it from the per-LED state so
 * both torches can be on at the same time.
 */
static int ocp81375_apply(struct ocp81375 *chip)
{
	u8 val = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(chip->leds); i++) {
		if (!chip->leds[i].mode)
			continue;
		val |= i ? OCP81375_EN_LED2 : OCP81375_EN_LED1;
		if (chip->leds[i].mode == 2)
			val |= OCP81375_EN_FLASH;
	}

	if (val)
		val |= OCP81375_EN_GLOBAL;

	return ocp81375_write(chip, OCP81375_REG_ENABLE, val);
}

static u8 ocp81375_level_reg(struct ocp81375_led *led, bool flash)
{
	if (flash)
		return led->num ? OCP81375_REG_FLASH_LED2 : OCP81375_REG_FLASH_LED1;

	return led->num ? OCP81375_REG_TORCH_LED2 : OCP81375_REG_TORCH_LED1;
}

/*
 * LED2 has to be decoupled from LED1, otherwise its current follows the
 * LED1 register (bit 7 of the LED1 level register is set by default).
 */
static int ocp81375_decouple(struct ocp81375_led *led, bool flash)
{
	struct ocp81375 *chip = led->chip;
	u8 reg = flash ? OCP81375_REG_FLASH_LED1 : OCP81375_REG_TORCH_LED1;

	if (!led->num)
		return 0;

	return ocp81375_update_bits(chip, reg, OCP81375_FOLLOW_MASK, 0);
}

static int ocp81375_set_level(struct ocp81375_led *led, unsigned int level,
			      bool flash)
{
	struct ocp81375 *chip = led->chip;
	u8 code = ocp81375_level_to_code(level);
	int ret;

	ret = ocp81375_decouple(led, flash);
	if (ret)
		return ret;

	return ocp81375_write(chip, ocp81375_level_reg(led, flash), code);
}

static int ocp81375_torch_set(struct led_classdev *led_cdev,
			      enum led_brightness brightness)
{
	struct ocp81375_led *led = led_cdev_to_led(led_cdev);
	struct ocp81375 *chip = led->chip;
	int ret;

	if (brightness > OCP81375_MAX_TORCH_LEVEL)
		brightness = OCP81375_MAX_TORCH_LEVEL;

	mutex_lock(&chip->lock);

	if (!brightness) {
		led->mode = 0;
		ret = ocp81375_apply(chip);
		goto out;
	}

	ret = ocp81375_set_level(led, brightness, false);
	if (ret)
		goto out;

	led->mode = 1;
	ret = ocp81375_apply(chip);
out:
	mutex_unlock(&chip->lock);
	return ret;
}

static int ocp81375_flash_brightness_set(struct led_classdev_flash *fled,
					 u32 brightness)
{
	struct ocp81375_led *led = fled_to_led(fled);

	if (brightness > OCP81375_MAX_FLASH_LEVEL)
		brightness = OCP81375_MAX_FLASH_LEVEL;

	led->flash_level = brightness;

	return 0;
}

static int ocp81375_strobe_set(struct led_classdev_flash *fled, bool state)
{
	struct ocp81375_led *led = fled_to_led(fled);
	struct ocp81375 *chip = led->chip;
	int ret;

	mutex_lock(&chip->lock);

	if (!state) {
		led->mode = 0;
		ret = ocp81375_apply(chip);
		goto out;
	}

	if (!led->flash_level) {
		ret = -EINVAL;
		goto out;
	}

	ret = ocp81375_set_level(led, led->flash_level, true);
	if (ret)
		goto out;

	led->mode = 2;
	ret = ocp81375_apply(chip);
out:
	mutex_unlock(&chip->lock);
	return ret;
}

static int ocp81375_strobe_get(struct led_classdev_flash *fled, bool *state)
{
	struct ocp81375_led *led = fled_to_led(fled);
	struct ocp81375 *chip = led->chip;
	int val;

	mutex_lock(&chip->lock);
	val = ocp81375_read(chip, OCP81375_REG_ENABLE);
	mutex_unlock(&chip->lock);

	if (val < 0)
		return val;

	*state = led->mode == 2;

	return 0;
}

static int ocp81375_timeout_set(struct led_classdev_flash *fled, u32 timeout)
{
	struct ocp81375_led *led = fled_to_led(fled);

	/* the hardware timeout stays at 400 ms, like the vendor stack */
	led->timeout_us = timeout;

	return 0;
}

static int ocp81375_fault_get(struct led_classdev_flash *fled, u32 *fault)
{
	struct ocp81375_led *led = fled_to_led(fled);
	struct ocp81375 *chip = led->chip;
	int ret;

	mutex_lock(&chip->lock);
	ret = ocp81375_read(chip, OCP81375_REG_FLAG1);
	if (ret >= 0) {
		*fault = ret;
		ret = ocp81375_read(chip, OCP81375_REG_FLAG2);
		if (ret >= 0)
			*fault |= ret << 8;
	}
	mutex_unlock(&chip->lock);

	return ret < 0 ? ret : 0;
}

static const struct led_flash_ops ocp81375_flash_ops = {
	.flash_brightness_set = ocp81375_flash_brightness_set,
	.strobe_set = ocp81375_strobe_set,
	.strobe_get = ocp81375_strobe_get,
	.timeout_set = ocp81375_timeout_set,
	.fault_get = ocp81375_fault_get,
};

static int ocp81375_hw_init(struct ocp81375 *chip)
{
	int ret;

	if (chip->enable_gpio) {
		gpiod_set_value_cansleep(chip->enable_gpio, 1);
		msleep(20);
	}

	ret = ocp81375_write(chip, OCP81375_REG_ENABLE, 0x00);
	if (ret)
		return ret;

	ret = ocp81375_write(chip, OCP81375_REG_BOOST, OCP81375_BOOST_INIT);
	if (ret)
		return ret;

	return ocp81375_write(chip, OCP81375_REG_TIMING,
			      OCP81375_TIMING_400MS);
}

static int ocp81375_register_led(struct ocp81375 *chip,
				 struct fwnode_handle *fwnode, u8 num)
{
	struct ocp81375_led *led = &chip->leds[num];
	struct led_init_data init_data = {};
	struct led_flash_setting *setting;

	led->chip = chip;
	led->num = num;
	led->flash_level = 0;
	led->timeout_us = OCP81375_FLASH_TIMEOUT_US;

	led->fled_cdev.ops = &ocp81375_flash_ops;

	setting = &led->fled_cdev.brightness;
	setting->min = 0;
	setting->max = OCP81375_MAX_FLASH_LEVEL;
	setting->step = 1;
	setting->val = OCP81375_MAX_FLASH_LEVEL;

	setting = &led->fled_cdev.timeout;
	setting->min = OCP81375_FLASH_TIMEOUT_US;
	setting->max = OCP81375_FLASH_TIMEOUT_US;
	setting->step = 1;
	setting->val = OCP81375_FLASH_TIMEOUT_US;

	led->fled_cdev.led_cdev.brightness_set_blocking = ocp81375_torch_set;
	led->fled_cdev.led_cdev.max_brightness = OCP81375_MAX_TORCH_LEVEL;
	led->fled_cdev.led_cdev.flags |= LED_DEV_CAP_FLASH;

	init_data.fwnode = fwnode;
	init_data.devicename = "ocp81375";
	init_data.default_label = num ? "aux:flash" : "main:flash";

	return devm_led_classdev_flash_register_ext(&chip->client->dev,
						    &led->fled_cdev,
						    &init_data);
}

static int ocp81375_probe(struct i2c_client *client)
{
	struct ocp81375 *chip;
	int ret, id;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	chip->enable_gpio = devm_gpiod_get_optional(&client->dev, "enable",
						    GPIOD_OUT_LOW);

	ret = ocp81375_hw_init(chip);
	if (ret)
		return dev_err_probe(&client->dev, ret, "chip init failed\n");

	id = ocp81375_read(chip, OCP81375_REG_DEVICE_ID);
	if (id < 0)
		return dev_err_probe(&client->dev, id, "device does not answer\n");
	dev_info(&client->dev, "OCP81375 detected, id 0x%02x\n", id);

	ret = ocp81375_register_led(chip, NULL, 0);
	if (ret)
		return ret;

	return ocp81375_register_led(chip, NULL, 1);
}

static void ocp81375_remove(struct i2c_client *client)
{
	struct ocp81375 *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	ocp81375_write(chip, OCP81375_REG_ENABLE, 0x00);
	mutex_unlock(&chip->lock);

	if (chip->enable_gpio)
		gpiod_set_value_cansleep(chip->enable_gpio, 0);
}

static const struct i2c_device_id ocp81375_id[] = {
	{ "ocp81375", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ocp81375_id);

static const struct of_device_id ocp81375_of_match[] = {
	{ .compatible = "orientchip,ocp81375" },
	{ .compatible = "mediatek,strobe_main" },
	{ }
};
MODULE_DEVICE_TABLE(of, ocp81375_of_match);

static struct i2c_driver ocp81375_driver = {
	.driver = {
		.name = "ocp81375",
		.of_match_table = ocp81375_of_match,
	},
	.probe = ocp81375_probe,
	.remove = ocp81375_remove,
	.id_table = ocp81375_id,
};
module_i2c_driver(ocp81375_driver);

MODULE_DESCRIPTION("Orient-Chip OCP81375 dual LED flash driver");
MODULE_LICENSE("GPL");
