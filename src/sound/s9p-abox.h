/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Galaxy S9+ ABOX: IPC message layout of the stock "calliope" firmware
 * (vendor include/sound/samsung/abox_ipc.h; 764 bytes per message).
 */
#ifndef S9P_ABOX_H
#define S9P_ABOX_H

#include <linux/build_bug.h>
#include <linux/types.h>

enum s9p_abox_ipc_id {
	IPC_SYSTEM = 1,
	IPC_PCMPLAYBACK,
	IPC_PCMCAPTURE,
	IPC_OFFLOAD,
	IPC_ERAP,
	WDMA0_BUF_FULL = 0x8,
	WDMA1_BUF_FULL = 0x9,
	IPC_ABOX_CONFIG = 0xa,
	RDMA0_BUF_EMPTY = 0xb,
	RDMA3_BUF_EMPTY = 0xe,
};

enum s9p_abox_pcmmsg {
	PCM_PLTDAI_OPEN = 11,
	PCM_PLTDAI_CLOSE = 12,
	PCM_PLTDAI_HW_PARAMS = 14,
	PCM_PLTDAI_HW_FREE = 15,
	PCM_PLTDAI_PREPARE = 16,
	PCM_PLTDAI_TRIGGER = 17,
	PCM_PLTDAI_POINTER = 18,
	PCM_SET_BUFFER = 20,
	PCM_PLTDAI_ACK = 22,
};

enum s9p_abox_configmsg {
	SET_MIXER_SAMPLE_RATE = 1,
	SET_OUT1_SAMPLE_RATE,
	SET_OUT2_SAMPLE_RATE,
	SET_RECP_SAMPLE_RATE,
	SET_INMUX0_SAMPLE_RATE,
	SET_MIXER_FORMAT = 0x10,
	SET_OUT1_FORMAT,
	SET_OUT2_FORMAT,
	SET_RECP_FORMAT,
	SET_INMUX0_FORMAT,
};

enum s9p_abox_systemmsg {
	ABOX_SUSPEND = 1,
	ABOX_RESUME,
	ABOX_BOOT_DONE,
	ABOX_CHANGE_GEAR,
	ABOX_REQUEST_SYSCLK = 7,
	ABOX_REPORT_LOG = 0x10,
	ABOX_FLUSH_LOG,
	ABOX_START_VSS = 0xa0,	/* the CP is online: start the voice firmware */
};

struct s9p_abox_pcmtask_msg {
	s32 msgtype;
	s32 channel_id;
	union {
		struct {
			s32 sample_rate;
			s32 bit_depth;
			s32 channels;
		} hw_params;
		struct {
			s32 phyaddr;	/* IOVA */
			s32 size;	/* period bytes */
			s32 count;	/* periods */
		} setbuff;
		u32 pointer;
		s32 trigger;
	} param;
};

struct s9p_abox_config_msg {
	s32 msgtype;
	s32 param1;
	s32 param2;
};

struct s9p_abox_system_msg {
	s32 msgtype;
	s32 param1;
	s32 param2;
	s32 param3;
	char bundle[740];
};

struct s9p_abox_ipc_msg {
	s32 ipcid;
	s32 task_id;
	union {
		struct s9p_abox_system_msg system;
		struct s9p_abox_pcmtask_msg pcmtask;
		struct s9p_abox_config_msg config;
	} msg;
};
static_assert(sizeof(struct s9p_abox_ipc_msg) == 764);

#define S9P_ABOX_RDMA_MAX	8
#define S9P_ABOX_IOVA_RDMA(n)	(0x91000000 + 0x100000 * (n))
#define S9P_ABOX_WDMA_MAX	5
#define S9P_ABOX_IOVA_WDMA(n)	(0x92000000 + 0x100000 * (n))

/* callbacks of the PCM layer, called in IRQ context */
struct s9p_abox_ops {
	void (*rdma_period)(void *data, int id, s64 pointer);	/* pointer -1: unknown */
	void (*wdma_period)(void *data, int id, s64 pointer);	/* capture, same */
	void (*ipc)(void *data, const struct s9p_abox_ipc_msg *m);
};

int s9p_abox_ipc(struct s9p_abox_ipc_msg *msg);
int s9p_abox_set_ops(const struct s9p_abox_ops *ops, void *data);
bool s9p_abox_ready(void);
void *s9p_abox_rdma_buffer(unsigned int id, dma_addr_t *pa, u32 *iova);
void *s9p_abox_wdma_buffer(unsigned int id, dma_addr_t *pa, u32 *iova);
int s9p_abox_uaif_bclk(unsigned int uaif, unsigned long hz);
void __iomem *s9p_abox_sfr(void);
struct device *s9p_abox_dev(void);

#endif
