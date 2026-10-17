# SPDX-License-Identifier: GPL-2.0
# Samsung's modem interface (drivers/misc/modem_v1, mcu_ipc, pmu-cp, pmucal_cp
# of the SM-G965F 4.9 kernel) ported to mainline as one module. The options are
# those of exynos9810-star2lte_defconfig: SS310AP (Shannon) over shared memory.
obj-m += s9p-modem.o
s9p-modem-y := modem_main.o modem_variation.o modem_io_device.o modem_utils.o \
	modem_dump.o link_device_shmem.o link_device_memory_snapshot.o \
	link_device_memory_flow_control.o link_device_memory_debug.o \
	link_device_memory_sbd.o modem_notifier.o modem_ctrl_ss310ap.o \
	mcu_ipc.o shm_ipc.o s9p-cp-pmu.o s9p-modem-core.o

ccflags-y += -I$(src) -I$(src)/include -include $(src)/s9p-compat.h -DCONFIG_CP_RAM_LOGGING=1 \
	-DCONFIG_OPTION_REGION=\"EUR\" \
	-DCONFIG_SOC_EXYNOS9810=1 \
	-DCONFIG_MCU_IPC=1 -DCONFIG_SHM_IPC=1 \
	-DCONFIG_SEC_SIPC_MODEM_IF=1 -DCONFIG_SEC_MODEM_SS310AP=1 \
	-DCONFIG_UMTS_MODEM_SS310AP=1 -DCONFIG_LINK_DEVICE_SHMEM=1 \
	-DCONFIG_LINK_DEVICE_NAPI=1 \
	-DCONFIG_CP_SECURE_BOOT=1 -DCONFIG_CP_PMUCAL=1 \
	-Wno-error -Wno-unused-function -Wno-unused-variable
