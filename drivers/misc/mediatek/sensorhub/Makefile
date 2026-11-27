# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)

obj-$(CONFIG_MTK_X676B_SENSORHUB) += hf_manager.o
obj-$(CONFIG_MTK_X676B_SENSORHUB) += x676b_sensorhub.o
x676b_sensorhub-objs := tiny_crc8.o ready.o ipi_comm.o sensor_comm.o \
			sensor_list.o transceiver.o share_memory.o \
			timesync.o debug.o custom_cmd.o
