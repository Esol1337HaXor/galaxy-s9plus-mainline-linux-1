// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): ABOX audio DSP, firmware boot and IPC.
 *
 * All the audio of the phone (codec, amplifiers, BT) goes through ABOX. Its
 * DMA engines (RDMA/WDMA) are only programmed by Samsung's firmware
 * ("calliope", a Cortex-A7 inside ABOX), so this driver boots it with the
 * stock binaries from the vendor partition and talks to it as the vendor
 * kernel does (sound/soc/samsung/abox/abox.c, abox_gic.c, abox_ipc.h).
 *
 * The firmware uses fixed IOVAs behind the ABOX SysMMU (v7, non-VM), so the
 * SysMMU is programmed here with a private page table of 1 MiB sections:
 *   0x80000000  18 MiB  calliope_dram.bin (+ extra binaries)
 *   0x90000000   1 MiB  calliope_iva.bin
 *   0x91000000  1 MiB/RDMA  playback buffers
 *   0x92000000  1 MiB/WDMA  capture buffers
 *   0x10000000  identity (chip id)
 *
 * Analysis: audio/analisis/01-abox.md.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/wait.h>

#include "s9p-abox.h"

/* blocks */
#define ABOX_SFR		0x17c50000
#define ABOX_SRAM		0x17d00000
#define ABOX_SRAM_SIZE		0x31000
#define ABOX_SYSREG		0x17c10000
#define ABOX_GICD		0x17cf1000
#define ABOX_GICC		0x17cf2000
#define ABOX_SYSMMU		0x17e00000
#define PMU_BASE		0x14060000
#define GPIO_AUD		0x17c60000	/* pinctrl of the AUD block: gpb0/1/2 */
#define  GPB_CON(n)		(0x20 * (n))
#define  GPB_PUD(n)		(0x20 * (n) + 0x8)
#define CMU_AUD			0x17c00000
#define  QCH_GPIO_AUD		0x304c
#define  PLL_LOCKTIME		0x0000
#define  PLL_CON0		0x0120
#define   PLL_ENABLE		BIT(31)
#define   PLL_STABLE		BIT(29)
#define   PLL_MPS_MASK		(GENMASK(25, 16) | GENMASK(13, 8) | GENMASK(2, 0))
#define  PLL_CON3		0x018c
#define  DIV_AUDIF		0x1800
#define  DIV_UAIF(n)		(0x1824 + 4 * (n))
#define  MUX_UAIF(n)		(0x1004 + 4 * (n))
#define ABOX_IRQ		76		/* AP GIC SPI: ABOX GIC CPU interface */

/* SFR */
#define ABOX_VERSION		0x0004
#define ABOX_SYSPOWER_CTRL	0x0010
#define ABOX_REMAP_MASK		0x0024
#define ABOX_REMAP_ADDR		0x0028

/* SYSREG_AUD */
#define SYSREG_MISC_CON		0x032c

/* PMU */
#define PMU_GPIO_MODE_ABOX	0x1424
#define PMU_ABOX_STANDBY	0x3804
#define  ABOX_STANDBY_WFI	BIT(28)
#define PMU_ABOX_CPU_CONF	0x415c
#define PMU_ABOX_CPU_STATUS	0x4160
#define PMU_ABOX_CPU_OPTION	0x4164
#define  ABOX_CPU_ENABLE	BIT(28)
#define PMU_PAD_RET_ABOX	0x4170

/* ABOX GIC (GICv2, secure-free on 9810) */
#define GICD_CTLR		0x000
#define GICD_IPRIORITYR		0x400
#define GICD_SGIR		0xf00
#define GICC_CTLR		0x000
#define GICC_PMR		0x004
#define GICC_IAR		0x00c
#define GICC_EOIR		0x010
#define GICC_DIR		0x1000

/* IPC areas in SRAM (vendor DT ipc_*_offset) */
#define IPC_TX			0x30000
#define IPC_TX_ACK		0x302fc
#define IPC_RX			0x30300
#define IPC_RX_ACK		0x305fc
#define IPC_COMPAT		0x30040

/* SysMMU v7 (non-VM layout, as exynos-iommu sysmmu_v7_variant) */
#define MMU_CTRL		0x000
#define  MMU_CTRL_ENABLE	0x5
#define  MMU_CTRL_BLOCK		0x7
#define MMU_CFG			0x004
#define  MMU_CFG_VAL		((15 << 7) | BIT(20) | BIT(24) | BIT(2))
#define MMU_PT_BASE		0x00c
#define MMU_FLUSH_ALL		0x010
#define MMU_INT_STATUS		0x060
#define MMU_INT_CLEAR		0x064
#define MMU_FAULT_VA		0x070
#define MMU_FAULT_INFO		0x078
#define SECT(pa)		((u32)((pa) >> 4) | (3 << 4) | 2)	/* section, prot [5:4] = 3: RW */

#define IOVA_DRAM_FW		0x80000000
#define DRAM_FW_SIZE		(SZ_16M + SZ_2M)
#define IOVA_IVA_FW		0x90000000
#define IVA_FW_SIZE		SZ_512K
#define IOVA_VSS_FW		0xa0000000	/* voice (CP shared memory on stock) */
#define VSS_FW_SIZE		0x700000
#define IOVA_VSS_PARAM		0xa1000000

static bool boot = true;
module_param(boot, bool, 0444);
MODULE_PARM_DESC(boot, "boot the ABOX firmware at load time");

static bool pll;
module_param(pll, bool, 0444);
MODULE_PARM_DESC(pll, "reprogram PLL_AUD to 1179.648 MHz (sboot already sets it)");

static bool poll = true;
module_param(poll, bool, 0444);
MODULE_PARM_DESC(poll, "poll the ABOX GIC from a work item instead of using its IRQ");

static const struct {
	const char *name;
	u32 offset;		/* in the DRAM firmware area (vendor DT ext_bin, area 1) */
} extra_bins[] = {
	{ "dsm.bin",			0x502000 },
	{ "AP_AUDIO_SLSI.bin",		0x7e0000 },
	{ "APBargeIn_AUDIO_SLSI.bin",	0x7dc000 },
	{ "SoundBoosterParam.bin",	0x4fc000 },
	{ "APDV_AUDIO_SLSI.bin",	0x4d0000 },
	{ "APBiBF_AUDIO_SLSI.bin",	0x7df000 },
	{ "dsm_tune.bin",		0x601000 },
};

struct s9p_abox {
	struct device *dev;
	void __iomem *sfr, *sram, *sysreg, *gicd, *gicc, *mmu, *pmu, *cmu, *gpio;
	int irq;

	u32 *lv1;			/* SysMMU level 1 table, 16 KiB */
	dma_addr_t lv1_pa;
	void *dram;			/* firmware DRAM, 18 MiB */
	dma_addr_t dram_pa;
	void *iva;
	dma_addr_t iva_pa;
	void *vss, *vparam;		/* no modem here: zeroed stand-ins */
	dma_addr_t vss_pa, vparam_pa;
	void *rdma[S9P_ABOX_RDMA_MAX];	/* 1 MiB each, on demand */
	dma_addr_t rdma_pa[S9P_ABOX_RDMA_MAX];

	spinlock_t tx_lock;		/* the single TX mailbox */
	wait_queue_head_t wq;
	bool booted;
	u32 version;
	struct s9p_abox_ops ops;	/* PCM layer callbacks */
	void *ops_data;

	struct delayed_work poll_work;
	unsigned int irq_hist[32];	/* ids seen, for storms */
	bool irq_off;

	struct dentry *dbg;
	spinlock_t log_lock;
	char log[8192];
	size_t log_len;
};

static struct s9p_abox *the_abox;

static __printf(2, 3) void alog(struct s9p_abox *a, const char *fmt, ...)
{
	unsigned long flags;
	char line[192];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vscnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	dev_info(a->dev, "%s\n", line);
	spin_lock_irqsave(&a->log_lock, flags);
	if (a->log_len + n + 2 >= sizeof(a->log)) {
		size_t keep = a->log_len / 2;

		memmove(a->log, a->log + a->log_len - keep, keep);
		a->log_len = keep;
	}
	memcpy(a->log + a->log_len, line, n);
	a->log_len += n;
	a->log[a->log_len++] = '\n';
	spin_unlock_irqrestore(&a->log_lock, flags);
}

/* ------------------------------------------------------------------ */
/* SysMMU                                                               */

static void mmu_map(struct s9p_abox *a, u32 iova, phys_addr_t pa, size_t size)
{
	u32 i;

	for (i = 0; i < DIV_ROUND_UP(size, SZ_1M); i++)
		a->lv1[(iova >> 20) + i] = SECT(pa + (phys_addr_t)i * SZ_1M);
	wmb();
	if (readl(a->mmu + MMU_CTRL) == MMU_CTRL_ENABLE)
		writel(1, a->mmu + MMU_FLUSH_ALL);
}

static void mmu_enable(struct s9p_abox *a)
{
	writel(MMU_CTRL_BLOCK, a->mmu + MMU_CTRL);
	writel(0xffffffff, a->mmu + MMU_INT_CLEAR);	/* faults of an earlier run */
	writel(MMU_CFG_VAL, a->mmu + MMU_CFG);
	writel(a->lv1_pa >> 12, a->mmu + MMU_PT_BASE);
	writel(1, a->mmu + MMU_FLUSH_ALL);
	writel(MMU_CTRL_ENABLE, a->mmu + MMU_CTRL);
	alog(a, "SysMMU on: pt %pad, ctrl %08x cfg %08x", &a->lv1_pa,
	     readl(a->mmu + MMU_CTRL), readl(a->mmu + MMU_CFG));
}

/* 1 MiB, 1 MiB-aligned (CMA alignment) coherent buffer */
static void *alloc_mb(struct s9p_abox *a, size_t size, dma_addr_t *pa)
{
	void *p = dma_alloc_coherent(a->dev, ALIGN(size, SZ_1M), pa, GFP_KERNEL);

	if (p && (*pa & (SZ_1M - 1))) {
		dev_err(a->dev, "buffer %pad not 1 MiB aligned\n", pa);
		dma_free_coherent(a->dev, ALIGN(size, SZ_1M), p, *pa);
		return NULL;
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* IPC                                                                  */

static void gic_init(struct s9p_abox *a)
{
	int i;

	writel(0xff, a->gicc + GICC_PMR);
	writel(0x3, a->gicd + GICD_CTLR);
	for (i = 0; i < 40; i++)
		writel(0x10101010, a->gicd + GICD_IPRIORITYR + 4 * i);
	writel(0x3, a->gicc + GICC_CTLR);
}

int s9p_abox_ipc(struct s9p_abox_ipc_msg *msg)
{
	struct s9p_abox *a = the_abox;
	unsigned long flags;
	int i, ret = 0;

	if (!a)
		return -ENODEV;
	spin_lock_irqsave(&a->tx_lock, flags);
	memcpy_toio(a->sram + IPC_TX, msg, sizeof(*msg));
	writel(1, a->sram + IPC_TX_ACK);
	writel(BIT(16) | (msg->ipcid & 0xf), a->gicd + GICD_SGIR);
	for (i = 10000; i && readl(a->sram + IPC_TX_ACK); i--)
		udelay(1);
	if (readl(a->sram + IPC_TX_ACK)) {
		writel(0, a->sram + IPC_TX_ACK);
		ret = -ETIMEDOUT;
	}
	spin_unlock_irqrestore(&a->tx_lock, flags);
	if (ret)
		alog(a, "IPC %u/%d: timeout", msg->ipcid, msg->msg.system.msgtype);
	return ret;
}
EXPORT_SYMBOL_GPL(s9p_abox_ipc);

static void handle_system(struct s9p_abox *a, struct s9p_abox_ipc_msg *m)
{
	struct s9p_abox_system_msg *s = &m->msg.system;

	switch (s->msgtype) {
	case ABOX_BOOT_DONE: {
		u32 v = s->param3;

		a->version = v;
		a->booted = true;
		alog(a, "BOOT_DONE: Calliope version %c%c%c%c%s",
		     (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff,
		     (readl(a->sram + IPC_COMPAT + 20) >> 24) == 'A' ?
		     " (INCOMPATIBLE with this protocol)" : "");
		wake_up(&a->wq);
		break;
	}
	case ABOX_CHANGE_GEAR:
	case ABOX_REQUEST_SYSCLK:
		/* frequency requests: the CMU stays as sboot left it */
		break;
	case ABOX_REPORT_LOG:
	case ABOX_FLUSH_LOG:
		break;
	default:
		alog(a, "system msg %d (%d %d %d)", s->msgtype, s->param1, s->param2, s->param3);
	}
}

/* drain the ABOX GIC CPU interface of the AP; at most @budget interrupts */
static int abox_drain(struct s9p_abox *a, int budget)
{
	struct s9p_abox_ipc_msg m;
	int n = 0;
	u32 iar, id;

	while (n < budget) {
		iar = readl(a->gicc + GICC_IAR);
		id = iar & 0x3ff;
		if (id >= 1021)
			break;
		writel(iar, a->gicc + GICC_EOIR);
		if (id < 16)
			writel(iar, a->gicc + GICC_DIR);
		n++;
		a->irq_hist[min_t(u32, id, 31)]++;

		if (id >= RDMA0_BUF_EMPTY && id <= RDMA3_BUF_EMPTY) {
			if (a->ops.rdma_period)
				a->ops.rdma_period(a->ops_data, id - RDMA0_BUF_EMPTY, -1);
			continue;
		}
		memcpy_fromio(&m, a->sram + IPC_RX, sizeof(m));
		writel(0, a->sram + IPC_RX_ACK);
		switch (m.ipcid) {
		case IPC_SYSTEM:
			handle_system(a, &m);
			break;
		case IPC_PCMPLAYBACK:
			if (m.msg.pcmtask.msgtype == PCM_PLTDAI_POINTER && a->ops.rdma_period)
				a->ops.rdma_period(a->ops_data, m.msg.pcmtask.channel_id,
						   m.msg.pcmtask.param.pointer);
			else if (a->ops.ipc)
				a->ops.ipc(a->ops_data, &m);
			break;
		default:
			if (a->ops.ipc)
				a->ops.ipc(a->ops_data, &m);
			else
				alog(a, "IPC from ABOX: id %u (gic irq %u)", m.ipcid, id);
		}
	}
	return n;
}

static irqreturn_t abox_irq(int irq, void *data)
{
	struct s9p_abox *a = data;
	int n = abox_drain(a, 64);

	if (n >= 64 && !a->irq_off) {
		a->irq_off = true;
		disable_irq_nosync(irq);
		alog(a, "interrupt storm from the ABOX GIC: IRQ disabled (see state)");
	}
	return n ? IRQ_HANDLED : IRQ_NONE;
}

static void poll_work(struct work_struct *w)
{
	struct s9p_abox *a = container_of(w, struct s9p_abox, poll_work.work);

	if (abox_drain(a, 32) >= 32)
		alog(a, "many interrupts in one poll (storm?)");
	schedule_delayed_work(&a->poll_work, msecs_to_jiffies(20));
}

int s9p_abox_set_ops(const struct s9p_abox_ops *ops, void *data)
{
	if (!the_abox)
		return -ENODEV;
	the_abox->ops_data = data;
	if (ops)
		the_abox->ops = *ops;
	else
		memset(&the_abox->ops, 0, sizeof(the_abox->ops));
	return 0;
}
EXPORT_SYMBOL_GPL(s9p_abox_set_ops);

bool s9p_abox_ready(void)
{
	return the_abox && the_abox->booted;
}
EXPORT_SYMBOL_GPL(s9p_abox_ready);

/* RDMA n buffer: 1 MiB at IOVA 0x91000000 + n MiB (as the vendor) */
void *s9p_abox_rdma_buffer(unsigned int id, dma_addr_t *pa, u32 *iova)
{
	struct s9p_abox *a = the_abox;

	if (!a || id >= S9P_ABOX_RDMA_MAX)
		return NULL;
	if (!a->rdma[id]) {
		a->rdma[id] = alloc_mb(a, SZ_1M, &a->rdma_pa[id]);
		if (!a->rdma[id])
			return NULL;
		mmu_map(a, S9P_ABOX_IOVA_RDMA(id), a->rdma_pa[id], SZ_1M);
	}
	*pa = a->rdma_pa[id];
	*iova = S9P_ABOX_IOVA_RDMA(id);
	return a->rdma[id];
}
EXPORT_SYMBOL_GPL(s9p_abox_rdma_buffer);

/*
 * UAIF bit clock: AUDIF = PLL_AUD (1179.648 MHz) / 16 = 73.728 MHz, as the
 * vendor abox_register_bclk_usage() for the 48 kHz family; then the UAIF
 * divider. Allowed with the firmware running (the vendor does it at
 * hw_params).
 */
int s9p_abox_uaif_bclk(unsigned int uaif, unsigned long hz)
{
	struct s9p_abox *a = the_abox;
	u32 div;

	if (!a || uaif > 3 || !hz)
		return -EINVAL;
	div = 73728000 / hz;
	if (!div || div > 512 || 73728000 % hz)
		return -EINVAL;
	writel((readl(a->cmu + DIV_AUDIF) & ~0x1ff) | 15, a->cmu + DIV_AUDIF);
	writel((readl(a->cmu + DIV_UAIF(uaif)) & ~0x1ff) | (div - 1), a->cmu + DIV_UAIF(uaif));
	writel(readl(a->cmu + MUX_UAIF(uaif)) & ~1, a->cmu + MUX_UAIF(uaif));
	alog(a, "UAIF%u bit clock %lu Hz (AUDIF div %08x, UAIF div %08x)", uaif, hz,
	     readl(a->cmu + DIV_AUDIF), readl(a->cmu + DIV_UAIF(uaif)));
	return 0;
}
EXPORT_SYMBOL_GPL(s9p_abox_uaif_bclk);

void __iomem *s9p_abox_sfr(void)
{
	return the_abox ? the_abox->sfr : NULL;
}
EXPORT_SYMBOL_GPL(s9p_abox_sfr);

struct device *s9p_abox_dev(void)
{
	return the_abox ? the_abox->dev : NULL;
}
EXPORT_SYMBOL_GPL(s9p_abox_dev);

/* ------------------------------------------------------------------ */
/* firmware boot                                                        */

static int load_fw(struct s9p_abox *a, const char *name, void *dst, void __iomem *io,
		   size_t max, bool required)
{
	const struct firmware *fw;
	int ret = request_firmware(&fw, name, a->dev);

	if (ret) {
		if (required)
			alog(a, "firmware %s: %d (put it in /lib/firmware)", name, ret);
		return required ? ret : 0;
	}
	if (fw->size > max) {
		alog(a, "firmware %s too big (%zu > %zu)", name, fw->size, max);
		release_firmware(fw);
		return -EFBIG;
	}
	if (io) {
		memcpy_toio(io, fw->data, fw->size);
		memset_io(io + fw->size, 0, max - fw->size);
	} else {
		memcpy(dst, fw->data, fw->size);
	}
	alog(a, "firmware %s: %zu bytes", name, fw->size);
	release_firmware(fw);
	return 0;
}


/*
 * CMU_AUD: PLL_AUD (pll1031x, 26 MHz in) to the stock 48 kHz family rate,
 * 1179.648 MHz = 26 MHz * (45 + 24319/65536) / 1; AUDIF = PLL/16 = 73.728 MHz,
 * UAIF0/1 bit clock = AUDIF/48 = 1.536 MHz (48 kHz, 16 bit, stereo). Only
 * done before the ABOX CPU runs: the AUD bus clock comes from this PLL too.
 */
static void cmu_setup(struct s9p_abox *a)
{
	u32 con0 = readl(a->cmu + PLL_CON0), v;
	unsigned long t;

	if (!pll) {
		alog(a, "CMU_AUD left as is: PLL con0 %08x con3 %08x, AUDIF div %08x, UAIF0 div %08x",
		     con0, readl(a->cmu + PLL_CON3), readl(a->cmu + DIV_AUDIF),
		     readl(a->cmu + DIV_UAIF(0)));
		return;
	}
	{
		writel(con0 & ~PLL_ENABLE, a->cmu + PLL_CON0);
		con0 = (con0 & ~(PLL_MPS_MASK | PLL_ENABLE)) | 45 << 16 | 1 << 8 | 0;
		writel(con0, a->cmu + PLL_CON0);
		writel((readl(a->cmu + PLL_CON3) & ~0xffff) | 24319, a->cmu + PLL_CON3);
		writel(con0 | PLL_ENABLE, a->cmu + PLL_CON0);
		t = jiffies + msecs_to_jiffies(10);
		while (!(readl(a->cmu + PLL_CON0) & PLL_STABLE) && time_before(jiffies, t))
			udelay(10);
	}
	writel((readl(a->cmu + DIV_AUDIF) & ~0x1ff) | 15, a->cmu + DIV_AUDIF);
	for (v = 0; v < 2; v++) {
		writel((readl(a->cmu + DIV_UAIF(v)) & ~0x1ff) | 47, a->cmu + DIV_UAIF(v));
		writel(readl(a->cmu + MUX_UAIF(v)) & ~1, a->cmu + MUX_UAIF(v));
	}
	alog(a, "CMU_AUD: PLL con0 %08x con3 %08x (%s), AUDIF div %08x, UAIF0 div %08x",
	     readl(a->cmu + PLL_CON0), readl(a->cmu + PLL_CON3),
	     readl(a->cmu + PLL_CON0) & PLL_STABLE ? "locked" : "NOT LOCKED",
	     readl(a->cmu + DIV_AUDIF), readl(a->cmu + DIV_UAIF(0)));
}


/*
 * I2S pins of the UAIFs (vendor pinctrl "aud-i2s0-bus" / "aud-i2s1-bus"):
 * function 2, pull-down; gpb0-1..4 = UAIF0 (codec AIF1), gpb1-0..3 = UAIF1
 * (amplifiers). The AUD pinctrl is not in the port's DT, its clock is
 * forced on through its Q-channel first.
 */
static void i2s_pins(struct s9p_abox *a)
{
	writel(0x003f0002, a->cmu + QCH_GPIO_AUD);	/* CLOCK_REQ, no auto gating */
	writel((readl(a->gpio + GPB_CON(0)) & ~0x000ffff0) | 0x00022220, a->gpio + GPB_CON(0));
	writel((readl(a->gpio + GPB_PUD(0)) & ~0x000ffff0) | 0x00011110, a->gpio + GPB_PUD(0));
	writel((readl(a->gpio + GPB_CON(1)) & ~0x0000ffff) | 0x00002222, a->gpio + GPB_CON(1));
	writel((readl(a->gpio + GPB_PUD(1)) & ~0x0000ffff) | 0x00001111, a->gpio + GPB_PUD(1));
	alog(a, "I2S pins: gpb0 con %08x, gpb1 con %08x", readl(a->gpio + GPB_CON(0)),
	     readl(a->gpio + GPB_CON(1)));
}

static int abox_boot(struct s9p_abox *a)
{
	struct s9p_abox_ipc_msg m = {};
	unsigned long t;
	int ret, i;
	u32 v;

	alog(a, "ABOX %08x version %08x, CPU status %08x option %08x",
	     readl(a->sfr), readl(a->sfr + ABOX_VERSION),
	     readl(a->pmu + PMU_ABOX_CPU_STATUS), readl(a->pmu + PMU_ABOX_CPU_OPTION));
	if (readl(a->pmu + PMU_ABOX_CPU_STATUS) & 1) {
		alog(a, "the ABOX CPU is already running: not touching it");
		return -EBUSY;
	}

	cmu_setup(a);
	writel(1, a->sysreg + SYSREG_MISC_CON);
	writel(0xfff00000, a->sfr + ABOX_REMAP_MASK);
	writel(ABOX_SRAM, a->sfr + ABOX_REMAP_ADDR);
	gic_init(a);

	ret = load_fw(a, "calliope_sram.bin", NULL, a->sram, ABOX_SRAM_SIZE, true) ?:
	      load_fw(a, "calliope_dram.bin", a->dram, NULL, DRAM_FW_SIZE, true) ?:
	      load_fw(a, "calliope_iva.bin", a->iva, NULL, IVA_FW_SIZE, false);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(extra_bins); i++)
		load_fw(a, extra_bins[i].name, a->dram + extra_bins[i].offset, NULL,
			DRAM_FW_SIZE - extra_bins[i].offset, false);
	dma_wmb();

	/* DRAM stays on in cpuidle, CPU power, CPU enable */
	writel(1, a->sfr + ABOX_SYSPOWER_CTRL);
	writel(readl(a->pmu + PMU_ABOX_CPU_CONF) | 1, a->pmu + PMU_ABOX_CPU_CONF);
	writel(readl(a->pmu + PMU_ABOX_CPU_OPTION) | ABOX_CPU_ENABLE, a->pmu + PMU_ABOX_CPU_OPTION);
	t = jiffies + HZ;
	do {
		v = readl(a->pmu + PMU_ABOX_CPU_STATUS);
	} while (!(v & 1) && time_before(jiffies, t));
	alog(a, "ABOX CPU %s (status %08x)", v & 1 ? "started" : "DID NOT START", v);
	if (!(v & 1))
		return -ETIMEDOUT;

	for (i = 0; i < 20 && !a->booted; i++) {
		msleep(100);	/* progress, readable even if the system dies */
		alog(a, "t+%d ms: mmu int %08x va %08x, gic hppir %08x, rx ack %u, cpu %08x",
		     (i + 1) * 100, readl(a->mmu + MMU_INT_STATUS), readl(a->mmu + MMU_FAULT_VA),
		     readl(a->gicc + 0x18), readl(a->sram + IPC_RX_ACK),
		     readl(a->pmu + PMU_ABOX_CPU_STATUS));
	}
	if (!a->booted) {
		v = readl(a->mmu + MMU_INT_STATUS);
		alog(a, "no BOOT_DONE in 2 s (SysMMU int %08x fault va %08x info %08x)",
		     v, readl(a->mmu + MMU_FAULT_VA), readl(a->mmu + MMU_FAULT_INFO));
		return -ETIMEDOUT;
	}
	/* I2S pads out of retention (the vendor does it right after starting the CPU) */
	writel(readl(a->pmu + PMU_PAD_RET_ABOX) | BIT(28), a->pmu + PMU_PAD_RET_ABOX);
	writel(readl(a->pmu + PMU_GPIO_MODE_ABOX) | BIT(0), a->pmu + PMU_GPIO_MODE_ABOX);
	i2s_pins(a);
	m.ipcid = IPC_SYSTEM;
	m.msg.system.msgtype = ABOX_RESUME;
	ret = s9p_abox_ipc(&m);
	alog(a, "ABOX_RESUME: %d", ret);
	return ret;
}

static void abox_halt(struct s9p_abox *a)
{
	struct s9p_abox_ipc_msg m = {};
	int i;

	if (a->booted) {
		m.ipcid = IPC_SYSTEM;
		m.msg.system.msgtype = ABOX_SUSPEND;
		s9p_abox_ipc(&m);
		for (i = 1000; i && !(readl(a->pmu + PMU_ABOX_STANDBY) & ABOX_STANDBY_WFI); i--)
			udelay(10);
		alog(a, "ABOX_SUSPEND: %s", i ? "in WFI" : "no WFI");
	}
	writel(readl(a->pmu + PMU_ABOX_CPU_OPTION) & ~ABOX_CPU_ENABLE, a->pmu + PMU_ABOX_CPU_OPTION);
	writel(readl(a->pmu + PMU_ABOX_CPU_CONF) & ~1, a->pmu + PMU_ABOX_CPU_CONF);
	for (i = 1000; i && (readl(a->pmu + PMU_ABOX_CPU_STATUS) & 1); i--)
		udelay(10);
	alog(a, "ABOX CPU %s", i ? "off" : "STILL ON");
	writel(0, a->sfr + ABOX_SYSPOWER_CTRL);
	a->booted = false;
}

/* ------------------------------------------------------------------ */
/* debugfs                                                              */

static int log_show(struct seq_file *s, void *u)
{
	struct s9p_abox *a = s->private;
	unsigned long flags;

	spin_lock_irqsave(&a->log_lock, flags);
	seq_write(s, a->log, a->log_len);
	spin_unlock_irqrestore(&a->log_lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(log);

static int state_show(struct seq_file *s, void *u)
{
	struct s9p_abox *a = s->private;
	int i;

	seq_printf(s, "booted=%d version=%08x irq_off=%d\n", a->booted, a->version, a->irq_off);
	seq_puts(s, "gic ids seen:");
	for (i = 0; i < 32; i++)
		if (a->irq_hist[i])
			seq_printf(s, " %d:%u", i, a->irq_hist[i]);
	seq_putc(s, '\n');
	seq_printf(s, "cpu status %08x option %08x standby %08x\n",
		   readl(a->pmu + PMU_ABOX_CPU_STATUS), readl(a->pmu + PMU_ABOX_CPU_OPTION),
		   readl(a->pmu + PMU_ABOX_STANDBY));
	seq_printf(s, "sysmmu ctrl %08x int %08x fault va %08x info %08x\n",
		   readl(a->mmu + MMU_CTRL), readl(a->mmu + MMU_INT_STATUS),
		   readl(a->mmu + MMU_FAULT_VA), readl(a->mmu + MMU_FAULT_INFO));
	seq_printf(s, "lv1[0x800] %08x [0x80b] %08x [0x900] %08x (pt %pad, PT_BASE %08x)\n",
		   a->lv1[0x800], a->lv1[0x80b], a->lv1[0x900], &a->lv1_pa,
		   readl(a->mmu + MMU_PT_BASE));
	seq_printf(s, "ipc tx ack %u rx ack %u\n", readl(a->sram + IPC_TX_ACK),
		   readl(a->sram + IPC_RX_ACK));
	for (i = 0; i < 0x60; i += 16)
		seq_printf(s, "sfr %03x: %08x %08x %08x %08x\n", i, readl(a->sfr + i),
			   readl(a->sfr + i + 4), readl(a->sfr + i + 8), readl(a->sfr + i + 12));
	for (i = 0x200; i < 0x210; i += 16)
		seq_printf(s, "sfr %03x: %08x %08x %08x %08x\n", i, readl(a->sfr + i),
			   readl(a->sfr + i + 4), readl(a->sfr + i + 8), readl(a->sfr + i + 12));
	for (i = 0x500; i < 0x530; i += 16)
		seq_printf(s, "uaif %03x: %08x %08x %08x %08x\n", i, readl(a->sfr + i),
			   readl(a->sfr + i + 4), readl(a->sfr + i + 8), readl(a->sfr + i + 12));
	for (i = 0; i < 2; i++)
		seq_printf(s, "rdma%d: %08x %08x %08x %08x %08x %08x status %08x\n", i,
			   readl(a->sfr + 0x1000 + 0x100 * i), readl(a->sfr + 0x1004 + 0x100 * i),
			   readl(a->sfr + 0x1008 + 0x100 * i), readl(a->sfr + 0x100c + 0x100 * i),
			   readl(a->sfr + 0x1010 + 0x100 * i), readl(a->sfr + 0x1014 + 0x100 * i),
			   readl(a->sfr + 0x1030 + 0x100 * i));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(state);

/* ------------------------------------------------------------------ */

static int map_abox_irq(void)
{
	struct device_node *gic = of_find_node_by_path("/soc@0/interrupt-controller@10101000");
	struct irq_fwspec spec = {
		.param_count = 3,
		.param = { 0, ABOX_IRQ, IRQ_TYPE_LEVEL_HIGH },
	};
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq ?: -EINVAL;
}

static int s9p_abox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s9p_abox *a;
	int ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	spin_lock_init(&a->tx_lock);
	spin_lock_init(&a->log_lock);
	init_waitqueue_head(&a->wq);
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	a->sfr = devm_ioremap(dev, ABOX_SFR, 0x10000);
	a->sram = devm_ioremap(dev, ABOX_SRAM, ABOX_SRAM_SIZE);
	a->sysreg = devm_ioremap(dev, ABOX_SYSREG, 0x1000);
	a->gicd = devm_ioremap(dev, ABOX_GICD, 0x1000);
	a->gicc = devm_ioremap(dev, ABOX_GICC, 0x2000);
	a->mmu = devm_ioremap(dev, ABOX_SYSMMU, 0x9000);
	a->pmu = devm_ioremap(dev, PMU_BASE, 0x5000);
	a->cmu = devm_ioremap(dev, CMU_AUD, 0x4000);
	a->gpio = devm_ioremap(dev, GPIO_AUD, 0x100);
	if (!a->sfr || !a->sram || !a->sysreg || !a->gicd || !a->gicc || !a->mmu || !a->pmu ||
	    !a->cmu || !a->gpio)
		return -ENOMEM;
	if (readl(a->sfr) != 0x41424f58)
		return dev_err_probe(dev, -ENODEV, "no ABOX at %#x (%08x)\n", ABOX_SFR, readl(a->sfr));

	/* page table and firmware memory; kept for the module's life */
	a->lv1 = dma_alloc_coherent(dev, SZ_16K, &a->lv1_pa, GFP_KERNEL);
	a->dram = alloc_mb(a, DRAM_FW_SIZE, &a->dram_pa);
	a->iva = alloc_mb(a, SZ_1M, &a->iva_pa);
	a->vss = alloc_mb(a, VSS_FW_SIZE, &a->vss_pa);
	a->vparam = alloc_mb(a, SZ_1M, &a->vparam_pa);
	if (!a->lv1 || !a->dram || !a->iva || !a->vss || !a->vparam || (a->lv1_pa & (SZ_16K - 1)))
		return dev_err_probe(dev, -ENOMEM, "no memory for the ABOX firmware\n");
	memset(a->lv1, 0, SZ_16K);
	mmu_map(a, IOVA_DRAM_FW, a->dram_pa, DRAM_FW_SIZE);
	mmu_map(a, IOVA_IVA_FW, a->iva_pa, SZ_1M);
	mmu_map(a, IOVA_VSS_FW, a->vss_pa, VSS_FW_SIZE);
	mmu_map(a, IOVA_VSS_PARAM, a->vparam_pa, SZ_1M);
	mmu_map(a, 0x10000000, 0x10000000, SZ_1M);	/* chip id, identity */
	alog(a, "dram fw %pad, iva %pad", &a->dram_pa, &a->iva_pa);

	INIT_DELAYED_WORK(&a->poll_work, poll_work);
	if (!poll) {
		a->irq = map_abox_irq();
		if (a->irq < 0)
			return a->irq;
		ret = devm_request_irq(dev, a->irq, abox_irq, 0, "s9p-abox", a);
		if (ret)
			return ret;
	}

	a->dbg = debugfs_create_dir("s9p-abox", NULL);
	debugfs_create_file("log", 0444, a->dbg, a, &log_fops);
	debugfs_create_file("state", 0444, a->dbg, a, &state_fops);
	platform_set_drvdata(pdev, a);
	the_abox = a;

	mmu_enable(a);
	if (poll)
		schedule_delayed_work(&a->poll_work, 0);
	if (boot) {
		ret = abox_boot(a);
		if (ret)
			alog(a, "boot failed: %d", ret);
	}
	return 0;
}

static void s9p_abox_remove(struct platform_device *pdev)
{
	struct s9p_abox *a = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&a->poll_work);
	abox_halt(a);
	the_abox = NULL;
	debugfs_remove_recursive(a->dbg);
	/*
	 * Without control of pd-aud the CA7 cannot be powered off for sure:
	 * block the SysMMU so nothing reaches memory, and leave the buffers
	 * allocated (a stray DMA into freed pages would corrupt the kernel).
	 */
	writel(MMU_CTRL_BLOCK, a->mmu + MMU_CTRL);
	dev_warn(a->dev, "after a firmware boot, ABOX only boots again after a reboot\n");
	dev_warn(a->dev, "ABOX memory (%u KiB) left allocated on purpose\n",
		 (DRAM_FW_SIZE + 3 * SZ_1M + VSS_FW_SIZE + SZ_16K) / 1024);
}

static struct platform_driver s9p_abox_driver = {
	.probe = s9p_abox_probe,
	.remove = s9p_abox_remove,
	.driver = { .name = "s9p-abox" },
};

static struct platform_device *s9p_abox_pdev;

static int __init s9p_abox_init(void)
{
	int ret = platform_driver_register(&s9p_abox_driver);

	if (ret)
		return ret;
	s9p_abox_pdev = platform_device_register_simple("s9p-abox", -1, NULL, 0);
	if (IS_ERR(s9p_abox_pdev)) {
		platform_driver_unregister(&s9p_abox_driver);
		return PTR_ERR(s9p_abox_pdev);
	}
	return 0;
}

static void __exit s9p_abox_exit(void)
{
	platform_device_unregister(s9p_abox_pdev);
	platform_driver_unregister(&s9p_abox_driver);
}

module_init(s9p_abox_init);
module_exit(s9p_abox_exit);
MODULE_DESCRIPTION("Galaxy S9+ ABOX audio DSP: firmware boot and IPC");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("calliope_sram.bin");
MODULE_FIRMWARE("calliope_dram.bin");
MODULE_FIRMWARE("calliope_iva.bin");
