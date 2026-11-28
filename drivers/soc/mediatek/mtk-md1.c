// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6789 MD1 (integrated modem) bring-up.
 *
 * The modem is a separate MIPS/nanoMIPS subsystem with its own boot ROM.
 * LK normally performs the DRAM bank remap, loads md1rom/md1dsp, stores
 * the AP<->MD shared memory base in ATF and locks the remap.  This driver
 * performs the runtime half of the sequence:
 *
 *   regulators -> SPM srclken_o1 -> power domain off/iso/on
 *   -> flight mode -> let the boot ROM go -> boot status
 *
 * It also creates the CCCI ring buffers in the reserved CCISM memory and
 * can ring the CCIF doorbell.  The full CCCI handshake still needs the
 * LK boot tags; see docs/AGENTS/x676b_modem_port.md.
 */
#include <linux/arm-smccc.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define SPM_MD1_PWR_CON		0x0300
#define SPM_MD_EXT_BUCK		0x0398
#define SPM_PWR_STATUS		0x016c
#define SPM_PWR_STATUS2		0x0170
#define SPM_O1_CON		0x0000
#define SPM_O1_EN		0x0008

#define CCISM_AP		0x8e829000UL
#define CCISM_SIZE		0xb4400
#define CCISM_EXP_AP		0x8e8dd400UL
#define CCISM_EXP_SIZE		0x1e400
#define MDSS_DBG_MD		0x408dd400UL

#define CCIF_SRAM_SIZE		512
#define CCIF_CHDATA		0x100
#define RBF_HEADER		0xEE0000EE
#define RBF_FOOTER		0xFF0000FF
#define RBF_CTL_LEN		40
#define QUEUE_NUM		16
#define H2D_EXCEPTION_ACK	16

#define MTK_SIP_KERNEL_CCCI_CONTROL \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, \
			   ARM_SMCCC_OWNER_SIP, 0x505)
#define MD_POWER_CONFIG		6
#define MD_FLIGHT_MODE_SET	7
#define MD_KERNEL_BOOT_UP	0
#define MD_BOOT_STATUS		4
#define SCP_CLK_SET_DONE	13

#define MD_BOOT_OK		0x5443000c

struct mtk_md1 {
	struct device *dev;
	struct regmap *spm;
	struct regulator *vmodem;
	struct regulator *vsram;
	void __iomem *ccif_ap;
	void __iomem *ccif_md;
	void __iomem *ccism;
	void __iomem *ccism_exp;
	struct dentry *debugfs;
	u32 boot_status0;
	u32 boot_status1;
	bool powered;
};

static const int rx_n[QUEUE_NUM] = {
	80 * 1024, 80 * 1024, 40 * 1024, 80 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
	8 * 1024, 0, 0, 0, 0, 0, 0, 0,
};
static const int tx_n[QUEUE_NUM] = {
	128 * 1024, 40 * 1024, 8 * 1024, 40 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
	8 * 1024, 0, 0, 0, 0, 0, 0, 0,
};
static const int rx_e[QUEUE_NUM] = {
	12 * 1024, 32 * 1024, 8 * 1024, 0,
	0, 0, 8 * 1024, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void md1_smc(struct mtk_md1 *md, unsigned long a1, unsigned long a2,
		    unsigned long a3, struct arm_smccc_res *res)
{
	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, a1, a2, a3, 0, 0, 0, 0,
		      res);
}

static int md1_regulators_on(struct mtk_md1 *md)
{
	int ret;

	ret = regulator_set_voltage(md->vmodem, 825000, 825000);
	if (ret)
		return ret;
	ret = regulator_set_voltage(md->vsram, 825000, 825000);
	if (ret)
		return ret;
	ret = regulator_enable(md->vmodem);
	if (ret)
		return ret;
	ret = regulator_enable(md->vsram);
	if (ret)
		return ret;
	regulator_sync_voltage(md->vmodem);
	regulator_sync_voltage(md->vsram);
	msleep(10);
	return 0;
}

static int md1_wait_ack(struct mtk_md1 *md)
{
	unsigned int sta, sta2;
	int i;

	for (i = 0; i < 200; i++) {
		regmap_read(md->spm, SPM_PWR_STATUS, &sta);
		regmap_read(md->spm, SPM_PWR_STATUS2, &sta2);
		if ((sta & BIT(0)) && (sta2 & BIT(0)))
			return 0;
		usleep_range(1000, 2000);
	}
	return -ETIMEDOUT;
}

static int md1_power_on(struct mtk_md1 *md)
{
	struct arm_smccc_res res;
	unsigned int v;
	int ret;

	ret = md1_regulators_on(md);
	if (ret)
		return ret;

	/* SPM srclken_o1, as in LK ccci_md_power_on(). */
	regmap_write(md->spm, SPM_O1_CON, 0x0b160001);
	regmap_read(md->spm, SPM_O1_EN, &v);
	regmap_write(md->spm, SPM_O1_EN, v | 0x200000);

	/* md_cd_power_off(): flight mode before the domain goes down. */
	md1_smc(md, MD_FLIGHT_MODE_SET, 1, 0, &res);
	regmap_read(md->spm, SPM_MD1_PWR_CON, &v);
	regmap_write(md->spm, SPM_MD1_PWR_CON, v & ~BIT(2));
	msleep(20);
	regmap_read(md->spm, SPM_MD_EXT_BUCK, &v);
	regmap_write(md->spm, SPM_MD_EXT_BUCK, v | 3);
	regmap_read(md->spm, SPM_MD1_PWR_CON, &v);
	regmap_write(md->spm, SPM_MD1_PWR_CON, v & ~BIT(0));
	msleep(20);

	/* md_cd_power_on(): flight mode off, then RST_B and PWR_ON. */
	md1_smc(md, MD_FLIGHT_MODE_SET, 0, 0, &res);
	regmap_read(md->spm, SPM_MD_EXT_BUCK, &v);
	regmap_write(md->spm, SPM_MD_EXT_BUCK, v & ~3);
	regmap_read(md->spm, SPM_MD1_PWR_CON, &v);
	regmap_write(md->spm, SPM_MD1_PWR_CON, v | BIT(0));
	udelay(1);
	regmap_write(md->spm, SPM_MD1_PWR_CON, v | BIT(0) | BIT(2));

	ret = md1_wait_ack(md);
	if (ret) {
		dev_err(md->dev, "MD1 power ack timeout\n");
		return ret;
	}
	md->powered = true;

	/* Publish the AP<->MD shared memory base (CCCI op 13). */
	md1_smc(md, SCP_CLK_SET_DONE, 0, 0, &res);
	dev_info(md->dev, "MD1 powered, PWR_ACK ok\n");
	return 0;
}

static int md1_let_go(struct mtk_md1 *md)
{
	struct arm_smccc_res res;
	int i;

	md1_smc(md, MD_POWER_CONFIG, MD_KERNEL_BOOT_UP, 0, &res);
	for (i = 0; i < 50; i++) {
		md1_smc(md, MD_POWER_CONFIG, MD_BOOT_STATUS, 0, &res);
		md->boot_status0 = res.a1;
		md->boot_status1 = res.a2;
		if (md->boot_status0)
			break;
		usleep_range(10000, 12000);
	}
	dev_info(md->dev, "MD1 boot status %08x/%08x, boot_slave=%ld\n",
		 md->boot_status0, md->boot_status1, res.a3);
	return md->boot_status0 == MD_BOOT_OK ? 0 : -EIO;
}

static void md1_ring(void __iomem *base, int rx, int tx)
{
	int len = RBF_CTL_LEN + rx + tx;

	memset_io(base, 0, len);
	writel(RBF_HEADER, base);
	writel(RBF_HEADER, base + 4);
	writel(rx, base + 16);
	writel(tx, base + 28);
	writel(RBF_FOOTER, base + len - 8);
	writel(RBF_FOOTER, base + len - 4);
}

static void md1_ccif_init(struct mtk_md1 *md)
{
	void __iomem *p = md->ccism;
	void __iomem *e = md->ccism_exp;
	int i;

	for (i = 0; i < QUEUE_NUM; i++) {
		md1_ring(p, rx_n[i], tx_n[i]);
		p += RBF_CTL_LEN + rx_n[i] + tx_n[i];
		md1_ring(e, rx_e[i], rx_e[i]);
		e += RBF_CTL_LEN + 2 * rx_e[i];
	}

	memset_io(md->ccif_ap + CCIF_CHDATA, 0, CCIF_SRAM_SIZE);
	writel(0x7274626e, md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 12);
	writel(MDSS_DBG_MD, md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 8);
	writel(CCISM_EXP_SIZE, md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 4);

	writel(0x00007fff, md->ccif_ap + 0x24);
	writel(0x00000000, md->ccif_ap + 0x20);
}

static int md1_trigger(struct mtk_md1 *md)
{
	int i, rchnum = 0;

	writel(1 << H2D_EXCEPTION_ACK, md->ccif_ap + 0x04);
	for (i = 0; i < 2000; i++) {
		rchnum = readl(md->ccif_md + 0x10);
		if (rchnum)
			break;
		usleep_range(1000, 2000);
	}
	dev_info(md->dev, "CCIF doorbell: md_rchnum=%08x after %d ms\n",
		 rchnum, i);
	return rchnum ? 0 : -ETIMEDOUT;
}

static ssize_t power_write(struct file *f, const char __user *buf,
			   size_t len, loff_t *off)
{
	struct mtk_md1 *md = f->private_data;
	int ret = md1_power_on(md);

	if (!ret)
		ret = md1_let_go(md);
	return ret ? ret : len;
}

static const struct file_operations power_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = power_write,
};

static ssize_t trigger_write(struct file *f, const char __user *buf,
			     size_t len, loff_t *off)
{
	struct mtk_md1 *md = f->private_data;
	int ret = md1_trigger(md);

	return ret ? ret : len;
}

static const struct file_operations trigger_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = trigger_write,
};

static int md1_probe(struct platform_device *pdev)
{
	struct mtk_md1 *md;
	struct resource *res;

	md = devm_kzalloc(&pdev->dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = &pdev->dev;

	md->spm = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						  "mediatek,spm");
	if (IS_ERR(md->spm))
		return dev_err_probe(&pdev->dev, PTR_ERR(md->spm),
				     "no SPM syscon\n");

	md->vmodem = devm_regulator_get(&pdev->dev, "vmodem");
	if (IS_ERR(md->vmodem))
		return dev_err_probe(&pdev->dev, PTR_ERR(md->vmodem),
				     "no vmodem\n");
	md->vsram = devm_regulator_get(&pdev->dev, "vsram");
	if (IS_ERR(md->vsram))
		return dev_err_probe(&pdev->dev, PTR_ERR(md->vsram),
				     "no vsram\n");

	md->ccif_ap = devm_platform_ioremap_resource_byname(pdev, "ap");
	if (IS_ERR(md->ccif_ap))
		return PTR_ERR(md->ccif_ap);
	md->ccif_md = devm_platform_ioremap_resource_byname(pdev, "md");
	if (IS_ERR(md->ccif_md))
		return PTR_ERR(md->ccif_md);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ccism");
	if (!res)
		return -EINVAL;
	md->ccism = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(md->ccism))
		return PTR_ERR(md->ccism);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ccism-exp");
	if (!res)
		return -EINVAL;
	md->ccism_exp = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(md->ccism_exp))
		return PTR_ERR(md->ccism_exp);

	md1_ccif_init(md);
	platform_set_drvdata(pdev, md);

	md->debugfs = debugfs_create_dir("mtk_md1", NULL);
	debugfs_create_file("power", 0200, md->debugfs, md, &power_fops);
	debugfs_create_file("trigger", 0200, md->debugfs, md, &trigger_fops);

	dev_info(&pdev->dev, "MT6789 MD1 ready (power=%d)\n", md->powered);
	return 0;
}

static void md1_remove(struct platform_device *pdev)
{
	struct mtk_md1 *md = platform_get_drvdata(pdev);

	debugfs_remove_recursive(md->debugfs);
}

static const struct of_device_id md1_of_match[] = {
	{ .compatible = "mediatek,mt6789-md1" },
	{ }
};
MODULE_DEVICE_TABLE(of, md1_of_match);

static struct platform_driver md1_driver = {
	.probe = md1_probe,
	.remove = md1_remove,
	.driver = {
		.name = "mtk-md1",
		.of_match_table = md1_of_match,
	},
};
module_platform_driver(md1_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6789 MD1 modem bring-up");
