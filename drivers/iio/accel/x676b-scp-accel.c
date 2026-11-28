// SPDX-License-Identifier: GPL-2.0
/*
 * X676B (MT6789) SCP sensorhub accelerometer via share memory.
 *
 * The tinysys SCP runs the sensorhub middleware and writes sensor records to
 * a ring buffer in the 0x8f000000 share-memory pool.  The vendor stack needs
 * an IPI doorbell to notice new records; this driver polls the ring instead,
 * which makes the accelerometer usable even before the SCP->AP mailbox path
 * is fully understood.
 *
 * Ring:  0x8f24e300, header {rp, wp, buffer_size, item_size=0x24}, data+0x10
 * Record: sensor_type u8, action u8, accuracy u8, pad u8, timestamp s64,
 *         value[6] s32.  For the MXC4005X (sensor_type 1) value[3..5] are
 *         floats in m/s^2.
 * List:  0x8f515e00, header {rp, wp, buffer_size, item_size=0x28}; entries
 *         are {sensor_type u8, pad[3], gain u32, name[16], vendor[16]}.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>

#define X676B_SHARE_BASE	0x8f000000UL
#define X676B_SHARE_SIZE	0x5a0000UL
#define X676B_SENS_RING_OFF	0x24e300UL
#define X676B_SENS_LIST_OFF	0x515e00UL

#define X676B_RING_HDR_SIZE	0x10
#define X676B_RING_ITEM_SIZE	0x24
#define X676B_LIST_ITEM_SIZE	0x28

struct x676b_accel {
	struct device *dev;
	void __iomem *share;
	struct iio_dev *indio;
	struct delayed_work poll_work;
	struct dentry *debugfs;
	struct mutex lock;
	u32 sensor_type;
	u32 gain;
	s32 latest[3];
	s64 timestamp;
	unsigned int poll_ms;
	bool force;
};

static unsigned int poll_ms = 20;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Ring poll interval in ms (0 disables polling)");

static bool force;
module_param(force, bool, 0644);
MODULE_PARM_DESC(force, "Parse stale records even when rp == wp");

static u32 rd32(void __iomem *p)
{
	return readl(p);
}

static void wr32(void __iomem *p, u32 v)
{
	writel(v, p);
}

static int x676b_find_accel(struct x676b_accel *acc)
{
	void __iomem *list = acc->share + X676B_SENS_LIST_OFF;
	u8 name[16];
	unsigned int i, j;
	u32 wp;

	/* Search the list data area for the MXC4005X name. */
	for (i = 0; i < 0x1000 - 8; i++) {
		bool match = true;

		for (j = 0; j < 4; j++) {
			if (readb(list + X676B_RING_HDR_SIZE + i + 8 + j) !=
			    "mxc4"[j]) {
				match = false;
				break;
			}
		}
		if (!match)
			continue;
		acc->sensor_type = readb(list + X676B_RING_HDR_SIZE + i);
		acc->gain = rd32(list + X676B_RING_HDR_SIZE + i + 4);
		for (j = 0; j < sizeof(name) - 1; j++)
			name[j] = readb(list + X676B_RING_HDR_SIZE + i + 8 + j);
		name[sizeof(name) - 1] = 0;
		dev_info(acc->dev, "found %s type=%u gain=%u\n",
			 name, acc->sensor_type, acc->gain);
		return 0;
	}
	wp = rd32(list + 4);
	dev_warn(acc->dev, "accel name not found (list wp=%#x), assuming type 1\n", wp);
	acc->sensor_type = 1;
	acc->gain = 1000;
	return 0;
}

static void x676b_read_ring(struct x676b_accel *acc)
{
	void __iomem *ring = acc->share + X676B_SENS_RING_OFF;
	void __iomem *data = ring + X676B_RING_HDR_SIZE;
	u32 rp = rd32(ring + 0);
	u32 wp = rd32(ring + 4);
	u32 capacity = rd32(ring + 8);
	u32 item = rd32(ring + 12);
	u32 pos, n;

	if (item != X676B_RING_ITEM_SIZE || capacity > 0x100000) {
		dev_warn_once(acc->dev, "bad ring: item=%u cap=%u\n", item, capacity);
		return;
	}
	if (rp == wp && !acc->force)
		return;
	if (rp == wp)
		rp = (wp >= 8 * item) ? wp - 8 * item : 0;
	if (wp > capacity)
		wp = capacity;

	n = (wp >= rp) ? (wp - rp) / item : (capacity - rp) / item;
	if (!n && acc->force)
		n = 8;
	pos = rp;

	while (n--) {
		u8 type = readb(data + pos);
		u8 action = readb(data + pos + 1);
		s64 ts = 0;
		s32 v0, v1, v2;
		u32 gain = acc->gain ? acc->gain : 1000;

		if (type == acc->sensor_type && action == 0) {
			memcpy_fromio(&ts, data + pos + 4, sizeof(ts));
			memcpy_fromio(&v0, data + pos + 0x0c, 4);
			memcpy_fromio(&v1, data + pos + 0x10, 4);
			memcpy_fromio(&v2, data + pos + 0x14, 4);
			/* value[0..2] are raw milli-units; gain -> m/s^2. */
			acc->latest[0] = div_s64((s64)v0 * 1000, gain);
			acc->latest[1] = div_s64((s64)v1 * 1000, gain);
			acc->latest[2] = div_s64((s64)v2 * 1000, gain);
			acc->timestamp = ts;
		}
		pos += item;
		if (pos >= capacity)
			pos = 0;
	}
	if (rp != wp)
		wr32(ring + 0, wp); /* publish the read position */
}

static void x676b_poll_work(struct work_struct *work)
{
	struct x676b_accel *acc = container_of(to_delayed_work(work),
					      struct x676b_accel, poll_work);

	mutex_lock(&acc->lock);
	x676b_read_ring(acc);
	mutex_unlock(&acc->lock);

	if (acc->poll_ms)
		schedule_delayed_work(&acc->poll_work,
				      msecs_to_jiffies(acc->poll_ms));
}

static int x676b_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct x676b_accel *acc = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW: {
		int idx = 0;

		if (chan->channel2 == IIO_MOD_Y)
			idx = 1;
		else if (chan->channel2 == IIO_MOD_Z)
			idx = 2;
		mutex_lock(&acc->lock);
		*val = acc->latest[idx];
		mutex_unlock(&acc->lock);
		return IIO_VAL_INT;
	}
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 1000; /* milli-m/s^2 */
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = acc->poll_ms ? 1000 / acc->poll_ms : 0;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static void x676b_name(char *dst, size_t n, void __iomem *src)
{
	size_t i;

	for (i = 0; i + 1 < n; i++) {
		u8 c = readb(src + i);

		if (!c)
			break;
		dst[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	dst[i] = 0;
}

static int x676b_debug_show(struct seq_file *s, void *unused)
{
	struct x676b_accel *acc = s->private;
	void __iomem *list = acc->share + X676B_SENS_LIST_OFF;
	void __iomem *ring = acc->share + X676B_SENS_RING_OFF;
	void __iomem *data = ring + X676B_RING_HDR_SIZE;
	u32 rp, wp, cap, item, pos, start;
	char name[17], vendor[17];
	int i;

	seq_printf(s, "list: rp=%u wp=%u size=%u item=%u\n",
		   rd32(list + 0), rd32(list + 4), rd32(list + 8), rd32(list + 12));
	for (i = 0; i < 32; i++) {
		void __iomem *e = list + X676B_RING_HDR_SIZE + i * X676B_LIST_ITEM_SIZE;
		u8 type = readb(e);

		if (type == 0 || type == 0xff)
			break;
		x676b_name(name, sizeof(name), e + 8);
		x676b_name(vendor, sizeof(vendor), e + 24);
		seq_printf(s, "  type=%u gain=%u name=%s vendor=%s\n",
			   type, rd32(e + 4), name, vendor);
	}

	rp = rd32(ring + 0);
	wp = rd32(ring + 4);
	cap = rd32(ring + 8);
	item = rd32(ring + 12);
	seq_printf(s, "ring: rp=%u wp=%u size=%u item=%u\n", rp, wp, cap, item);
	if (item != X676B_RING_ITEM_SIZE || wp > cap)
		return 0;
	start = (wp >= 8 * item) ? wp - 8 * item : 0;
	for (pos = start; pos < wp; pos += item) {
		u8 type = readb(data + pos);
		u8 action = readb(data + pos + 1);
		s64 ts;
		s32 v[6];
		int j;

		memcpy_fromio(&ts, data + pos + 4, sizeof(ts));
		for (j = 0; j < 6; j++)
			memcpy_fromio(&v[j], data + pos + 12 + j * 4, 4);
		seq_printf(s, "  type=%u act=%u ts=%lld v=%d,%d,%d,%d,%d,%d\n",
			   type, action, ts, v[0], v[1], v[2], v[3], v[4], v[5]);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(x676b_debug);

static const struct iio_info x676b_info = {
	.read_raw = x676b_read_raw,
};

#define X676B_CHAN(_axis) {					\
	.type = IIO_ACCEL,					\
	.modified = 1,						\
	.channel2 = IIO_MOD_##_axis,				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |	\
				    BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.scan_index = 0,					\
	.scan_type = { .sign = 's', .realbits = 32,		\
		       .storagebits = 32, .endianness = IIO_LE }, \
}

static const struct iio_chan_spec x676b_channels[] = {
	X676B_CHAN(X),
	X676B_CHAN(Y),
	X676B_CHAN(Z),
};

static int x676b_probe(struct platform_device *pdev)
{
	struct x676b_accel *acc;
	struct iio_dev *indio;
	int ret;

	indio = devm_iio_device_alloc(&pdev->dev, sizeof(*acc));
	if (!indio)
		return -ENOMEM;
	acc = iio_priv(indio);
	acc->dev = &pdev->dev;
	acc->poll_ms = poll_ms;
	acc->force = force;
	mutex_init(&acc->lock);
	INIT_DELAYED_WORK(&acc->poll_work, x676b_poll_work);

	acc->share = devm_ioremap_wc(&pdev->dev, X676B_SHARE_BASE,
				     X676B_SHARE_SIZE);
	if (!acc->share)
		return -ENOMEM;

	ret = x676b_find_accel(acc);
	if (ret)
		return ret;

	x676b_read_ring(acc);

	indio->name = "x676b-scp-accel";
	indio->info = &x676b_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = x676b_channels;
	indio->num_channels = ARRAY_SIZE(x676b_channels);
	platform_set_drvdata(pdev, indio);
	acc->indio = indio;

	ret = devm_iio_device_register(&pdev->dev, indio);
	if (ret)
		return ret;

	acc->debugfs = debugfs_create_file("x676b_scp_accel", 0444, NULL,
					   acc, &x676b_debug_fops);

	if (acc->poll_ms)
		schedule_delayed_work(&acc->poll_work,
				      msecs_to_jiffies(acc->poll_ms));
	dev_info(&pdev->dev, "registered, type=%u poll=%ums\n",
		 acc->sensor_type, acc->poll_ms);
	return 0;
}

static void x676b_remove(struct platform_device *pdev)
{
	struct iio_dev *indio = platform_get_drvdata(pdev);
	struct x676b_accel *acc = iio_priv(indio);

	cancel_delayed_work_sync(&acc->poll_work);
	debugfs_remove(acc->debugfs);
}

static struct platform_driver x676b_driver = {
	.probe = x676b_probe,
	.remove = x676b_remove,
	.driver = {
		.name = "x676b-scp-accel",
	},
};

static struct platform_device *x676b_pdev;

static int __init x676b_accel_init(void)
{
	int ret;

	ret = platform_driver_register(&x676b_driver);
	if (ret)
		return ret;
	x676b_pdev = platform_device_register_simple("x676b-scp-accel", -1,
						     NULL, 0);
	if (IS_ERR(x676b_pdev)) {
		platform_driver_unregister(&x676b_driver);
		return PTR_ERR(x676b_pdev);
	}
	return 0;
}

static void __exit x676b_accel_exit(void)
{
	platform_device_unregister(x676b_pdev);
	platform_driver_unregister(&x676b_driver);
}
module_init(x676b_accel_init);
module_exit(x676b_accel_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("X676B SCP share-memory accelerometer");
