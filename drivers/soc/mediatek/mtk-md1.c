// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6789 MD1 (integrated modem) bring-up.
 *
 * The modem is a separate MIPS/nanoMIPS subsystem with its own boot ROM.
 * LK normally performs the DRAM bank remap, loads md1rom/md1dsp, stores
 * the AP<->MD shared memory base in ATF and locks the remap.  This driver
 * performs the runtime half of the sequence:
 *
 *   regulators -> CCIF clocks/rings/magic -> SPM srclken_o1
 *   -> power domain off/iso/on -> flight mode -> let the boot ROM go
 *   -> boot status -> wait for the CCCI doorbell
 *
 * The LK boot tags (smem_layout, hdr_tbl_inf, ...) are consumed by the
 * Linux CCCI stack, not by md1rom: the modem firmware image contains none
 * of those names.  The modem uses the fixed MT6789 SMEM layout, so the
 * important part is to put CCISM/CCIF into the state the stock kernel
 * uses before releasing the modem.
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/wait.h>

#define SPM_MD1_PWR_CON		0x0300
#define SPM_MD_EXT_BUCK		0x0398
#define SPM_PWR_STATUS		0x016c
#define SPM_PWR_STATUS2		0x0170
#define SPM_O1_CON		0x0000
#define SPM_O1_EN		0x0008

#define INFRA_AO_CCIF_RST_SET	0x0150
#define INFRA_AO_CCIF_RST_CLR	0x0154
#define INFRA_AO_CCIF_RST_BIT	BIT(8)
#define INFRA_AO_CLK_STATUS0	0x0094
#define INFRA_AO_CLK_STATUS1	0x00c8

#define CCISM_AP		0x8e829000UL
#define CCISM_SIZE		0xb4400
#define CCISM_EXP_AP		0x8e8dd400UL
#define CCISM_EXP_SIZE		0x1e400
/* SMEM_USER_RAW_MDSS_DBG: AP 0x8e800800, MD view 0x40800800 */
#define MDSS_DBG_MD		0x40800800UL
#define MDSS_DBG_SIZE		0x3800

#define CCIF_AP_CON		0x00
#define CCIF_AP_BUSY		0x04
#define CCIF_AP_TCHNUM		0x0c
#define CCIF_AP_RCHNUM		0x10
#define CCIF_AP_ACK		0x14
#define CCIF_AP_IRQ0		0x18
#define CCIF_AP_IRQ1		0x1c
#define CCIF_AP_IRQ0_MASK	0x20
#define CCIF_AP_IRQ1_MASK	0x24
#define CCIF_CHDATA		0x100
#define CCIF_SRAM_SIZE		512

#define RBF_HEADER		0xEE0000EE
#define RBF_FOOTER		0xFF0000FF
#define RBF_CTL_LEN		40
#define QUEUE_NUM		16
#define H2D_EXCEPTION_ACK	16
#define AP_MD_DATA_NOTIFY	23

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
	struct regmap *infra;
	struct regulator *vmodem;
	struct regulator *vsram;
	void __iomem *ccif_ap;
	void __iomem *ccif_md;
	void __iomem *ccism;
	void __iomem *ccism_exp;
	struct clk_bulk_data *clks;
	int num_clks;
	int irq0;
	int irq1;
	struct dentry *debugfs;
	wait_queue_head_t hs_wq;
	u32 last_rch;
	u32 boot_status0;
	u32 boot_status1;
	bool clks_on;
	bool rails_on;
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

	if (md->rails_on)
		return 0;
	ret = regulator_set_voltage(md->vmodem, 825000, 825000);
	if (ret)
		return ret;
	regulator_sync_voltage(md->vmodem);
	/* Vendor table: VMODEM+NR+FE -> 2 ms -> VSRAM_MD. */
	udelay(2000);
	ret = regulator_set_voltage(md->vsram, 825000, 825000);
	if (ret)
		return ret;
	regulator_sync_voltage(md->vsram);
	ret = regulator_enable(md->vmodem);
	if (ret)
		return ret;
	ret = regulator_enable(md->vsram);
	if (ret)
		return ret;
	md->rails_on = true;
	msleep(10);
	dev_info(md->dev, "rails vmodem=%d vsram=%d uV\n",
		 regulator_get_voltage(md->vmodem),
		 regulator_get_voltage(md->vsram));
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

static void md1_dump_ring(struct mtk_md1 *md, void __iomem *base,
			   const char *tag)
{
	u32 rd = readl(base + 8);
	u32 wr = readl(base + 12);
	u32 len = readl(base + 16);
	int i;

	dev_info(md->dev, "%s : rd=%u wr=%u len=%u\n", tag, rd, wr, len);
	if (rd == wr || !len)
		return;
	for (i = 0; i < 16; i++) {
		u32 v = readl(base + 32 + ((rd + i * 4) % len));

		dev_info(md->dev, "%s : data[%02d]=%08x\n", tag, i, v);
		if (i == 0 && v != 0xffffffff)
			break;
	}
	if (readl(base + 32 + (rd % len)) == 0xffffffff &&
	    readl(base + 32 + ((rd + 4) % len)) == 0 &&
	    readl(base + 32 + ((rd + 12) % len)) == 0x5555ffff)
		dev_info(md->dev, "%s : CCCI HS1 packet detected\n", tag);
}

static void md1_dump(struct mtk_md1 *md, const char *tag)
{
	struct arm_smccc_res res;
	unsigned int v, spm0, spm8, s0, s1, s2, s3;
	unsigned int ap_c, ap_b, ap_t, ap_r, ap_m0, ap_m1;
	unsigned int md_c, md_b, md_t, md_r;

	regmap_read(md->spm, SPM_MD1_PWR_CON, &v);
	regmap_read(md->spm, SPM_MD_EXT_BUCK, &s0);
	regmap_read(md->spm, SPM_PWR_STATUS, &s1);
	regmap_read(md->spm, SPM_PWR_STATUS2, &s2);
	regmap_read(md->spm, SPM_O1_CON, &spm0);
	regmap_read(md->spm, SPM_O1_EN, &spm8);
	s3 = 0;
	if (md->infra)
		regmap_read(md->infra, INFRA_AO_CLK_STATUS0, &s3);

	ap_c = readl(md->ccif_ap + CCIF_AP_CON);
	ap_b = readl(md->ccif_ap + CCIF_AP_BUSY);
	ap_t = readl(md->ccif_ap + CCIF_AP_TCHNUM);
	ap_r = readl(md->ccif_ap + CCIF_AP_RCHNUM);
	ap_m0 = readl(md->ccif_ap + CCIF_AP_IRQ0_MASK);
	ap_m1 = readl(md->ccif_ap + CCIF_AP_IRQ1_MASK);
	md_c = readl(md->ccif_md + CCIF_AP_CON);
	md_b = readl(md->ccif_md + CCIF_AP_BUSY);
	md_t = readl(md->ccif_md + CCIF_AP_TCHNUM);
	md_r = readl(md->ccif_md + CCIF_AP_RCHNUM);

	md1_smc(md, MD_POWER_CONFIG, MD_BOOT_STATUS, 0, &res);
	dev_info(md->dev,
		 "%s: spm 300=%08x 398=%08x 16c=%08x 170=%08x 000=%08x 008=%08x\n",
		 tag, v, s0, s1, s2, spm0, spm8);
	dev_info(md->dev,
		 "%s: clk status0=%08x clks_on=%d\n", tag, s3, md->clks_on);
	dev_info(md->dev,
		 "%s: ap con=%08x busy=%08x tch=%08x rch=%08x m0=%08x m1=%08x\n",
		 tag, ap_c, ap_b, ap_t, ap_r, ap_m0, ap_m1);
	dev_info(md->dev,
		 "%s: md con=%08x busy=%08x tch=%08x rch=%08x irq0=%08x irq1=%08x\n",
		 tag, md_c, md_b, md_t, md_r,
		 readl(md->ccif_ap + CCIF_AP_IRQ0),
		 readl(md->ccif_ap + CCIF_AP_IRQ1));
	dev_info(md->dev,
		 "%s: sram tail f4=%08x f8=%08x fc=%08x\n", tag,
		 readl(md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 12),
		 readl(md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 8),
		 readl(md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 4));
	dev_info(md->dev,
		 "%s: q0 hdr=%08x %08x rx=%u/%u/%u tx=%u/%u/%u\n", tag,
		 readl(md->ccism), readl(md->ccism + 4),
		 readl(md->ccism + 8), readl(md->ccism + 12),
		 readl(md->ccism + 16), readl(md->ccism + 20),
		 readl(md->ccism + 24), readl(md->ccism + 28));
	dev_info(md->dev,
		 "%s: boot_status smc=%08lx/%08lx boot_slave=%ld last_rch=%08x\n",
		 tag, res.a1, res.a2, res.a3, md->last_rch);
	md1_dump_ring(md, md->ccism, "q0");
	md1_dump_ring(md, md->ccism_exp, "exp0");
}

static int md1_ccif_reset(struct mtk_md1 *md)
{
	if (!md->infra)
		return -EOPNOTSUPP;

	regmap_write(md->infra, INFRA_AO_CCIF_RST_SET,
		     INFRA_AO_CCIF_RST_BIT);
	udelay(10);
	regmap_write(md->infra, INFRA_AO_CCIF_RST_CLR,
		     INFRA_AO_CCIF_RST_BIT);
	udelay(10);
	dev_info(md->dev, "CCIF reset pulse done\n");
	return 0;
}

static int md1_ccif_start(struct mtk_md1 *md, bool reset)
{
	void __iomem *p = md->ccism;
	void __iomem *e = md->ccism_exp;
	int i, ret;

	if (md->num_clks && !md->clks_on) {
		ret = clk_bulk_prepare_enable(md->num_clks, md->clks);
		if (ret) {
			dev_err(md->dev, "CCIF clock enable failed: %d\n", ret);
			return ret;
		}
		md->clks_on = true;
	}

	/*
	 * Stock ccif_start(): zero CCISM, create the normal and exception
	 * rings, reset the ring pointers, then reset the CCIF hardware and
	 * publish the MDSS_DBG region in the SRAM tail.
	 */
	memset_io(md->ccism, 0, CCISM_SIZE);
	memset_io(md->ccism_exp, 0, CCISM_EXP_SIZE);
	for (i = 0; i < QUEUE_NUM; i++) {
		md1_ring(p, rx_n[i], tx_n[i]);
		p += RBF_CTL_LEN + rx_n[i] + tx_n[i];
		md1_ring(e, rx_e[i], rx_e[i]);
		e += RBF_CTL_LEN + 2 * rx_e[i];
	}

	if (reset)
		md1_ccif_reset(md);

	memset_io(md->ccif_ap + CCIF_CHDATA, 0, CCIF_SRAM_SIZE);
	memset_io(md->ccif_md + CCIF_CHDATA, 0, CCIF_SRAM_SIZE);
	writel(0x7274626e, md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 12);
	writel(MDSS_DBG_MD, md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 8);
	writel(MDSS_DBG_SIZE, md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 4);
	writel(0x00007fff, md->ccif_ap + CCIF_AP_IRQ1_MASK);
	writel(0x00000000, md->ccif_ap + CCIF_AP_IRQ0_MASK);

	dev_info(md->dev, "CCIF start: tail=%08x %08x %08x mask=%08x\n",
		 readl(md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 12),
		 readl(md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 8),
		 readl(md->ccif_ap + CCIF_CHDATA + CCIF_SRAM_SIZE - 4),
		 readl(md->ccif_ap + CCIF_AP_IRQ1_MASK));
	return 0;
}

static void md1_ccif_stop(struct mtk_md1 *md)
{
	if (!md->clks_on)
		return;
	writel(0xffffffff, md->ccif_ap + CCIF_AP_IRQ0_MASK);
	writel(0xffffffff, md->ccif_ap + CCIF_AP_IRQ1_MASK);
	clk_bulk_disable_unprepare(md->num_clks, md->clks);
	md->clks_on = false;
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
	dev_info(md->dev, "MD1 powered, PWR_ACK ok, publish ret=%ld\n", res.a0);
	return 0;
}

static int md1_power_off(struct mtk_md1 *md)
{
	struct arm_smccc_res res;
	unsigned int v;

	if (md->spm) {
		md1_smc(md, MD_FLIGHT_MODE_SET, 1, 0, &res);
		regmap_read(md->spm, SPM_MD1_PWR_CON, &v);
		regmap_write(md->spm, SPM_MD1_PWR_CON, v & ~BIT(2));
		msleep(20);
		regmap_read(md->spm, SPM_MD_EXT_BUCK, &v);
		regmap_write(md->spm, SPM_MD_EXT_BUCK, v | 3);
		regmap_read(md->spm, SPM_MD1_PWR_CON, &v);
		regmap_write(md->spm, SPM_MD1_PWR_CON, v & ~BIT(0));
		msleep(20);
	}
	md->powered = false;
	dev_info(md->dev, "MD1 domain off\n");
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
	dev_info(md->dev, "MD1 let-go: status0=%08x status1=%08x slave=%ld\n",
		 md->boot_status0, md->boot_status1, res.a3);
	return md->boot_status0 == MD_BOOT_OK ? 0 : -EIO;
}

static int md1_trigger(struct mtk_md1 *md, unsigned int ch)
{
	int i, rchnum = 0;
	u32 busy;

	md->last_rch = 0;
	busy = readl(md->ccif_ap + CCIF_AP_BUSY);
	if (!(busy & BIT(ch))) {
		writel(BIT(ch), md->ccif_ap + CCIF_AP_BUSY);
		writel(ch, md->ccif_ap + CCIF_AP_TCHNUM);
	}
	for (i = 0; i < 2000; i++) {
		rchnum = readl(md->ccif_md + CCIF_AP_RCHNUM);
		if (rchnum)
			break;
		usleep_range(1000, 2000);
	}
	dev_info(md->dev, "CCIF ch%u: md_rchnum=%08x ap_rchnum=%08x after %d ms\n",
		 ch, rchnum, readl(md->ccif_ap + CCIF_AP_RCHNUM), i);
	return rchnum ? 0 : -ETIMEDOUT;
}

static irqreturn_t md1_ccif_irq(int irq, void *data)
{
	struct mtk_md1 *md = data;
	u32 rch = readl(md->ccif_ap + CCIF_AP_RCHNUM);

	md->last_rch = rch;
	if (rch)
		writel(rch & (0xffff << 15), md->ccif_ap + CCIF_AP_ACK);
	dev_info_ratelimited(md->dev, "CCIF irq%d rch=%08x irq0=%08x irq1=%08x\n",
			     irq, rch, readl(md->ccif_ap + CCIF_AP_IRQ0),
			     readl(md->ccif_ap + CCIF_AP_IRQ1));
	wake_up_all(&md->hs_wq);
	return IRQ_HANDLED;
}

static int md1_wait_hs(struct mtk_md1 *md, unsigned int ms)
{
	unsigned int rch = 0;
	int i;

	if (wait_event_timeout(md->hs_wq, md->last_rch != 0,
			       msecs_to_jiffies(ms)))
		return 0;
	for (i = 0; i < ms; i += 10) {
		rch = readl(md->ccif_md + CCIF_AP_RCHNUM);
		if (rch) {
			md->last_rch = rch;
			return 0;
		}
		msleep(10);
	}
	return -ETIMEDOUT;
}

static int md1_boot(struct mtk_md1 *md)
{
	int ret;

	dev_info(md->dev, "=== MD1 boot sequence start ===\n");
	md1_dump(md, "pre");

	/*
	 * Stock order: LK leaves MD powered; the kernel initializes CCIF
	 * while the domain is on, then power-cycles it and releases the
	 * boot vector.  Writes to the CCIF SRAM are only accepted in the
	 * powered state.
	 */
	ret = md1_power_on(md);
	if (ret)
		return ret;
	md1_dump(md, "on1");

	ret = md1_ccif_start(md, true);
	if (ret)
		return ret;
	md1_dump(md, "ccif");

	md1_power_off(md);
	md1_dump(md, "off2");

	ret = md1_power_on(md);
	if (ret)
		return ret;
	md1_dump(md, "on2");

	ret = md1_let_go(md);
	md1_dump(md, "letgo");
	if (ret)
		return ret;

	ret = md1_wait_hs(md, 3000);
	dev_info(md->dev, "HS wait: %s (rch=%08x)\n",
		 ret ? "timeout" : "signalled", md->last_rch);
	md1_dump(md, "hs");
	return ret;
}

static ssize_t boot_write(struct file *f, const char __user *buf, size_t len,
			  loff_t *off)
{
	struct mtk_md1 *md = f->private_data;
	int ret = md1_boot(md);

	return ret ? ret : len;
}

static const struct file_operations boot_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = boot_write,
};

static ssize_t off_write(struct file *f, const char __user *buf, size_t len,
			 loff_t *off)
{
	struct mtk_md1 *md = f->private_data;

	md1_power_off(md);
	return len;
}

static const struct file_operations off_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = off_write,
};

static ssize_t ccif_write(struct file *f, const char __user *buf, size_t len,
			  loff_t *off)
{
	struct mtk_md1 *md = f->private_data;
	int ret = md1_ccif_start(md, true);

	return ret ? ret : len;
}

static const struct file_operations ccif_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ccif_write,
};

static ssize_t letgo_write(struct file *f, const char __user *buf, size_t len,
			   loff_t *off)
{
	struct mtk_md1 *md = f->private_data;
	int ret = md1_let_go(md);

	return ret ? ret : len;
}

static const struct file_operations letgo_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = letgo_write,
};

static ssize_t trigger_write(struct file *f, const char __user *buf,
			     size_t len, loff_t *off)
{
	struct mtk_md1 *md = f->private_data;
	int ret = md1_trigger(md, H2D_EXCEPTION_ACK);

	return ret ? ret : len;
}

static const struct file_operations trigger_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = trigger_write,
};

static ssize_t trigger23_write(struct file *f, const char __user *buf,
			       size_t len, loff_t *off)
{
	struct mtk_md1 *md = f->private_data;
	int ret = md1_trigger(md, AP_MD_DATA_NOTIFY);

	return ret ? ret : len;
}

static const struct file_operations trigger23_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = trigger23_write,
};

static ssize_t dump_write(struct file *f, const char __user *buf, size_t len,
			  loff_t *off)
{
	struct mtk_md1 *md = f->private_data;

	md1_dump(md, "manual");
	return len;
}

static const struct file_operations dump_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = dump_write,
};

static int md1_probe(struct platform_device *pdev)
{
	struct mtk_md1 *md;
	struct resource *res;
	int ret;

	md = devm_kzalloc(&pdev->dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = &pdev->dev;
	init_waitqueue_head(&md->hs_wq);

	md->spm = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						  "mediatek,spm");
	if (IS_ERR(md->spm))
		return dev_err_probe(&pdev->dev, PTR_ERR(md->spm),
				     "no SPM syscon\n");

	md->infra = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						    "mediatek,infracfg");
	if (IS_ERR(md->infra)) {
		dev_info(&pdev->dev, "no INFRA_AO syscon, CCIF reset disabled\n");
		md->infra = NULL;
	}

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
	/*
	 * CCISM lives in no-map reserved memory; do not request the region,
	 * just map it write-combined like the vendor CCCI code does.
	 */
	md->ccism = devm_ioremap_wc(&pdev->dev, res->start,
				    resource_size(res));
	if (IS_ERR(md->ccism))
		return PTR_ERR(md->ccism);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ccism-exp");
	if (!res)
		return -EINVAL;
	md->ccism_exp = devm_ioremap_wc(&pdev->dev, res->start,
					resource_size(res));
	if (IS_ERR(md->ccism_exp))
		return PTR_ERR(md->ccism_exp);

	ret = devm_clk_bulk_get_all(&pdev->dev, &md->clks);
	if (ret < 0)
		dev_info(&pdev->dev, "no CCIF clocks in DT: %d\n", ret);
	else
		md->num_clks = ret;
	dev_info(&pdev->dev, "CCIF clocks: %d\n", md->num_clks);

	md->irq0 = platform_get_irq_byname(pdev, "ccif0");
	if (md->irq0 > 0) {
		ret = devm_request_irq(&pdev->dev, md->irq0, md1_ccif_irq, 0,
				       "mtk-md1-ccif0", md);
		if (ret)
			dev_info(&pdev->dev, "IRQ0 %d request failed: %d\n",
				 md->irq0, ret);
	}
	md->irq1 = platform_get_irq_byname(pdev, "ccif1");
	if (md->irq1 > 0) {
		ret = devm_request_irq(&pdev->dev, md->irq1, md1_ccif_irq, 0,
				       "mtk-md1-ccif1", md);
		if (ret)
			dev_info(&pdev->dev, "IRQ1 %d request failed: %d\n",
				 md->irq1, ret);
	}

	platform_set_drvdata(pdev, md);
	md->debugfs = debugfs_create_dir("mtk_md1", NULL);
	debugfs_create_file("boot", 0200, md->debugfs, md, &boot_fops);
	debugfs_create_file("off", 0200, md->debugfs, md, &off_fops);
	debugfs_create_file("ccif", 0200, md->debugfs, md, &ccif_fops);
	debugfs_create_file("letgo", 0200, md->debugfs, md, &letgo_fops);
	debugfs_create_file("trigger", 0200, md->debugfs, md, &trigger_fops);
	debugfs_create_file("trigger23", 0200, md->debugfs, md, &trigger23_fops);
	debugfs_create_file("dump", 0200, md->debugfs, md, &dump_fops);

	dev_info(&pdev->dev, "MT6789 MD1 ready (power=%d)\n", md->powered);
	return 0;
}

static void md1_remove(struct platform_device *pdev)
{
	struct mtk_md1 *md = platform_get_drvdata(pdev);

	md1_ccif_stop(md);
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
