/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6789 tinysys SCP IPI interface.
 *
 * This is a small compatibility surface for the sensorhub stack: the SCP
 * firmware uses the Android mbox/IPI protocol, not the upstream SRAM-share
 * IPI.  The transport lives in drivers/soc/mediatek/mtk-scp-tinysys.c.
 */

#ifndef __LINUX_SOC_MEDIATEK_MTK_SCP_TINYSYS_H
#define __LINUX_SOC_MEDIATEK_MTK_SCP_TINYSYS_H

#include <linux/notifier.h>
#include <linux/types.h>

struct mtk_ipi_device;

/* SCP platform events (vendor scp.h). */
enum {
	SCP_EVENT_READY = 1,
	SCP_EVENT_STOP,
};

#define IPI_PIN_BUSY		(-6)
#define SENS_FEATURE_ID		1

/* SCP IPI channel ids used by the sensorhub firmware. */
enum {
	IPI_OUT_SENSOR_CTRL	= 29,
	IPI_IN_SENSOR_CTRL	= 30,
	IPI_OUT_SENSOR_NOTIFY	= 31,
	IPI_IN_SENSOR_NOTIFY	= 32,
};

/* Reserve-memory block ids (vendor scp.h). */
enum scp_reserve_mem_id {
	SCP_A_SECDUMP_MEM_ID = 0,
	VOW_MEM_ID,
	SENS_MEM_ID,
	SCP_A_LOGGER_MEM_ID,
	AUDIO_IPI_MEM_ID,
	VOW_BARGEIN_MEM_ID,
	SCP_DRV_PARAMS_MEM_ID,
	ULTRA_MEM_ID,
	SENS_SUPER_MEM_ID,
	SENS_LIST_MEM_ID,
	SENS_DEBUG_MEM_ID,
	SENS_CUSTOM_W_MEM_ID,
	SENS_CUSTOM_R_MEM_ID,
	SCP_SPK_MEM_ID,
	NUMS_MEM_ID,
};

typedef int (*mtk_ipi_handler_t)(unsigned int id, void *prdata, void *data,
				 unsigned int len);

extern struct mtk_ipi_device scp_ipidev;

int mtk_ipi_register(struct mtk_ipi_device *ipidev, int id,
		     mtk_ipi_handler_t cb, void *prdata, void *msg);
int mtk_ipi_unregister(struct mtk_ipi_device *ipidev, int id);
int mtk_ipi_send(struct mtk_ipi_device *ipidev, int id, int opt, void *data,
		 int len, int timeout);

phys_addr_t scp_get_reserve_mem_phys(enum scp_reserve_mem_id id);
phys_addr_t scp_get_reserve_mem_virt(enum scp_reserve_mem_id id);
phys_addr_t scp_get_reserve_mem_size(enum scp_reserve_mem_id id);

void scp_A_register_notify(struct notifier_block *nb);
void scp_A_unregister_notify(struct notifier_block *nb);
int scp_wdt_reset(unsigned int core);
int scp_register_sensor(int feature, int sensor);
int scp_deregister_sensor(int feature, int sensor);

#endif /* __LINUX_SOC_MEDIATEK_MTK_SCP_TINYSYS_H */
