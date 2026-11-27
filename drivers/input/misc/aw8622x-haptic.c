// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW8622x haptic driver (AW86224/AW86225 branch) for the
 * Infinix X676B.  The chip sits on i2c-6 at 7-bit address 0x58 and is
 * wired as an LRA driver with a single GPIO reset line.
 *
 * The vendor stack drives the chip in RAM mode with the RAM waveform
 * from aw8622x_haptic.bin.  This driver uses the continuous mode instead:
 * the chip generates a sine wave at the LRA resonance set by the
 * calibrated registers, which is enough for "vibrate for N ms" and for
 * input force feedback.  The initial register values are the live dump
 * of the vendor stack on this very device, so the LRA calibration
 * (trim_lra, drv levels, sine array) is kept as Awinic tuned it.
 *
 * Interfaces:
 *  - /sys/class/leds/vibrator/brightness   (1 = on, 0 = off)
 *  - /sys/class/leds/vibrator/duration     (write ms, vibrates and stops)
 *  - /dev/input/eventN (haptic_nv)         (FF_RUMBLE)
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#define AW862XX_REG_ID		0x00
#define AW862XX_REG_SYSINTM	0x03
#define AW862XX_REG_PLAYCFG2	0x07
#define AW862XX_REG_PLAYCFG3	0x08
#define AW862XX_REG_PLAYCFG4	0x09
#define AW862XX_REG_CONTCFG1	0x18
#define AW862XX_REG_SYSCTRL1	0x43
#define AW862XX_REG_SYSCTRL2	0x44
#define AW862XX_REG_GLBRD5	0x3f

#define AW862XX_RESET_VALUE	0xaa

#define PLAYCFG3_MODE_MASK	0x03
#define PLAYCFG3_MODE_STOP	0x03
#define PLAYCFG3_MODE_CONT	0x02
#define PLAYCFG4_GO		0x01

#define SYSCTRL1_RAMINIT	BIT(3)
#define SYSCTRL2_STANDBY	BIT(6)

#define GLBRD_STATE_MASK	0x0f
#define GLBRD_STATE_STANDBY	0x00

#define AW862XX_MAX_DURATION_MS	15000

struct aw8622x {
	struct i2c_client *client;
	struct mutex lock;
	struct gpio_desc *reset_gpio;
	struct led_classdev led;
	struct input_dev *input;
	struct delayed_work stop_work;
	unsigned int play_ms;
	bool playing;
};

/*
 * Register values taken from the live vendor state on the X676B
 * (temp/haptic/reg_dump.txt).  Read-only status registers and the
 * detection result registers are deliberately not written.
 */
static const struct {
	u8 reg;
	u8 val;
} aw8622x_init_table[] = {
	{ 0x03, 0x19 },	/* SYSINTM */
	{ 0x05, 0x00 },	/* SYSER */
	{ 0x07, 0x7c },	/* PLAYCFG2: gain */
	{ 0x08, 0x00 },	/* PLAYCFG3: stop */
	{ 0x0a, 0x04 },	/* WAVCFG1: sequence */
	{ 0x12, 0xf0 },	/* WAVCFG9: loop */
	{ 0x18, 0xe1 },	/* CONTCFG1 */
	{ 0x19, 0x8d },	/* CONTCFG2 */
	{ 0x1a, 0x44 },	/* CONTCFG3: drv width */
	{ 0x1b, 0x06 },	/* CONTCFG4 */
	{ 0x1c, 0x08 },	/* CONTCFG5: brake gain */
	{ 0x1d, 0x7f },	/* CONTCFG6: drv1 level + track */
	{ 0x1e, 0x00 },	/* CONTCFG7: drv2 level */
	{ 0x1f, 0x04 },	/* CONTCFG8: drv1 time */
	{ 0x20, 0x14 },	/* CONTCFG9: drv2 time */
	{ 0x21, 0x06 },	/* CONTCFG10: brake time */
	{ 0x22, 0x12 },	/* CONTCFG11: track margin */
	{ 0x23, 0x0c },	/* CONTCFG12 */
	{ 0x24, 0x62 },	/* CONTCFG13: tset/bemf */
	{ 0x2d, 0x38 },	/* RTPCFG1: 3K SRAM */
	{ 0x2e, 0x00 },	/* RTPCFG2 */
	{ 0x2f, 0x46 },	/* RTPCFG3: fifo */
	{ 0x30, 0x00 },	/* RTPCFG4 */
	{ 0x31, 0x00 },	/* RTPCFG5 */
	{ 0x33, 0x01 },	/* TRGCFG1..6 */
	{ 0x34, 0x01 },
	{ 0x35, 0x01 },
	{ 0x36, 0x01 },
	{ 0x37, 0x01 },
	{ 0x38, 0x01 },
	{ 0x39, 0x22 },	/* TRGCFG7 */
	{ 0x3a, 0x30 },	/* TRGCFG8 */
	{ 0x3c, 0x04 },	/* GLBCFG2 */
	{ 0x3e, 0x1b },	/* GLBCFG4 */
	{ 0x43, 0x84 },	/* SYSCTRL1: vbat hw mode, raminit off */
	{ 0x44, 0x2a },	/* SYSCTRL2: 12 kHz, standby off */
	{ 0x45, 0x05 },	/* sine array */
	{ 0x46, 0xb2 },
	{ 0x47, 0xff },
	{ 0x48, 0xef },
	{ 0x49, 0x55 },	/* SYSCTRL7 */
	{ 0x4c, 0xa0 },	/* PWMCFG1 */
	{ 0x4e, 0xbf },	/* PWMCFG3: protection */
	{ 0x4f, 0x32 },	/* PWMCFG4 */
	{ 0x51, 0x12 },	/* DETCFG1 */
	{ 0x52, 0x00 },	/* DETCFG2 */
	{ 0x5a, 0x31 },	/* TRIMCFG3: f0 calibration */
	{ 0x77, 0xc0 },	/* ANACFG8 */
};

static int aw8622x_write(struct aw8622x *aw, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(aw->client, reg, val);
}

static int aw8622x_read(struct aw8622x *aw, u8 reg)
{
	return i2c_smbus_read_byte_data(aw->client, reg);
}

static int aw8622x_update_bits(struct aw8622x *aw, u8 reg, u8 mask, u8 val)
{
	int old = aw8622x_read(aw, reg);

	if (old < 0)
		return old;

	return aw8622x_write(aw, reg, (old & ~mask) | (val & mask));
}

static int aw8622x_hw_reset(struct aw8622x *aw)
{
	if (!aw->reset_gpio)
		return 0;

	gpiod_set_value_cansleep(aw->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(aw->reset_gpio, 1);
	usleep_range(8000, 8500);

	return 0;
}

static int aw8622x_init_chip(struct aw8622x *aw)
{
	size_t i;
	int ret;

	ret = aw8622x_hw_reset(aw);
	if (ret)
		return ret;

	ret = aw8622x_read(aw, AW862XX_REG_ID);
	if (ret < 0)
		return ret;

	/* software reset, the chip needs a moment before I2C works again */
	ret = aw8622x_write(aw, AW862XX_REG_ID, AW862XX_RESET_VALUE);
	if (ret)
		return ret;
	usleep_range(2000, 2500);

	for (i = 0; i < ARRAY_SIZE(aw8622x_init_table); i++) {
		ret = aw8622x_write(aw, aw8622x_init_table[i].reg,
				    aw8622x_init_table[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static int aw8622x_play(struct aw8622x *aw)
{
	int ret, state;

	/* vendor cont_config: drive levels + full drv2 time, then go */
	ret = aw8622x_write(aw, 0x1d, 0x7f);
	if (ret)
		return ret;
	ret = aw8622x_write(aw, 0x1e, 0x00);
	if (ret)
		return ret;
	ret = aw8622x_write(aw, 0x20, 0xff);
	if (ret)
		return ret;

	/* continuous mode: the chip drives the LRA by itself */
	ret = aw8622x_update_bits(aw, AW862XX_REG_PLAYCFG3,
				  PLAYCFG3_MODE_MASK, PLAYCFG3_MODE_CONT);
	if (ret)
		return ret;

	ret = aw8622x_write(aw, AW862XX_REG_PLAYCFG4, PLAYCFG4_GO);
	if (ret)
		return ret;

	usleep_range(20000, 25000);
	state = aw8622x_read(aw, AW862XX_REG_GLBRD5);
	dev_info(&aw->client->dev, "play: GLBRD5=0x%02x\n", state);

	return 0;
}

static int aw8622x_stop(struct aw8622x *aw)
{
	int state, cnt = 40;
	int ret;

	/* the delayed stop work must be cancelled by the caller, always
	 * before taking aw->lock: the work itself takes that lock
	 */
	ret = aw8622x_update_bits(aw, AW862XX_REG_SYSCTRL1,
				  SYSCTRL1_RAMINIT, SYSCTRL1_RAMINIT);
	if (ret)
		return ret;

	ret = aw8622x_update_bits(aw, AW862XX_REG_PLAYCFG3,
				  PLAYCFG3_MODE_MASK, PLAYCFG3_MODE_STOP);
	if (ret)
		return ret;

	ret = aw8622x_write(aw, AW862XX_REG_PLAYCFG4, PLAYCFG4_GO);
	if (ret)
		return ret;

	aw8622x_update_bits(aw, AW862XX_REG_SYSCTRL1, SYSCTRL1_RAMINIT, 0);

	while (cnt--) {
		state = aw8622x_read(aw, AW862XX_REG_GLBRD5);
		if (state >= 0 && (state & GLBRD_STATE_MASK) == GLBRD_STATE_STANDBY)
			break;
		usleep_range(3000, 3500);
	}

	aw->playing = false;

	return 0;
}

static int aw8622x_start_locked(struct aw8622x *aw, unsigned int ms)
{
	int ret;

	if (ms > AW862XX_MAX_DURATION_MS)
		ms = AW862XX_MAX_DURATION_MS;

	ret = aw8622x_play(aw);
	if (ret)
		return ret;

	aw->playing = true;
	if (ms) {
		aw->play_ms = ms;
		schedule_delayed_work(&aw->stop_work, msecs_to_jiffies(ms));
	}

	return 0;
}

static void aw8622x_stop_work(struct work_struct *work)
{
	struct aw8622x *aw = container_of(to_delayed_work(work),
					  struct aw8622x, stop_work);

	mutex_lock(&aw->lock);
	if (aw->playing)
		aw8622x_stop(aw);
	mutex_unlock(&aw->lock);
}

static int aw8622x_led_brightness_set(struct led_classdev *led,
				      enum led_brightness value)
{
	struct aw8622x *aw = container_of(led, struct aw8622x, led);
	int ret = 0;

	if (!value)
		cancel_delayed_work_sync(&aw->stop_work);

	mutex_lock(&aw->lock);
	if (value && !aw->playing)
		ret = aw8622x_start_locked(aw, 0);
	else if (!value && aw->playing)
		ret = aw8622x_stop(aw);
	mutex_unlock(&aw->lock);

	return ret;
}

static ssize_t duration_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct led_classdev *led = dev_get_drvdata(dev);
	struct aw8622x *aw = container_of(led, struct aw8622x, led);
	unsigned int ms;
	int ret;

	if (kstrtouint(buf, 0, &ms))
		return -EINVAL;

	if (!ms || ms > AW862XX_MAX_DURATION_MS)
		return -EINVAL;

	cancel_delayed_work_sync(&aw->stop_work);

	mutex_lock(&aw->lock);
	if (aw->playing)
		aw8622x_stop(aw);
	ret = aw8622x_start_locked(aw, ms);
	mutex_unlock(&aw->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(duration);

static struct attribute *aw8622x_attrs[] = {
	&dev_attr_duration.attr,
	NULL,
};
ATTRIBUTE_GROUPS(aw8622x);

/* input FF: rumble while magnitude is non-zero */
static int aw8622x_ff_play(struct input_dev *input, void *data,
			   struct ff_effect *effect)
{
	struct aw8622x *aw = input_get_drvdata(input);
	bool on = effect->u.rumble.strong_magnitude ||
		  effect->u.rumble.weak_magnitude;
	int ret = 0;

	if (!on)
		cancel_delayed_work_sync(&aw->stop_work);

	mutex_lock(&aw->lock);
	if (on && !aw->playing)
		ret = aw8622x_start_locked(aw, 0);
	else if (!on && aw->playing)
		ret = aw8622x_stop(aw);
	mutex_unlock(&aw->lock);

	return ret;
}

static int aw8622x_register_input(struct aw8622x *aw)
{
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(&aw->client->dev);
	if (!input)
		return -ENOMEM;

	input->name = "haptic_nv";
	input->phys = "aw8622x/input0";
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, aw);

	input_set_capability(input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(input, NULL, aw8622x_ff_play);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret)
		return ret;

	aw->input = input;

	return 0;
}

static int aw8622x_probe(struct i2c_client *client)
{
	struct aw8622x *aw;
	int ret;

	aw = devm_kzalloc(&client->dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->client = client;
	mutex_init(&aw->lock);
	INIT_DELAYED_WORK(&aw->stop_work, aw8622x_stop_work);
	i2c_set_clientdata(client, aw);

	aw->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
						 GPIOD_OUT_HIGH);

	ret = aw8622x_init_chip(aw);
	if (ret)
		return dev_err_probe(&client->dev, ret, "chip init failed\n");

	ret = aw8622x_stop(aw);
	if (ret)
		return ret;

	aw->led.name = "vibrator";
	aw->led.max_brightness = 1;
	aw->led.brightness_set_blocking = aw8622x_led_brightness_set;
	aw->led.groups = aw8622x_groups;
	ret = devm_led_classdev_register(&client->dev, &aw->led);
	if (ret)
		return ret;

	ret = aw8622x_register_input(aw);
	if (ret)
		return ret;

	dev_info(&client->dev, "AW8622x haptic ready\n");

	return 0;
}

static void aw8622x_remove(struct i2c_client *client)
{
	struct aw8622x *aw = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&aw->stop_work);

	mutex_lock(&aw->lock);
	if (aw->playing)
		aw8622x_stop(aw);
	aw8622x_write(aw, AW862XX_REG_SYSCTRL2, SYSCTRL2_STANDBY);
	mutex_unlock(&aw->lock);
}

static const struct i2c_device_id aw8622x_id[] = {
	{ "haptic_nv", 0 },
	{ "aw8622x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw8622x_id);

static const struct of_device_id aw8622x_of_match[] = {
	{ .compatible = "awinic,haptic_nv" },
	{ .compatible = "awinic,aw8622x" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8622x_of_match);

static struct i2c_driver aw8622x_driver = {
	.driver = {
		.name = "aw8622x-haptic",
		.of_match_table = aw8622x_of_match,
	},
	.probe = aw8622x_probe,
	.remove = aw8622x_remove,
	.id_table = aw8622x_id,
};
module_i2c_driver(aw8622x_driver);

MODULE_DESCRIPTION("Awinic AW8622x LRA haptic driver");
MODULE_LICENSE("GPL");
