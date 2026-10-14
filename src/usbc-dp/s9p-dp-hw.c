// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (Exynos 9810) DisplayPort: combo PHY, DP TX controller, AUX,
 * link training, SST video and the DECON2 + DPU DMA scanout path.
 *
 * Register sequences follow Samsung's SM-G965F 4.9 kernel
 * (drivers/video/fbdev/exynos/dpu_9810/displayport_*.c, decon_reg.c,
 * dpp_reg.c; drivers/phy/phy-exynos-usbdp.c, phy-exynos-usb3p1.c); the
 * analysis with file:line references is in video-dp/analisis/02 and 03.
 *
 * Differences from the vendor flow, all deliberate:
 *  - The USB side of the combo PHY is the port's USB 2.0-only driver,
 *    which keeps PIPE3 forced. Only COMBO_PMA_CTRL and the PMA's CMN/DP
 *    registers are programmed here; LINK_CTRL.PIPE3_FORCE_EN and
 *    CLKRST.LINK_PCLK_SEL are left alone (module parameter pipe3 = 1
 *    replicates the vendor's full PIPE3 handover).
 *  - DECON2 is fed by IDMA VG0 by default: it sits behind the same AXI
 *    port and System MMU (DPUD1, in bypass) as DECON0's G0, a path that is
 *    known to work on this device. G1 (the vendor's choice, DPUD2) is
 *    available with chan=g1.
 *  - Shared DECON0 registers (DP_CONNECTION_CONTROL, CLOCK_CONTROL_0[28])
 *    are read-modify-written and restored, never cleared blindly.
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>

#include "s9p-dp.h"

static bool pipe3;
module_param(pipe3, bool, 0644);
MODULE_PARM_DESC(pipe3, "replicate the vendor's PIPE3 handover (touches the USB link)");

static char *chan = "vg0";
module_param(chan, charp, 0444);
MODULE_PARM_DESC(chan, "DPU DMA channel for DECON2: vg0 (default) or g1");

static int win = 4;
module_param(win, int, 0444);
MODULE_PARM_DESC(win, "blender window used by DECON2 (DECON0 uses 5)");

static int max_rate = 0x0a;
module_param(max_rate, int, 0644);
MODULE_PARM_DESC(max_rate, "highest link rate code: 0x06 RBR, 0x0a HBR (default), 0x14 HBR2");

static int pixel_order;
module_param(pixel_order, int, 0644);
MODULE_PARM_DESC(pixel_order, "DECON2 OUTFIFO PIXEL_ORDER_SWAP (0 RGB, 4 BGR)");

static bool force_hpd;
module_param(force_hpd, bool, 0644);
MODULE_PARM_DESC(force_hpd, "force the controller's internal HPD high");

static bool ignore_pll;
module_param(ignore_pll, bool, 0644);
MODULE_PARM_DESC(ignore_pll, "bring the controller up even if the PHY PLL does not lock");

static bool kick_decon0 = true;
module_param(kick_decon0, bool, 0644);
MODULE_PARM_DESC(kick_decon0, "request DECON0's global shadow update so DP_CONNECTION_CONTROL latches");

static const struct s9p_dp_chan chans[] = {
	{ "vg0", 0x16073000, 0x16023000, 0, true },
	{ "g1",  0x16072000, 0x16022000, 3, false },
};

/* ------------------------------------------------------------------ */
/* register map                                                        */

/* DP controller, 0x11090000 */
#define SYSTEM_DEVICE_VERSION		0x0000
#define SYSTEM_SW_RESET_CONTROL		0x0004
#define SYSTEM_CLK_CONTROL		0x0008
#define SYSTEM_MAIN_LINK_BANDWIDTH	0x000c
#define SYSTEM_MAIN_LINK_LANE_COUNT	0x0010
#define SYSTEM_SW_FUNCTION_ENABLE	0x0014
#define SYSTEM_COMMON_FUNCTION_ENABLE	0x0018
#define  AUX_FUNC_EN			BIT(0)
#define  PCS_FUNC_EN			BIT(1)
#define SYSTEM_SST1_FUNCTION_ENABLE	0x001c
#define  SST1_VIDEO_FUNC_EN		BIT(0)
#define  SST1_LH_PWR_ON			BIT(4)
#define  SST1_LH_PWR_ON_STATUS		BIT(5)
#define SYSTEM_HPD_CONTROL		0x0028
#define  HPD_FORCE_EN			BIT(0)
#define  HPD_FORCE			BIT(1)
#define  HPD_STATUS			BIT(8)
#define SYSTEM_PLL_LOCK_CONTROL		0x002c
#define  PLL_LOCK_STATUS		BIT(4)
#define SYSTEM_IRQ_COMMON_STATUS	0x0108
#define SYSTEM_DEBUG_LH_PCH		0x0204
#define AUX_CONTROL			0x1000
#define  AUX_REPLY_TIMER_MODE		GENMASK(13, 12)
#define AUX_TRANSACTION_START		0x1004
#define AUX_BUFFER_CLEAR		0x1008
#define AUX_ADDR_ONLY_COMMAND		0x100c
#define AUX_REQUEST_CONTROL		0x1010
#define AUX_COMMAND_CONTROL		0x1014
#define  DEFER_CTRL_EN			BIT(8)
#define AUX_MONITOR_1			0x1018
#define AUX_MONITOR_2			0x101c
#define AUX_TX_DATA(i)			(0x1030 + 4 * (i))
#define AUX_RX_DATA(i)			(0x1040 + 4 * (i))
#define PCS_CONTROL			0x3000
#define  SCRAMBLE_BYPASS		BIT(0)
#define  LINK_TRAINING_PATTERN		GENMASK(5, 4)
#define PCS_LANE_CONTROL		0x3004
#define PCS_TEST_PATTERN_CONTROL	0x3008
#define SST1_MAIN_CONTROL		0x5000
#define  ENHANCED_MODE			BIT(5)
#define  VIDEO_MODE			BIT(6)
#define  MVID_MODE			BIT(11)
#define SST1_INTERRUPT_STATUS_SET0	0x5024
#define  MAPI_FIFO_UNDER_FLOW		BIT(8)
#define  VSYNC_DET			BIT(7)
#define SST1_INTERRUPT_STATUS_SET1	0x5028
#define SST1_INTERRUPT_MASK_SET0	0x502c
#define SST1_MVID_MASTER_MODE		0x5044
#define SST1_NVID_MASTER_MODE		0x5048
#define SST1_MVID_SFR_CONFIGURE		0x504c
#define SST1_NVID_SFR_CONFIGURE		0x5050
#define SST1_MVID_MONITOR		0x5054
#define SST1_LH_TU_INT_OFF		0x5080
#define SST1_LH_TU_FRAC_OFF		0x5084
#define SST1_LH_TU_THR_OFF		0x5088
#define SST1_LH_TU_SEL_OFF		0x508c
#define SST1_LH_TU_INT_ON		0x5090
#define SST1_LH_TU_FRAC_ON		0x5094
#define SST1_LH_TU_THR_ON		0x5098
#define SST1_LH_TU_SEL_ON		0x509c
#define SST1_ACTIVE_SYMBOL_MODE		0x5104
#define SST1_VIDEO_CONTROL		0x5400
#define  STRM_VALID_FORCE		(BIT(9) | BIT(8))
#define  BPC_MASK			GENMASK(6, 4)
#define  COLOR_FORMAT			GENMASK(3, 2)
#define  DYNAMIC_RANGE_CEA		BIT(7)
#define  VSYNC_NEG			BIT(1)
#define  HSYNC_NEG			BIT(0)
#define SST1_VIDEO_ENABLE		0x5404
#define SST1_VIDEO_MASTER_TIMING_GEN	0x5408
#define SST1_VIDEO_HTOTAL		0x5414
#define SST1_VIDEO_VTOTAL		0x5418
#define SST1_VIDEO_HFP			0x541c
#define SST1_VIDEO_HBP			0x5420
#define SST1_VIDEO_HACTIVE		0x5424
#define SST1_VIDEO_VFP			0x5428
#define SST1_VIDEO_VBP			0x542c
#define SST1_VIDEO_VACTIVE		0x5430
#define SST1_VIDEO_BIST_CONTROL		0x5450
#define  BIST_EN			BIT(3)
#define SST1_INFOFRAME_UPDATE		0x5c04
#define SST1_INFOFRAME_SEND		0x5c08
#define SST1_INFOFRAME_AVI_DATA(i)	(0x5c40 + 4 * (i))
#define SST2_INTERRUPT_STATUS_SET0	0x6024
#define SST2_INTERRUPT_STATUS_SET1	0x6028

/* combo PHY PMA, 0x110a0000: 8-bit registers on a 4-byte stride */
#define DP_REG(n)			(0x800 + 4 * (n))
#define DP_REG_0			DP_REG(0)
#define DP_REG_1			DP_REG(1)
#define  CMN_INIT_RSTN			BIT(0)
#define DP_REG_3			DP_REG(3)
#define DP_REG_4			DP_REG(4)
#define DP_REG_B			DP_REG(0xb)
#define DP_REG_F			DP_REG(0xf)
#define CMN_REG2C			0x0b0
#define CMN_REG2D			0x0b4
#define DP_REG_B3			0xacc

/* USBCON, 0x11100000 */
#define USBCON_LINK_CTRL		0x04
#define  PIPE3_FORCE_EN			BIT(16)
#define USBCON_CLKRST			0x20
#define  LINK_PCLK_SEL			BIT(7)
#define USBCON_COMBO_PMA_CTRL		0x48
#define  PMA_APB_SW_RST			BIT(0)
#define  PMA_INIT_SW_RST		BIT(1)
#define  PMA_CMN_SW_RST			BIT(2)
#define  PMA_TRSV_SW_RST		BIT(3)
#define  PMA_LOW_PWRN			BIT(4)
#define  PMA_REF_FREQ			GENMASK(9, 8)
#define  PMA_PLL_REF_REQ		GENMASK(11, 10)

/* PMU, 0x14060000 */
#define PMU_USB3DP_PHY_CTRL		0x0704
#define PMU_USB20_PHY_CTRL		0x072c
#define PMU_DPU_STATUS			0x4024

/* DECON (0x16030000 / 0x16050000) */
#define DECON_GLOBAL_CONTROL		0x0000
#define  DECON_SRESET			BIT(28)
#define  DECON_TEN_BPC			BIT(20)
#define  DECON_OPERATION_MODE_CMD	BIT(8)
#define  DECON_RUN_STATUS		BIT(4)
#define  DECON_EN			BIT(1)
#define  DECON_EN_F			BIT(0)
#define DECON_SRAM_SHARE_ENABLE		0x0030
#define DECON_INTERRUPT_ENABLE		0x0040
#define DECON_EXTRA_INTERRUPT_ENABLE	0x0044
#define DECON_INTERRUPT_PENDING		0x004c
#define  DECON_FRAME_DONE_INT		BIT(13)
#define  DECON_FRAME_START_INT		BIT(12)
#define  DECON_EXTRA_INT		BIT(4)
#define DECON_EXTRA_INTERRUPT_PENDING	0x0050
#define DECON_SHADOW_REG_UPDATE_REQ	0x0060
#define  SHADOW_GLOBAL			BIT(31)
#define DECON_HW_SW_TRIG_CONTROL	0x0070
#define DECON_SCALED_SIZE_CONTROL_0	0x00a0
#define DECON_CLOCK_CONTROL_0		0x00f0
#define  QACTIVE_PLL_VALUE		BIT(28)
#define DECON_OUTFIFO_SIZE_CONTROL_0	0x0120
#define DECON_OUTFIFO_TH_CONTROL_0	0x012c
#define DECON_OUTFIFO_DATA_ORDER	0x0130
#define DECON_BLENDER_BG_IMAGE_SIZE_0	0x0200
#define DECON_DATA_PATH_CONTROL_0	0x0214
#define DECON_DATA_PATH_CONTROL_1	0x0218
#define DECON_DATA_PATH_CONTROL_2	0x0230
#define DECON0_DP_CONNECTION_CONTROL	0x0270
#define DECON_FRAME_COUNT		0x02a0
#define DECON_RESOURCE_OCCUPANCY(i)	(0x0010 + 4 * (i))
#define DECON_SHADOW_OFFSET		0x7000
/* the shared window pool, in DECON0's space */
#define WIN_BASE(w)			(0x1000 + 0x30 * (w))
#define WIN_CONTROL_0			0x00
#define WIN_START_POSITION		0x08
#define WIN_END_POSITION		0x0c
#define WIN_COLORMAP_0			0x10
#define WIN_COLORMAP_1			0x14
#define WIN_START_TIME_CONTROL		0x18

/* IDMA channel / DPP */
#define IDMA_ENABLE			0x000
#define IDMA_IRQ			0x004
#define IDMA_IN_CON			0x008
#define  IDMA_IC_MAX			GENMASK(26, 19)
#define  IDMA_IMG_FORMAT		GENMASK(15, 11)
#define  IDMA_ROTATION			GENMASK(10, 8)
#define  IDMA_BLOCK_EN			BIT(3)
#define IDMA_OUT_CON			0x00c
#define IDMA_SRC_SIZE			0x010
#define IDMA_SRC_OFFSET			0x014
#define IDMA_IMG_SIZE			0x018
#define IDMA_IN_BASE_ADDR_Y		0x040
#define IDMA_IN_BASE_ADDR_C		0x044
#define IDMA_DYNAMIC_GATING_EN		0x058
#define IDMA_SHADOW			0x800
#define IDMA_CFG_ERR_STATE		0x870
#define DPP_ENABLE			0x000
#define DPP_IRQ				0x004
#define DPP_IN_CON			0x008
#define DPP_IMG_SIZE			0x018
#define DPP_MAIN_H_RATIO		0x044
#define DPP_MAIN_V_RATIO		0x048
#define DPP_V_COEF(n, s, x)		(0x200 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_H_COEF(n, s, x)		(0x290 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_DYNAMIC_GATING_EN		0xa54
#define DPP_LINECNT_CON			0xd00
#define DPU_DMA_QOS_LUT07_00		0x070
#define DPU_DMA_QOS_LUT15_08		0x078

#define CLK_GATE_MANUAL			BIT(20)
#define CLK_GATE_CG_VAL			BIT(21)

/* dpp_coef.h, scaling ratio 1:1 (selection 0), the 9 phases written */
static const s16 h_coef_8t[9][8] = {
	{   0,   0,   0, 512,   0,   0,   0,   0 },
	{  -2,   8, -25, 509,  30,  -9,   2,  -1 },
	{  -4,  14, -46, 499,  64, -19,   5,  -1 },
	{  -5,  20, -62, 482, 101, -30,   8,  -2 },
	{  -6,  23, -73, 458, 142, -41,  12,  -3 },
	{  -6,  25, -80, 429, 185, -53,  15,  -3 },
	{  -6,  26, -83, 395, 228, -63,  19,  -4 },
	{  -6,  25, -82, 357, 273, -71,  21,  -5 },
	{  -5,  23, -78, 316, 316, -78,  23,  -5 },
};

static const s16 v_coef_4t[9][4] = {
	{   0, 512,   0,   0 },
	{ -15, 508,  20,  -1 },
	{ -25, 495,  45,  -3 },
	{ -31, 473,  75,  -5 },
	{ -33, 443, 110,  -8 },
	{ -33, 408, 148, -11 },
	{ -31, 367, 190, -14 },
	{ -27, 324, 234, -19 },
	{ -23, 279, 279, -23 },
};

/* displayport_reg.c phy_tune_parameters[swing][pre] = { amp, emp, idrv_en } */
static const u8 phy_tune[4][4][3] = {
	{ { 0x04, 0x00, 0 }, { 0x00, 0x07, 0 }, { 0x02, 0x09, 1 }, { 0x00, 0x0d, 1 } },
	{ { 0x02, 0x00, 0 }, { 0x02, 0x06, 1 }, { 0x00, 0x0a, 1 }, { 0x00, 0x0a, 1 } },
	{ { 0x02, 0x00, 1 }, { 0x00, 0x06, 1 }, { 0x00, 0x06, 1 }, { 0x00, 0x06, 1 } },
	{ { 0x00, 0x00, 1 }, { 0x00, 0x00, 1 }, { 0x00, 0x00, 1 }, { 0x00, 0x00, 1 } },
};

static const u32 amp_base[4] = { 0x858, 0x8d8, 0x958, 0x9d8 };
static const u32 emp_base[4] = { 0x868, 0x8e8, 0x968, 0x9e8 };

static inline u32 rl(void __iomem *b, u32 o)
{
	return readl(b + o);
}

static inline void wl(void __iomem *b, u32 o, u32 v)
{
	writel(v, b + o);
}

static inline void rmw(void __iomem *b, u32 o, u32 clr, u32 set)
{
	writel((readl(b + o) & ~clr) | set, b + o);
}

/* field write: value shifted to the mask's LSB (vendor displayport_write_mask) */
static inline void wf(void __iomem *b, u32 o, u32 mask, u32 val)
{
	rmw(b, o, mask, (val << __ffs(mask)) & mask);
}

/* ------------------------------------------------------------------ */
/* mapping and clocks                                                  */

int s9p_dp_hw_map(struct s9p_dp *dp)
{
	int i;

	dp->chan = &chans[0];
	for (i = 0; i < ARRAY_SIZE(chans); i++)
		if (!strcmp(chan, chans[i].name))
			dp->chan = &chans[i];
	dp->win = clamp(win, 0, 5);
	if (dp->win == 5)
		dp_log(dp, "warning: window 5 is DECON0's");

	/* plain ioremap: s9p-decon already owns DECON0 / DPU_DMA regions */
	dp->link = ioremap(0x11090000, 0x10000);
	dp->pma = ioremap(0x110a0000, 0x1000);
	dp->pcs = ioremap(0x110b0000, 0x800);
	dp->usbcon = ioremap(0x11100000, 0x200);
	dp->pmu = ioremap(0x14060000, 0x5000);
	dp->d0 = ioremap(0x16030000, 0x10000);
	dp->d2 = ioremap(0x16050000, 0x10000);
	dp->idma = ioremap(dp->chan->idma, 0x1000);
	dp->dpp = ioremap(dp->chan->dpp, 0x1000);
	dp->dma = ioremap(0x16070000, 0x100);
	dp->cmu_dpu = ioremap(0x16000000, 0x4000);
	dp->sysreg_dpu = ioremap(0x16011000, 0x100);
	dp->cmu_fsys0 = ioremap(0x11000000, 0x4000);
	if (!dp->link || !dp->pma || !dp->pcs || !dp->usbcon || !dp->pmu ||
	    !dp->d0 || !dp->d2 || !dp->idma || !dp->dpp || !dp->dma ||
	    !dp->cmu_dpu || !dp->sysreg_dpu || !dp->cmu_fsys0)
		return -ENOMEM;
	dp_log(dp, "channel %s (IDMA %08x, DPP %08x, CHMAP %u), window %u",
	       dp->chan->name, dp->chan->idma, dp->chan->dpp, dp->chan->chmap, dp->win);
	return 0;
}

void s9p_dp_hw_unmap(struct s9p_dp *dp)
{
	void __iomem **m[] = { &dp->link, &dp->pma, &dp->pcs, &dp->usbcon, &dp->pmu,
			       &dp->d0, &dp->d2, &dp->idma, &dp->dpp, &dp->dma,
			       &dp->cmu_dpu, &dp->sysreg_dpu, &dp->cmu_fsys0 };
	int i;

	for (i = 0; i < ARRAY_SIZE(m); i++)
		if (*m[i]) {
			iounmap(*m[i]);
			*m[i] = NULL;
		}
}

/* open a CMU gate (MANUAL + CG_VAL), logging before/after */
static void gate_open(struct s9p_dp *dp, void __iomem *base, u32 off, u32 phys)
{
	u32 v = rl(base, off);

	if ((v & CLK_GATE_MANUAL) && !(v & CLK_GATE_CG_VAL)) {
		wl(base, off, v | CLK_GATE_CG_VAL);
		dp_log(dp, "gate %08x: %08x -> %08x (opened)", phys, v, rl(base, off));
	} else {
		dp_log(dp, "gate %08x: %08x", phys, v);
	}
}

/* a Q-channel with ENABLE=0 and CLOCK_REQ=0 has its clock stopped */
static void qch_check(struct s9p_dp *dp, void __iomem *base, u32 off, u32 phys,
		      const char *name)
{
	u32 v = rl(base, off);

	if (!(v & BIT(0)) && !(v & BIT(1))) {
		wl(base, off, v | BIT(1));
		dp_log(dp, "qch %s %08x: %08x -> %08x (clock requested)", name, phys,
		       v, rl(base, off));
	} else {
		dp_log(dp, "qch %s %08x: %08x", name, phys, v);
	}
}

static void dp_clocks(struct s9p_dp *dp)
{
	dp_log(dp, "pd-dpu status %08x", rl(dp->pmu, PMU_DPU_STATUS));
	qch_check(dp, dp->cmu_dpu, 0x3020, 0x16003020, "DPU");
	qch_check(dp, dp->cmu_fsys0, 0x3014, 0x11003014, "DP_LINK");
	qch_check(dp, dp->cmu_fsys0, 0x3054, 0x11003054, "USBDPPHY");
	gate_open(dp, dp->cmu_fsys0, 0x2008, 0x11002008);	/* USBDPPHY_REF_SOC_PLL */
	dp_log(dp, "fsys0 usbdp ref mux %08x, fsys0 bus user %08x",
	       rl(dp->cmu_fsys0, 0x0240), rl(dp->cmu_fsys0, 0x0100));
}

/* ------------------------------------------------------------------ */
/* combo PHY                                                           */

static void pmu_phy_on(struct s9p_dp *dp)
{
	u32 a = rl(dp->pmu, PMU_USB3DP_PHY_CTRL), b = rl(dp->pmu, PMU_USB20_PHY_CTRL);

	if (!(a & 1))
		wl(dp->pmu, PMU_USB3DP_PHY_CTRL, a | 1);
	if (!(b & 1))
		wl(dp->pmu, PMU_USB20_PHY_CTRL, b | 1);
	if (!(a & 1) || !(b & 1))
		dp_log(dp, "PMU PHY isolation released: 704 %08x->%08x 72c %08x->%08x",
		       a, rl(dp->pmu, PMU_USB3DP_PHY_CTRL), b,
		       rl(dp->pmu, PMU_USB20_PHY_CTRL));
}

/* phy_exynos_usbdp_enable(): CMN/TRSV/PCS setup the DP path always ran on */
static void pma_table(struct s9p_dp *dp, bool port1)
{
	void __iomem *p = dp->pma, *c = dp->pcs;

	rmw(p, 0x150, 0, BIT(7));
	rmw(c, 0x200, 0, BIT(0));
	wl(c, 0x204, 0x000b0a13);
	rmw(c, 0x20c, BIT(0), 0);
	wl(c, 0x300, 0x3f);
	rmw(c, 0x42c, BIT(1), BIT(0));
	wl(p, 0x038, 0x44);
	wl(p, 0x03c, 0xff);
	wl(p, 0x040, 0xc8);
	wl(p, 0x094, 0x30);
	wl(p, 0x098, 0x4f);
	wl(p, 0x09c, 0xdc);
	wl(p, 0x14c, 0x85);
	rmw(p, 0x090, 0, BIT(0));
	if (!port1) {
		rmw(p, CMN_REG2C, GENMASK(2, 1), 3 << 1);
		rmw(p, CMN_REG2D, BIT(4), BIT(5));
		rmw(p, DP_REG_B3, BIT(7), BIT(6) | BIT(1) | BIT(0));
		wl(p, 0x858, 0);
		wl(p, 0x868, 0);
	} else {
		rmw(p, CMN_REG2C, GENMASK(2, 1), 0);
		rmw(p, CMN_REG2D, BIT(5), BIT(4));
		rmw(p, DP_REG_B3, BIT(6) | BIT(1) | BIT(0), BIT(7));
		wl(p, 0x958, 0);
		wl(p, 0x968, 0);
	}
	rmw(p, CMN_REG2C, 0, BIT(0));
	rmw(p, CMN_REG2D, GENMASK(3, 0), 1);
	wl(p, 0x1a0, 0x49);
	rmw(p, 0x13c, 0, BIT(0));
	wl(p, 0x144, 0x12);
	rmw(p, 0x0f4, 0, BIT(0));
	rmw(p, 0x218, 0, BIT(0));
	/* ss_tune (host values) */
	rmw(p, 0x0f0, 0xff, 0x70);
	rmw(p, 0x0e8, 0xf0, 0x40);
	rmw(p, 0x0ec, 0x0f, 0x04);
	rmw(p, 0x0c8, 0xf0, 0x70);
	rmw(p, 0x0cc, 0x3f, 5 << 3);
	rmw(p, 0x0c4, 0xcc, 3 << 2);
	rmw(p, 0x224, 0x38, 1 << 3);
	rmw(p, 0x218, 0xe0, 1 << 5);
	wl(p, 0x190, 0x0f);
	rmw(p, 0x150, BIT(7), 0);
	wl(p, 0x1b4, 0x78);
}

/* PMA power-up and reset release (phy_exynos_usb_v3p1_pma_ready / _sw_rst_release) */
static void pma_init(struct s9p_dp *dp)
{
	u32 v = rl(dp->usbcon, USBCON_COMBO_PMA_CTRL);

	dp_log(dp, "USBCON linkctrl %08x clkrst %08x pma %08x",
	       rl(dp->usbcon, USBCON_LINK_CTRL), rl(dp->usbcon, USBCON_CLKRST), v);
	rmw(dp->usbcon, USBCON_COMBO_PMA_CTRL, PMA_LOW_PWRN, 0);
	udelay(1);
	rmw(dp->usbcon, USBCON_COMBO_PMA_CTRL, 0,
	    PMA_APB_SW_RST | PMA_INIT_SW_RST | PMA_CMN_SW_RST | PMA_TRSV_SW_RST);
	udelay(1);
	rmw(dp->usbcon, USBCON_COMBO_PMA_CTRL,
	    PMA_APB_SW_RST | PMA_INIT_SW_RST | PMA_REF_FREQ | PMA_PLL_REF_REQ,
	    FIELD_PREP(PMA_REF_FREQ, 1));
	if (pipe3)
		rmw(dp->usbcon, USBCON_LINK_CTRL, PIPE3_FORCE_EN, 0);

	/* vendor used_phy_port = !gpg0-4, i.e. the raw orientation */
	pma_table(dp, !(dp->sw_sel ^ (dp->pin == S9P_DP_PIN_B)));

	rmw(dp->usbcon, USBCON_COMBO_PMA_CTRL, PMA_CMN_SW_RST | PMA_TRSV_SW_RST, 0);
	udelay(200);
	if (pipe3)
		rmw(dp->usbcon, USBCON_CLKRST, 0, LINK_PCLK_SEL);
	dp_log(dp, "PMA up: pma_ctrl %08x", rl(dp->usbcon, USBCON_COMBO_PMA_CTRL));
}

static void dp_phy_init_setting(struct s9p_dp *dp)
{
	static const u32 regs[][2] = {
		{ 0x844, 0x00 }, { 0x8c4, 0x00 }, { 0x944, 0x00 }, { 0x9c4, 0x00 },
		{ 0xb18, 0x20 }, { 0xb24, 0x1e }, { 0xb64, 0x30 }, { 0xba4, 0x30 },
		{ 0xb3c, 0x0d }, { 0xb7c, 0x08 }, { 0xbbc, 0x08 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		wl(dp->pma, regs[i][0], regs[i][1]);
}

static bool pin_4lane(u8 pin)
{
	return pin == S9P_DP_PIN_A || pin == S9P_DP_PIN_C || pin == S9P_DP_PIN_E;
}

/* displayport_reg_phy_mode_setting() */
static void dp_phy_mode(struct s9p_dp *dp)
{
	void __iomem *p = dp->pma;
	u32 lane_map;

	if (pin_4lane(dp->pin)) {
		rmw(p, CMN_REG2C, GENMASK(2, 0), (2 << 1) | 1);
		rmw(p, CMN_REG2D, BIT(5) | BIT(4), 0);
		rmw(p, DP_REG_B3, GENMASK(7, 6) | GENMASK(1, 0), 2);
		rmw(p, DP_REG_B, GENMASK(1, 0), 0);
		wl(p, DP_REG_0, 0xff);
	} else if (dp->sw_sel) {
		rmw(p, CMN_REG2C, GENMASK(2, 0), (3 << 1) | 1);
		rmw(p, CMN_REG2D, BIT(5) | BIT(4), BIT(5));
		rmw(p, DP_REG_B3, GENMASK(7, 6) | GENMASK(1, 0), (1 << 6) | 3);
		rmw(p, DP_REG_B, GENMASK(1, 0), 3);
		wl(p, DP_REG_0, 0xf3);
	} else {
		rmw(p, CMN_REG2C, GENMASK(2, 0), (0 << 1) | 1);
		rmw(p, CMN_REG2D, BIT(5) | BIT(4), BIT(4));
		rmw(p, DP_REG_B3, GENMASK(7, 6) | GENMASK(1, 0), (2 << 6) | 0);
		rmw(p, DP_REG_B, GENMASK(1, 0), 0);
		wl(p, DP_REG_0, 0xfc);
	}

	switch (dp->pin) {
	case S9P_DP_PIN_A:
		lane_map = dp->sw_sel ? 0x0213 : 0x1302;
		break;
	case S9P_DP_PIN_B:
		lane_map = dp->sw_sel ? 0x0123 : 0x3201;
		break;
	default:
		lane_map = dp->sw_sel ? 0x1023 : 0x2310;
		break;
	}
	wl(dp->link, PCS_LANE_CONTROL, lane_map);
	dp_log(dp, "PHY mode: pin %02x sw_sel %d: REG2C %02x REG2D %02x B3 %02x REG_B %02x REG_0 %02x lanes %04x",
	       dp->pin, dp->sw_sel, rl(p, CMN_REG2C), rl(p, CMN_REG2D),
	       rl(p, DP_REG_B3), rl(p, DP_REG_B), rl(p, DP_REG_0), lane_map);
}

/* (re)start the DP PHY PLL at the rate in SYSTEM_MAIN_LINK_BANDWIDTH */
static int dp_phy_restart(struct s9p_dp *dp, u8 rate, u8 lanes)
{
	u32 v;
	int ret;

	rmw(dp->pma, DP_REG_1, CMN_INIT_RSTN, 0);
	dp_phy_init_setting(dp);
	dp_phy_mode(dp);
	if (rate) {
		wl(dp->link, SYSTEM_MAIN_LINK_BANDWIDTH, rate);
		wl(dp->link, SYSTEM_MAIN_LINK_LANE_COUNT, lanes);
		rmw(dp->link, SST1_MAIN_CONTROL, ENHANCED_MODE, dp->enhanced ? ENHANCED_MODE : 0);
		udelay(60);
	}
	rmw(dp->pma, DP_REG_1, 0, CMN_INIT_RSTN);
	ret = readl_poll_timeout(dp->link + SYSTEM_PLL_LOCK_CONTROL, v,
				 v & PLL_LOCK_STATUS, 10, 10000);
	rmw(dp->pma, DP_REG_F, 0x0f, 0x0f);
	dp_log(dp, "PHY PLL %s at bw %02x (pll_lock_ctrl %08x)",
	       ret ? "NOT LOCKED" : "locked", rl(dp->link, SYSTEM_MAIN_LINK_BANDWIDTH), v);
	return ret;
}

/* displayport_reg_set_voltage_and_pre_emphasis() */
static void dp_phy_tune(struct s9p_dp *dp)
{
	/* logical lane feeding each PHY lane, -1 = unused (zeroed slot 0) */
	static const s8 map_ce[2][4] = { { 0, 1, 3, 2 }, { 2, 3, 1, 0 } };
	static const s8 map_a[2][4]  = { { 0, 3, 2, 1 }, { 1, 2, 3, 0 } };
	static const s8 map_b[2][4]  = { { 1, 0, -1, -1 }, { -1, -1, 0, 1 } };
	static const s8 map_df[2][4] = { { 0, 1, -1, -1 }, { -1, -1, 1, 0 } };
	const s8 *map;
	int l;

	switch (dp->pin) {
	case S9P_DP_PIN_A:
		map = map_a[dp->sw_sel];
		break;
	case S9P_DP_PIN_B:
		map = map_b[dp->sw_sel];
		break;
	case S9P_DP_PIN_C:
	case S9P_DP_PIN_E:
		map = map_ce[dp->sw_sel];
		break;
	default:
		map = map_df[dp->sw_sel];
		break;
	}
	for (l = 0; l < 4; l++) {
		int n = map[l];
		u8 v, p;

		if (n < 0)
			continue;
		v = min_t(u8, dp->swing[n], 3);
		p = min_t(u8, dp->pre[n], 3);
		wl(dp->pma, amp_base[l] + 4 * v,
		   phy_tune[v][p][0] | (phy_tune[v][p][2] << 4));
		wl(dp->pma, emp_base[l] + 4 * p, phy_tune[v][p][1]);
	}
	/* 2-lane pins: the vendor zeroes slot 0 of one idle PHY lane */
	if (!pin_4lane(dp->pin)) {
		l = dp->sw_sel ? 0 : 2;
		wl(dp->pma, amp_base[l], 0);
		wl(dp->pma, emp_base[l], 0);
	}
	wl(dp->pma, DP_REG_3, dp->swing[0] << 6 | dp->swing[1] << 4 |
			      dp->swing[2] << 2 | dp->swing[3]);
	wl(dp->pma, DP_REG_4, dp->pre[0] << 6 | dp->pre[1] << 4 |
			      dp->pre[2] << 2 | dp->pre[3]);
}

/* ------------------------------------------------------------------ */
/* controller bring-up                                                 */

/*
 * The USB 2.0 PHY driver does not know that DP uses the combo PHY: its
 * init sets PMA_LOW_PWRN again and its power_off isolates the PHY in the
 * PMU. Either one silently kills AUX and the main link.
 */
/* exported by the port's USB PHY driver from the DP-aware kernel on */
void s9p_usbdp_dp_hold(bool hold);

static void usbdp_hold(struct s9p_dp *dp, bool hold)
{
	void (*fn)(bool);

	if (dp->phy_held == hold)
		return;
	fn = symbol_get(s9p_usbdp_dp_hold);
	if (fn) {
		fn(hold);
		symbol_put(s9p_usbdp_dp_hold);
		dp_log(dp, "combo PHY %s the USB PHY driver", hold ? "held against" : "released to");
	}
	dp->phy_held = hold;
}

bool s9p_dp_phy_ok(struct s9p_dp *dp)
{
	/* the PMU first: it is always readable, the PHY blocks may not be */
	if (!(rl(dp->pmu, PMU_USB3DP_PHY_CTRL) & 1))
		return false;
	if (rl(dp->usbcon, USBCON_COMBO_PMA_CTRL) & PMA_LOW_PWRN)
		return false;
	/* a USB PHY reset leaves the DP PHY in reset / its PLL unlocked */
	if (dp->phy_ready && (!(rl(dp->pma, DP_REG_1) & CMN_INIT_RSTN) ||
			      !(rl(dp->link, SYSTEM_PLL_LOCK_CONTROL) & PLL_LOCK_STATUS)))
		return false;
	return true;
}

int s9p_dp_hw_up(struct s9p_dp *dp)
{
	u32 v;
	int ret;

	if (dp->phy_ready && !s9p_dp_phy_ok(dp)) {
		dp_log(dp, "combo PHY was powered down behind our back (pmu %08x pma %08x)",
		       rl(dp->pmu, PMU_USB3DP_PHY_CTRL),
		       rl(dp->usbcon, USBCON_COMBO_PMA_CTRL));
		dp->phy_ready = false;
		dp->trained = false;
	}
	if (dp->phy_ready)
		return 0;
	usbdp_hold(dp, true);
	dp_clocks(dp);
	pmu_phy_on(dp);
	v = rl(dp->link, SYSTEM_DEVICE_VERSION);
	dp_log(dp, "DP controller version %08x, clk_control %08x, hpd %08x",
	       v, rl(dp->link, SYSTEM_CLK_CONTROL), rl(dp->link, SYSTEM_HPD_CONTROL));

	pma_init(dp);

	/* controller soft reset */
	rmw(dp->link, SYSTEM_SW_RESET_CONTROL, 0, BIT(0));
	ret = readl_poll_timeout(dp->link + SYSTEM_SW_RESET_CONTROL, v,
				 !(v & BIT(0)), 1, 1000);
	if (ret)
		dp_log(dp, "controller soft reset did not complete (%08x)", v);

	ret = dp_phy_restart(dp, 0, 0);
	if (ret && !ignore_pll) {
		dp_log(dp, "PHY PLL did not lock: not enabling AUX (ignore_pll=1 overrides)");
		usbdp_hold(dp, false);
		return ret;
	}
	rmw(dp->link, SYSTEM_COMMON_FUNCTION_ENABLE, 0, PCS_FUNC_EN | AUX_FUNC_EN);
	rmw(dp->link, SYSTEM_SST1_FUNCTION_ENABLE, 0, SST1_VIDEO_FUNC_EN);
	rmw(dp->link, SYSTEM_SW_FUNCTION_ENABLE, 0, BIT(0));
	wl(dp->link, SYSTEM_IRQ_COMMON_STATUS, ~0);
	wl(dp->link, SST1_INTERRUPT_STATUS_SET0, ~0);
	wl(dp->link, SST1_INTERRUPT_STATUS_SET1, ~0);
	wl(dp->link, SST2_INTERRUPT_STATUS_SET0, ~0);
	wl(dp->link, SST2_INTERRUPT_STATUS_SET1, ~0);
	if (force_hpd)
		rmw(dp->link, SYSTEM_HPD_CONTROL, 0, HPD_FORCE_EN | HPD_FORCE);
	dp->phy_ready = true;
	dp_log(dp, "controller up (PLL %s), hpd_control %08x",
	       ret ? "unlocked" : "locked", rl(dp->link, SYSTEM_HPD_CONTROL));
	return 0;
}

/* bring the PHY back after the USB driver powered it down (M5) */
int s9p_dp_hw_recover(struct s9p_dp *dp)
{
	dp->phy_ready = false;
	dp->trained = false;
	return s9p_dp_hw_up(dp);
}

void s9p_dp_hw_down(struct s9p_dp *dp)
{
	if (!dp->phy_ready) {
		usbdp_hold(dp, false);
		return;
	}
	rmw(dp->link, SST1_VIDEO_ENABLE, BIT(0), 0);
	rmw(dp->link, SYSTEM_SW_FUNCTION_ENABLE, BIT(0), 0);
	rmw(dp->pma, DP_REG_1, CMN_INIT_RSTN, 0);
	wl(dp->pma, DP_REG_0, 0);
	/* the PMA stays powered: the USB driver owns PMA_LOW_PWRN */
	usbdp_hold(dp, false);
	dp->phy_ready = false;
	dp->trained = false;
	dp_log(dp, "DP PHY and controller down");
}

/* ------------------------------------------------------------------ */
/* AUX                                                                  */

static ssize_t aux_once(struct s9p_dp *dp, struct drm_dp_aux_msg *msg);

/* the vendor retries every transfer 3 times (AUX_RETRY_COUNT) */
static ssize_t s9p_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	struct s9p_dp *dp = container_of(aux, struct s9p_dp, aux);
	ssize_t ret = -EIO;
	int i;

	if (!dp->phy_ready || !s9p_dp_phy_ok(dp))
		return -ENODEV;
	for (i = 0; i < 3; i++) {
		ret = aux_once(dp, msg);
		if (ret >= 0 || (msg->request & DP_AUX_NATIVE_WRITE))
			break;
	}
	return ret;
}

static ssize_t aux_once(struct s9p_dp *dp, struct drm_dp_aux_msg *msg)
{
	u8 *buf = msg->buffer;
	size_t len = msg->size, i;
	u32 v, mon1, mon2, req = msg->request & 0xf;
	bool write = !(req & DP_AUX_I2C_READ);	/* bit 0 = read for both kinds */
	int ret;

	if (len > 16)
		return -E2BIG;

	wl(dp->link, AUX_BUFFER_CLEAR, 1);
	rmw(dp->link, AUX_COMMAND_CONTROL, 0, DEFER_CTRL_EN);
	wf(dp->link, AUX_CONTROL, AUX_REPLY_TIMER_MODE, 3);
	wl(dp->link, AUX_ADDR_ONLY_COMMAND, len ? 0 : 1);
	wl(dp->link, AUX_REQUEST_CONTROL, req << 28 | (msg->address & 0xfffff) << 8 |
					  (len ? len - 1 : 0));
	if (write)
		for (i = 0; i < len; i += 4) {
			u32 w = 0;
			int k;

			for (k = 0; k < 4 && i + k < len; k++)
				w |= buf[i + k] << (8 * k);
			wl(dp->link, AUX_TX_DATA(i / 4), w);
		}

	wl(dp->link, AUX_TRANSACTION_START, 1);
	ret = readl_poll_timeout(dp->link + AUX_TRANSACTION_START, v, !(v & 1),
				 10, 50000);
	mon1 = rl(dp->link, AUX_MONITOR_1);
	mon2 = rl(dp->link, AUX_MONITOR_2);
	wl(dp->link, AUX_ADDR_ONLY_COMMAND, 0);
	if (ret) {
		dp_log(dp, "AUX %x @%05x len %zu: timeout (mon %08x/%08x)", req,
		       msg->address, len, mon1, mon2);
		return -ETIMEDOUT;
	}
	if (((mon1 >> 8) & 0xf) || (mon2 & 0xff)) {
		/* ratelimit noise from EDID probing of absent blocks */
		dev_dbg(dp->dev, "AUX %x @%05x len %zu: status %x err %x\n", req,
			msg->address, len, (mon1 >> 8) & 0xf, mon2 & 0xff);
		udelay(400);
		return -EIO;
	}
	msg->reply = (mon1 >> 4) & 0xf;
	if (!write && len) {
		size_t got = (mon1 >> 24) & 0x7f;

		if (got && got < len)
			len = got;
		for (i = 0; i < len; i++)
			buf[i] = rl(dp->link, AUX_RX_DATA(i / 4)) >> (8 * (i % 4));
	}
	return len;
}

int s9p_dp_aux_register(struct s9p_dp *dp)
{
	dp->aux.name = "s9p-dp-aux";
	dp->aux.dev = dp->dev;
	dp->aux.drm_dev = &dp->drm;
	dp->aux.transfer = s9p_aux_transfer;
	drm_dp_aux_init(&dp->aux);
	return drm_dp_aux_register(&dp->aux);
}

int s9p_dp_probe_sink(struct s9p_dp *dp)
{
	u8 count = 0;
	int i, ret;

	for (i = 0; i < 4; i++) {
		ret = drm_dp_read_dpcd_caps(&dp->aux, dp->dpcd);
		if (!ret)
			break;
		msleep(50);
	}
	if (ret) {
		dp_log(dp, "no DPCD answer: %d", ret);
		return ret;
	}
	for (i = 0; i < 200; i++) {
		if (drm_dp_dpcd_readb(&dp->aux, DP_SINK_COUNT, &count) == 1 &&
		    DP_GET_SINK_COUNT(count))
			break;
		msleep(20);
	}
	dp->tps3 = drm_dp_tps3_supported(dp->dpcd);
	dp->enhanced = drm_dp_enhanced_frame_cap(dp->dpcd);
	dp_log(dp, "DPCD rev %02x max rate %02x lanes %u%s%s sink count %u (%*ph)",
	       dp->dpcd[DP_DPCD_REV], dp->dpcd[DP_MAX_LINK_RATE],
	       dp->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK,
	       dp->tps3 ? " TPS3" : "", dp->enhanced ? " enhanced" : "",
	       DP_GET_SINK_COUNT(count), 12, dp->dpcd);
	dp->connected = true;
	return 0;
}

/* ------------------------------------------------------------------ */
/* link training (displayport_full_link_training)                      */

static unsigned int rate_khz(u8 rate)
{
	return rate == 0x14 ? 540000 : rate == 0x0a ? 270000 : 162000;
}

static u8 pin_lanes(struct s9p_dp *dp)
{
	return pin_4lane(dp->pin) ? 4 : 2;
}

static u8 sink_lanes(struct s9p_dp *dp)
{
	u8 l = dp->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;

	l = min_t(u8, l ? l : 4, pin_lanes(dp));
	return l >= 4 ? 4 : l >= 2 ? 2 : 1;
}

static u8 rate_cap(struct s9p_dp *dp)
{
	u8 r = min_t(u8, dp->dpcd[DP_MAX_LINK_RATE] ? dp->dpcd[DP_MAX_LINK_RATE] : 0x0a,
		     max_rate);

	return r >= 0x14 ? 0x14 : r >= 0x0a ? 0x0a : 0x06;
}

/* 8b/10b: each lane carries rate_khz * 8 kbit/s; 24 bpp; 3% margin */
static bool fits(int clock_khz, u8 lanes, u8 rate)
{
	return (u64)clock_khz * 24 * 103 <= (u64)lanes * rate_khz(rate) * 8 * 100;
}

bool s9p_dp_mode_fits(struct s9p_dp *dp, int clock_khz)
{
	return fits(clock_khz, sink_lanes(dp), rate_cap(dp));
}

static int dpcd_w(struct s9p_dp *dp, unsigned int reg, u8 v)
{
	return drm_dp_dpcd_writeb(&dp->aux, reg, v) == 1 ? 0 : -EIO;
}

static void set_drive(struct s9p_dp *dp)
{
	u8 b[4];
	int i;

	dp_phy_tune(dp);
	for (i = 0; i < 4; i++)
		b[i] = dp->pre[i] << DP_TRAIN_PRE_EMPHASIS_SHIFT | dp->swing[i] |
		       (dp->swing[i] >= 3 ? DP_TRAIN_MAX_SWING_REACHED : 0) |
		       (dp->pre[i] >= 3 ? DP_TRAIN_MAX_PRE_EMPHASIS_REACHED : 0);
	drm_dp_dpcd_write(&dp->aux, DP_TRAINING_LANE0_SET, b, dp->lanes);
}

static void take_adjust(struct s9p_dp *dp, const u8 st[DP_LINK_STATUS_SIZE])
{
	int i;

	for (i = 0; i < dp->lanes; i++) {
		dp->swing[i] = drm_dp_get_adjust_request_voltage(st, i) >>
			       DP_TRAIN_VOLTAGE_SWING_SHIFT;
		dp->pre[i] = drm_dp_get_adjust_request_pre_emphasis(st, i) >>
			     DP_TRAIN_PRE_EMPHASIS_SHIFT;
	}
}

static void train_delay(struct s9p_dp *dp, bool eq)
{
	u8 interval = dp->dpcd[DP_TRAINING_AUX_RD_INTERVAL] & DP_TRAINING_AUX_RD_MASK;

	if (interval)
		msleep(interval * 4);
	else
		usleep_range(eq ? 400 : 100, eq ? 500 : 200);
}

static void set_pattern(struct s9p_dp *dp, int tps)
{
	wf(dp->link, PCS_TEST_PATTERN_CONTROL, GENMASK(2, 0), 0);
	wf(dp->link, PCS_CONTROL, LINK_TRAINING_PATTERN, tps);
	rmw(dp->link, PCS_CONTROL, SCRAMBLE_BYPASS, tps ? SCRAMBLE_BYPASS : 0);
}

static int train_at(struct s9p_dp *dp, u8 rate, bool restart)
{
	u8 st[DP_LINK_STATUS_SIZE], last[4] = {}, buf[2];
	int i, same = 0, changes = 0, eq_retry = 0, ret;

	memset(dp->swing, 0, sizeof(dp->swing));
	memset(dp->pre, 0, sizeof(dp->pre));
	/*
	 * Vendor: the PHY is only reset when rate or lane count change, and
	 * never while DECON streams (displayport_full_link_training).
	 */
	if (restart) {
		ret = dp_phy_restart(dp, rate, dp->lanes);
		if (ret)
			return ret;
	}
	buf[0] = rate;
	buf[1] = dp->lanes | (dp->enhanced ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0);
	drm_dp_dpcd_write(&dp->aux, DP_LINK_BW_SET, buf, 2);
	dpcd_w(dp, DP_DOWNSPREAD_CTRL, 0);
	dpcd_w(dp, DP_MAIN_LINK_CHANNEL_CODING_SET, DP_SET_ANSI_8B10B);

	/* clock recovery */
	set_pattern(dp, 1);
	dpcd_w(dp, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_1 |
				DP_LINK_SCRAMBLING_DISABLE);
	for (;;) {
		set_drive(dp);
		train_delay(dp, false);
		if (drm_dp_dpcd_read_link_status(&dp->aux, st) < 0)
			return -EIO;
		if (drm_dp_clock_recovery_ok(st, dp->lanes))
			break;
		for (i = 0; i < dp->lanes; i++)
			if (dp->swing[i] < 3)
				break;
		if (i == dp->lanes) {
			dp_log(dp, "CR failed at max swing (rate %02x)", rate);
			return -EAGAIN;
		}
		take_adjust(dp, st);
		if (!memcmp(last, dp->swing, 4)) {
			if (++same >= 5) {
				dp_log(dp, "CR stuck (rate %02x, status %6ph)", rate, st);
				return -EAGAIN;
			}
		} else {
			same = 0;
			if (++changes > 16)
				return -EAGAIN;
		}
		memcpy(last, dp->swing, 4);
	}
	dp_log(dp, "CR done at rate %02x x%u (swing %4ph pre %4ph)", rate,
	       dp->lanes, dp->swing, dp->pre);

	/* channel equalisation */
	if (dp->tps3) {
		set_pattern(dp, 3);
		dpcd_w(dp, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_3 |
					DP_LINK_SCRAMBLING_DISABLE);
	} else {
		set_pattern(dp, 2);
		dpcd_w(dp, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_2 |
					DP_LINK_SCRAMBLING_DISABLE);
	}
	for (;;) {
		set_drive(dp);
		train_delay(dp, true);
		if (drm_dp_dpcd_read_link_status(&dp->aux, st) < 0)
			return -EIO;
		if (!drm_dp_clock_recovery_ok(st, dp->lanes)) {
			dp_log(dp, "CR lost during EQ (status %6ph)", st);
			return -EAGAIN;
		}
		if (drm_dp_channel_eq_ok(st, dp->lanes)) {
			set_pattern(dp, 0);
			dpcd_w(dp, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);
			dp_log(dp, "link trained: rate %02x x%u, swing %4ph pre %4ph",
			       rate, dp->lanes, dp->swing, dp->pre);
			return 0;
		}
		if (++eq_retry >= 5) {
			dp_log(dp, "EQ failed (status %6ph)", st);
			set_pattern(dp, 0);
			dpcd_w(dp, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);
			return -EIO;
		}
		take_adjust(dp, st);
	}
}

int s9p_dp_train(struct s9p_dp *dp, const struct drm_display_mode *mode)
{
	u8 cap = rate_cap(dp), rate = 0x06;
	int ret;

	dp->lanes = sink_lanes(dp);
	/* lowest rate that carries the mode (vendor get_min_link_rate) */
	if (mode) {
		if (!fits(mode->clock, dp->lanes, 0x06))
			rate = fits(mode->clock, dp->lanes, 0x0a) ? 0x0a : 0x14;
	} else {
		rate = cap;
	}
	rate = min(rate, cap);
	dpcd_w(dp, DP_SET_POWER, DP_SET_POWER_D0);
	usleep_range(1000, 2000);
	for (;;) {
		ret = train_at(dp, rate, true);
		if (!ret)
			break;
		if (rate == 0x06 || ret == -EIO) {
			dpcd_w(dp, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);
			dp_log(dp, "link training failed at every rate");
			dp->trained = false;
			return ret;
		}
		rate = rate == 0x14 ? 0x0a : 0x06;
	}
	dp->rate = rate;
	dp->trained = true;
	if (mode && !fits(mode->clock, dp->lanes, rate)) {
		dp_log(dp, "trained link (%02x x%u) is too slow for %d kHz",
		       rate, dp->lanes, mode->clock);
		return -ENOSPC;
	}
	return 0;
}

/* HPD IRQ with the link lost: same rate and lanes, PHY not reset */
int s9p_dp_retrain(struct s9p_dp *dp)
{
	int ret;

	if (!dp->trained)
		return -ENOLINK;
	ret = train_at(dp, dp->rate, false);
	if (ret) {
		dp->trained = false;
		dp_log(dp, "in-place retrain failed: %d", ret);
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* SST1 video                                                           */

static void dp_set_tu(struct s9p_dp *dp, int clock_khz)
{
	const u64 E10 = 10000000000ULL;
	u64 tu_off, tu_on;
	u32 int_off, int_on, frac_off, frac_on, thr_off, thr_on;

	tu_off = div64_u64((u64)clock_khz * 24 * 32 * E10,
			   (u64)dp->lanes * rate_khz(dp->rate) * 8);
	tu_on = div64_u64(tu_off * 1000, 976);
	int_off = div64_u64(tu_off, E10);
	frac_off = div64_u64(tu_off - int_off * E10, 10);
	int_on = div64_u64(tu_on, E10);
	frac_on = div64_u64(tu_on - int_on * E10, 10);
	thr_off = int_off <= 2 ? 7 : int_off <= 5 ? 8 : 9;
	thr_on = int_on <= 2 ? 7 : int_on <= 5 ? 8 : 9;

	rmw(dp->link, SST1_ACTIVE_SYMBOL_MODE, 0, BIT(0));
	rmw(dp->link, SST1_LH_TU_SEL_OFF, 0, BIT(0));
	rmw(dp->link, SST1_LH_TU_SEL_ON, 0, BIT(0));
	wf(dp->link, SST1_LH_TU_INT_OFF, GENMASK(5, 0), int_off);
	wf(dp->link, SST1_LH_TU_FRAC_OFF, GENMASK(29, 0), frac_off);
	wl(dp->link, SST1_LH_TU_THR_OFF, thr_off);
	wf(dp->link, SST1_LH_TU_INT_ON, GENMASK(5, 0), int_on);
	wf(dp->link, SST1_LH_TU_FRAC_ON, GENMASK(29, 0), frac_on);
	wf(dp->link, SST1_LH_TU_THR_ON, GENMASK(3, 0), thr_on);
	dp_log(dp, "TU: off %u.%09u thr %u, on %u.%09u thr %u", int_off, frac_off,
	       thr_off, int_on, frac_on, thr_on);
}

static void dp_set_avi(struct s9p_dp *dp, const struct drm_display_mode *mode)
{
	u8 vic = drm_match_cea_mode(mode);

	/* data[0] = 0x10 (AFI present), data[1] = 0x08 (aspect as picture), data[3] = VIC */
	wl(dp->link, SST1_INFOFRAME_AVI_DATA(0), (u32)vic << 24 | 0x0810);
	wl(dp->link, SST1_INFOFRAME_AVI_DATA(1), 0);
	wl(dp->link, SST1_INFOFRAME_AVI_DATA(2), 0);
	wl(dp->link, SST1_INFOFRAME_AVI_DATA(3), 0);
	rmw(dp->link, SST1_INFOFRAME_UPDATE, 0, BIT(2));
	rmw(dp->link, SST1_INFOFRAME_SEND, 0, BIT(2));
}

int s9p_dp_video_on(struct s9p_dp *dp, const struct drm_display_mode *m, bool bist)
{
	unsigned int ls = rate_khz(dp->rate);
	u32 ctl;

	if (!dp->trained)
		return -ENOLINK;
	ctl = rl(dp->link, SST1_VIDEO_CONTROL);
	ctl &= ~(DYNAMIC_RANGE_CEA | BPC_MASK | COLOR_FORMAT | VSYNC_NEG | HSYNC_NEG |
		 STRM_VALID_FORCE);
	ctl |= FIELD_PREP(BPC_MASK, 1);		/* 8 bpc */
	if (m->flags & DRM_MODE_FLAG_NHSYNC)
		ctl |= HSYNC_NEG;
	if (m->flags & DRM_MODE_FLAG_NVSYNC)
		ctl |= VSYNC_NEG;
	wl(dp->link, SST1_VIDEO_CONTROL, ctl);

	wl(dp->link, SST1_VIDEO_VTOTAL, m->vtotal);
	wl(dp->link, SST1_VIDEO_HTOTAL, m->htotal);
	wl(dp->link, SST1_VIDEO_VACTIVE, m->vdisplay);
	wl(dp->link, SST1_VIDEO_VFP, m->vsync_start - m->vdisplay);
	wl(dp->link, SST1_VIDEO_VBP, m->vtotal - m->vsync_end);
	wl(dp->link, SST1_VIDEO_HACTIVE, m->hdisplay);
	wl(dp->link, SST1_VIDEO_HFP, m->hsync_start - m->hdisplay);
	wl(dp->link, SST1_VIDEO_HBP, m->htotal - m->hsync_end);

	wl(dp->link, SST1_MVID_MASTER_MODE, m->clock >> 1);
	wl(dp->link, SST1_NVID_MASTER_MODE, ls);
	rmw(dp->link, SST1_MAIN_CONTROL, 0, MVID_MODE);
	wl(dp->link, SST1_MVID_SFR_CONFIGURE, m->clock);
	wl(dp->link, SST1_NVID_SFR_CONFIGURE, ls);

	dp_set_tu(dp, m->clock);
	rmw(dp->link, SST1_VIDEO_MASTER_TIMING_GEN, 0, BIT(0));
	rmw(dp->link, SST1_MAIN_CONTROL, VIDEO_MODE, 0);
	dp_set_avi(dp, m);

	if (bist) {
		wf(dp->link, SST1_VIDEO_BIST_CONTROL, GENMASK(1, 0), 0);	/* colour bars */
		rmw(dp->link, SST1_VIDEO_CONTROL, 0, STRM_VALID_FORCE);
		rmw(dp->link, SST1_VIDEO_BIST_CONTROL, 0, BIST_EN);
	} else {
		rmw(dp->link, SST1_VIDEO_BIST_CONTROL, BIST_EN, 0);
	}
	wl(dp->link, SST1_INTERRUPT_STATUS_SET0, ~0);
	rmw(dp->link, SST1_VIDEO_ENABLE, 0, BIT(0));
	dp->bist = bist;
	drm_mode_copy(&dp->cur_mode, m);
	dp_log(dp, "video on: %dx%d@%d %d kHz %s, ctl %08x, rate %02x x%u%s",
	       m->hdisplay, m->vdisplay, drm_mode_vrefresh(m), m->clock,
	       m->flags & DRM_MODE_FLAG_NHSYNC ? "-h" : "+h", rl(dp->link, SST1_VIDEO_CONTROL),
	       dp->rate, dp->lanes, bist ? " (BIST colour bars)" : "");
	return 0;
}

void s9p_dp_video_off(struct s9p_dp *dp)
{
	rmw(dp->link, SST1_VIDEO_CONTROL, STRM_VALID_FORCE, 0);
	rmw(dp->link, SST1_VIDEO_BIST_CONTROL, BIST_EN, 0);
	rmw(dp->link, SST1_VIDEO_ENABLE, BIT(0), 0);
	dp->bist = false;
	dp_log(dp, "video off");
}

/* ------------------------------------------------------------------ */
/* DECON2 + IDMA/DPP                                                   */

static int lh_power(struct s9p_dp *dp, bool on)
{
	u32 v;
	int ret;

	rmw(dp->link, SYSTEM_SST1_FUNCTION_ENABLE, SST1_LH_PWR_ON, on ? SST1_LH_PWR_ON : 0);
	ret = readl_poll_timeout(dp->link + SYSTEM_SST1_FUNCTION_ENABLE, v,
				 !!(v & SST1_LH_PWR_ON_STATUS) == on, 1, 20000);
	if (ret)
		dp_log(dp, "LH P-channel power %s: timeout (%08x, lh fsm %08x)",
		       on ? "on" : "off", v, rl(dp->link, SYSTEM_DEBUG_LH_PCH));
	return ret;
}

static int decon2_wait_idle(struct s9p_dp *dp)
{
	u32 v;

	return readl_poll_timeout(dp->d2 + DECON_GLOBAL_CONTROL, v,
				  !(v & DECON_RUN_STATUS), 100, 60000);
}

static void decon2_reset(struct s9p_dp *dp)
{
	u32 v;

	rmw(dp->d2, DECON_GLOBAL_CONTROL, 0, DECON_SRESET);
	if (readl_poll_timeout(dp->d2 + DECON_GLOBAL_CONTROL, v, !(v & DECON_SRESET),
			       10, 20000))
		dp_log(dp, "DECON2 SRESET did not complete (%08x)", v);
}

static void dpu_prechecks(struct s9p_dp *dp)
{
	/* APB of DECON2 / DPP / DPU_DMA: a gated APB hangs the SoC on access */
	gate_open(dp, dp->cmu_dpu, 0x201c, 0x1600201c);
	gate_open(dp, dp->cmu_dpu, 0x2024, 0x16002024);
	gate_open(dp, dp->cmu_dpu, 0x202c, 0x1600202c);
	/* the AXI paths: D1 is DECON0's (reference), D2 is G1's */
	dp_log(dp, "QCH D1: btm %08x lhs %08x ppmu %08x smmu %08x | D2: btm %08x lhs %08x ppmu %08x smmu %08x",
	       rl(dp->cmu_dpu, 0x3014), rl(dp->cmu_dpu, 0x3038), rl(dp->cmu_dpu, 0x3044),
	       rl(dp->cmu_dpu, 0x3050), rl(dp->cmu_dpu, 0x3018), rl(dp->cmu_dpu, 0x303c),
	       rl(dp->cmu_dpu, 0x3048), rl(dp->cmu_dpu, 0x3054));
	{
		void __iomem *top = ioremap(0x1a241000, 0x1000);

		if (top) {
			/* ACLK = MUX_CLKCMU_DPU_BUS source / (DIV + 1); CLKCMU_DPU_BUS picks it */
			dp_log(dp, "DPU aclk: final mux %08x, DPU_BUS mux %08x div %08x, DPU div %08x, user mux %08x",
			       readl(top + 0x000), readl(top + 0x048), readl(top + 0x8cc),
			       readl(top + 0x8c8), rl(dp->cmu_dpu, 0x0100));
			iounmap(top);
		}
	}
	dp_log(dp, "DECON0 run %08x win %08x chmap %08x sram %08x out %08x",
	       rl(dp->d0, DECON_GLOBAL_CONTROL),
	       rl(dp->d0, DECON_DATA_PATH_CONTROL_0), rl(dp->d0, DECON_DATA_PATH_CONTROL_1),
	       rl(dp->d0, DECON_SRAM_SHARE_ENABLE), rl(dp->d0, DECON_OUTFIFO_DATA_ORDER));
}

static void dpp_init(struct s9p_dp *dp)
{
	void __iomem *i = dp->idma, *p = dp->dpp;
	int n, s, x;

	rmw(i, IDMA_IRQ, 0x3f6, 0);
	rmw(p, DPP_IRQ, 0x42, 0);
	rmw(i, IDMA_IRQ, 0, BIT(0));
	rmw(p, DPP_IRQ, 0, BIT(0));
	rmw(i, IDMA_ENABLE, BIT(9), 0);
	rmw(p, DPP_ENABLE, GENMASK(10, 8), 0);
	if (rl(dp->dma, DPU_DMA_QOS_LUT07_00) != 0x44444444)
		wl(dp->dma, DPU_DMA_QOS_LUT07_00, 0x44444444);
	if (rl(dp->dma, DPU_DMA_QOS_LUT15_08) != 0x44444444)
		wl(dp->dma, DPU_DMA_QOS_LUT15_08, 0x44444444);
	rmw(i, IDMA_DYNAMIC_GATING_EN, GENMASK(26, 0), 0);
	rmw(p, DPP_DYNAMIC_GATING_EN, GENMASK(6, 0), 0);
	wf(i, IDMA_OUT_CON, GENMASK(31, 24), 0xff);
	rmw(p, DPP_LINECNT_CON, BIT(2), BIT(0));

	/* 1:1 scaling ratio (written for every channel by the vendor) */
	wl(p, DPP_MAIN_H_RATIO, 0x100000);
	wl(p, DPP_MAIN_V_RATIO, 0x100000);
	if (dp->chan->scaler) {
		for (n = 0; n < 9; n++)
			for (s = 0; s < 8; s++)
				for (x = 0; x < 2; x++)
					wl(p, DPP_H_COEF(n, s, x), (u32)(s32)h_coef_8t[n][s]);
		for (n = 0; n < 9; n++)
			for (s = 0; s < 4; s++)
				for (x = 0; x < 2; x++)
					wl(p, DPP_V_COEF(n, s, x), (u32)(s32)v_coef_4t[n][s]);
	}
}

static void dpp_config(struct s9p_dp *dp, u32 w, u32 h, u32 pitch, dma_addr_t addr)
{
	void __iomem *i = dp->idma, *p = dp->dpp;
	u32 con;

	wl(i, IDMA_SRC_OFFSET, 0);
	wl(i, IDMA_SRC_SIZE, h << 16 | (pitch / 4));
	wl(i, IDMA_IMG_SIZE, h << 16 | w);
	wl(p, DPP_IMG_SIZE, h << 16 | w);
	con = rl(i, IDMA_IN_CON);
	con &= ~(IDMA_ROTATION | IDMA_BLOCK_EN | IDMA_IMG_FORMAT);
	con |= FIELD_PREP(IDMA_IMG_FORMAT, 4);		/* XRGB8888 */
	if (!(con & IDMA_IC_MAX))
		con |= FIELD_PREP(IDMA_IC_MAX, 0x40);
	wl(i, IDMA_IN_CON, con);
	wl(i, IDMA_IN_BASE_ADDR_Y, lower_32_bits(addr));
	wl(i, IDMA_IN_BASE_ADDR_C, lower_32_bits(addr));
	rmw(p, DPP_IN_CON, BIT(3) | GENMASK(2, 0), 0);	/* per-frame alpha, ARGB8888 */
}

static void window_config(struct s9p_dp *dp, u32 w, u32 h)
{
	u32 wb = WIN_BASE(dp->win), en, map;
	int k;

	rmw(dp->d0, wb + WIN_CONTROL_0, GENMASK(31, 16) | GENMASK(11, 8) | GENMASK(1, 0),
	    0x00ff0102);
	wl(dp->d0, wb + WIN_START_POSITION, 0);
	wl(dp->d0, wb + WIN_END_POSITION, (h - 1) << 16 | (w - 1));
	wl(dp->d0, wb + WIN_START_TIME_CONTROL, 0);
	wl(dp->d0, wb + WIN_COLORMAP_0, 0);
	wl(dp->d0, wb + WIN_COLORMAP_1, 0);

	en = BIT(4 * dp->win);
	map = 0;
	for (k = 0; k < 6; k++)
		map |= (k == dp->win ? dp->chan->chmap : 5) << (4 * k);
	wl(dp->d2, DECON_DATA_PATH_CONTROL_0, en);
	wl(dp->d2, DECON_DATA_PATH_CONTROL_1, map);
}

static void decon0_shared(struct s9p_dp *dp, bool on)
{
	if (on) {
		if (!dp->d0_f0_valid) {
			dp->d0_f0_saved = rl(dp->d0, DECON_CLOCK_CONTROL_0);
			dp->d0_f0_valid = true;
		}
		rmw(dp->d0, DECON_CLOCK_CONTROL_0, 0, QACTIVE_PLL_VALUE);
		wf(dp->d0, DECON0_DP_CONNECTION_CONTROL, GENMASK(2, 0), 2);
	} else if (dp->d0_f0_valid) {
		rmw(dp->d0, DECON_CLOCK_CONTROL_0, QACTIVE_PLL_VALUE,
		    dp->d0_f0_saved & QACTIVE_PLL_VALUE);
		dp->d0_f0_valid = false;
	}
	if (kick_decon0) {
		u32 v;

		rmw(dp->d0, DECON_SHADOW_REG_UPDATE_REQ, 0, SHADOW_GLOBAL);
		/*
		 * The global shadow of DECON0 latches at its next TE-triggered
		 * frame, i.e. at the panel's next flip. With the desktop idle or
		 * the panel blanked there is none: release the hardware trigger
		 * once, exactly as s9p-decon does for a flip (HW_TRIG_EN set,
		 * HW_TRIG_MASK_DECON cleared, read-modify-write). The panel then
		 * refreshes the frame it already shows.
		 */
		if (on && readl_poll_timeout(dp->d0 + DECON_SHADOW_OFFSET +
					     DECON0_DP_CONNECTION_CONTROL, v,
					     (v & 7) == 2, 1000, 100000)) {
			rmw(dp->d0, DECON_HW_SW_TRIG_CONTROL, BIT(4), BIT(0));
			if (readl_poll_timeout(dp->d0 + DECON_SHADOW_OFFSET +
					       DECON0_DP_CONNECTION_CONTROL, v,
					       (v & 7) == 2, 1000, 200000))
				dp_log(dp, "DP_CONNECTION_CONTROL not latched by DECON0 (req %08x trig %08x)",
				       rl(dp->d0, DECON_SHADOW_REG_UPDATE_REQ),
				       rl(dp->d0, DECON_HW_SW_TRIG_CONTROL));
			else
				dp_log(dp, "DP_CONNECTION_CONTROL latched after a DECON0 trigger");
		}
	}
	dp_log(dp, "DECON0 shared: dp_conn %08x (shadow %08x) clkctl %08x (shadow %08x)",
	       rl(dp->d0, DECON0_DP_CONNECTION_CONTROL),
	       rl(dp->d0, DECON_SHADOW_OFFSET + DECON0_DP_CONNECTION_CONTROL),
	       rl(dp->d0, DECON_CLOCK_CONTROL_0),
	       rl(dp->d0, DECON_SHADOW_OFFSET + DECON_CLOCK_CONTROL_0));
}

int s9p_dp_decon_on(struct s9p_dp *dp, const struct drm_display_mode *m,
		    dma_addr_t addr, u32 pitch)
{
	u32 w = m->hdisplay, h = m->vdisplay, v, sram;
	int ret;

	if (upper_32_bits(addr)) {
		dp_log(dp, "scanout buffer above 4 GiB: %pad", &addr);
		return -EINVAL;
	}
	dpu_prechecks(dp);
	{
		/* never take a window DECON0 has enabled */
		u32 d0w = rl(dp->d0, DECON_DATA_PATH_CONTROL_0);
		int k;

		if (d0w & BIT(4 * dp->win)) {
			for (k = 4; k >= 0; k--)
				if (!(d0w & BIT(4 * k)))
					break;
			dp_log(dp, "window %u is DECON0's (%08x): using %d", dp->win, d0w, k);
			if (k < 0)
				return -EBUSY;
			dp->win = k;
		}
	}
	if (rl(dp->d2, DECON_GLOBAL_CONTROL) & DECON_RUN_STATUS) {
		dp_log(dp, "DECON2 was left running: stopping it");
		rmw(dp->d2, DECON_GLOBAL_CONTROL, DECON_EN | DECON_EN_F, 0);
		rmw(dp->d2, DECON_SHADOW_REG_UPDATE_REQ, 0, SHADOW_GLOBAL);
		decon2_wait_idle(dp);
	}
	decon2_reset(dp);

	rmw(dp->sysreg_dpu, 0x10, 0, BIT(0));		/* TE_QACTIVE_PLL_EN */
	rmw(dp->d2, DECON_CLOCK_CONTROL_0, 0x11011101, 0);
	{
		/* SRAM1..3 (bits 16/20/24) not taken by DECON0; 12K wants all three */
		u32 d0s = rl(dp->d0, DECON_SRAM_SHARE_ENABLE);
		u32 free = (BIT(16) | BIT(20) | BIT(24)) & ~d0s;

		sram = free;
		if (!sram) {
			dp_log(dp, "no SRAM left for DECON2 (DECON0 %08x)", d0s);
			return -EBUSY;
		}
		if (free != (BIT(16) | BIT(20) | BIT(24)))
			dp_log(dp, "DECON0 shares SRAM %08x: DECON2 gets %08x", d0s, sram);
	}
	wl(dp->d2, DECON_SRAM_SHARE_ENABLE, sram);
	rmw(dp->d2, DECON_GLOBAL_CONTROL, DECON_OPERATION_MODE_CMD | DECON_TEN_BPC |
	    DECON_EN_F, 0);
	wl(dp->d2, DECON_BLENDER_BG_IMAGE_SIZE_0, h << 16 | w);
	wl(dp->d2, DECON_SCALED_SIZE_CONTROL_0, h << 16 | w);
	rmw(dp->d2, DECON_HW_SW_TRIG_CONTROL, 0, BIT(0));
	wf(dp->d2, DECON_OUTFIFO_DATA_ORDER, GENMASK(6, 4), pixel_order);
	rmw(dp->d2, DECON_DATA_PATH_CONTROL_2, GENMASK(25, 24) | GENMASK(7, 0), 0x08);
	wl(dp->d2, DECON_OUTFIFO_SIZE_CONTROL_0, h << 16 | w);
	wf(dp->d2, DECON_OUTFIFO_TH_CONTROL_0, GENMASK(2, 0), 5);
	wl(dp->d2, DECON_INTERRUPT_PENDING, ~0);
	wl(dp->d2, DECON_EXTRA_INTERRUPT_PENDING, ~0);
	decon0_shared(dp, true);
	dp->d2_irq_armed = true;
	rmw(dp->d2, DECON_INTERRUPT_ENABLE, 0x3111, 0x3011);
	wl(dp->d2, DECON_EXTRA_INTERRUPT_ENABLE, 0x110);

	window_config(dp, w, h);
	dpp_init(dp);
	dpp_config(dp, w, h, pitch, addr);
	dp->scanout = addr;
	dp->pitch = pitch;

	/* decon_reg_start */
	rmw(dp->d2, DECON_SHADOW_REG_UPDATE_REQ, 0, 0x3f);
	ret = lh_power(dp, true);
	rmw(dp->d2, DECON_GLOBAL_CONTROL, 0, DECON_EN | DECON_EN_F);
	rmw(dp->d2, DECON_SHADOW_REG_UPDATE_REQ, 0, SHADOW_GLOBAL);
	if (readl_poll_timeout(dp->d2 + DECON_GLOBAL_CONTROL, v, v & DECON_RUN_STATUS,
			       10, 20000)) {
		dp_log(dp, "DECON2 did not reach RUN (%08x)", v);
		ret = -ETIMEDOUT;
	}
	msleep(50);
	dp_log(dp, "DECON2: global %08x frames %u shadow_req %08x occ %08x/%08x/%08x int %08x/%08x",
	       rl(dp->d2, DECON_GLOBAL_CONTROL), rl(dp->d2, DECON_FRAME_COUNT),
	       rl(dp->d2, DECON_SHADOW_REG_UPDATE_REQ),
	       rl(dp->d2, DECON_RESOURCE_OCCUPANCY(0)), rl(dp->d2, DECON_RESOURCE_OCCUPANCY(1)),
	       rl(dp->d2, DECON_RESOURCE_OCCUPANCY(2)), rl(dp->d2, DECON_INTERRUPT_PENDING),
	       rl(dp->d2, DECON_EXTRA_INTERRUPT_PENDING));
	dp_log(dp, "IDMA %s: in_con %08x base %08x (active %08x) irq %08x cfg_err %08x | DP st0 %08x",
	       dp->chan->name, rl(dp->idma, IDMA_IN_CON), rl(dp->idma, IDMA_IN_BASE_ADDR_Y),
	       rl(dp->idma, IDMA_SHADOW + IDMA_IN_BASE_ADDR_Y), rl(dp->idma, IDMA_IRQ),
	       rl(dp->idma, IDMA_CFG_ERR_STATE), rl(dp->link, SST1_INTERRUPT_STATUS_SET0));
	dp->streaming = !ret;
	if (ret)
		s9p_dp_decon_off_force(dp);
	return ret;
}

void s9p_dp_decon_flip(struct s9p_dp *dp, dma_addr_t addr, u32 pitch)
{
	u32 v;

	readl_poll_timeout(dp->d2 + DECON_SHADOW_REG_UPDATE_REQ, v,
			   !(v & BIT(dp->win)), 100, 50000);
	if (pitch != dp->pitch) {
		wl(dp->idma, IDMA_SRC_SIZE, dp->cur_mode.vdisplay << 16 | (pitch / 4));
		dp->pitch = pitch;
	}
	wl(dp->idma, IDMA_IN_BASE_ADDR_Y, lower_32_bits(addr));
	wl(dp->idma, IDMA_IN_BASE_ADDR_C, lower_32_bits(addr));
	wmb();
	rmw(dp->d2, DECON_SHADOW_REG_UPDATE_REQ, 0, BIT(dp->win) | SHADOW_GLOBAL);
	rmw(dp->d2, DECON_GLOBAL_CONTROL, 0, DECON_EN | DECON_EN_F);
	dp->scanout = addr;
}

void s9p_dp_decon_vblank(struct s9p_dp *dp, bool on)
{
	wl(dp->d2, DECON_INTERRUPT_PENDING, DECON_FRAME_START_INT);
	rmw(dp->d2, DECON_INTERRUPT_ENABLE, DECON_FRAME_START_INT | BIT(0),
	    on ? DECON_FRAME_START_INT | BIT(0) : BIT(0));
}

/* the window's shadow request is clear: the last flip has been latched */
bool s9p_dp_decon_latched(struct s9p_dp *dp)
{
	return !(rl(dp->d2, DECON_SHADOW_REG_UPDATE_REQ) & BIT(dp->win));
}

bool s9p_dp_decon_irq(struct s9p_dp *dp)
{
	u32 p = rl(dp->d2, DECON_INTERRUPT_PENDING);

	if (p & DECON_EXTRA_INT) {
		u32 e = rl(dp->d2, DECON_EXTRA_INTERRUPT_PENDING);

		wl(dp->d2, DECON_EXTRA_INTERRUPT_PENDING, e);
		dev_warn_ratelimited(dp->dev, "DECON2 extra irq %08x (occ %08x/%08x/%08x)\n",
				     e, rl(dp->d2, DECON_RESOURCE_OCCUPANCY(0)),
				     rl(dp->d2, DECON_RESOURCE_OCCUPANCY(1)),
				     rl(dp->d2, DECON_RESOURCE_OCCUPANCY(2)));
	}
	wl(dp->d2, DECON_INTERRUPT_PENDING, p);
	return p & DECON_FRAME_START_INT;
}

void s9p_dp_decon_off(struct s9p_dp *dp)
{
	if (!dp->streaming)
		return;
	s9p_dp_decon_off_force(dp);
}

void s9p_dp_decon_off_force(struct s9p_dp *dp)
{
	rmw(dp->d2, DECON_INTERRUPT_ENABLE, 0x3111, 0);
	wl(dp->d2, DECON_INTERRUPT_PENDING, ~0);
	dp->d2_irq_armed = false;
	if (dp->irq_fs > 0)
		synchronize_irq(dp->irq_fs);
	rmw(dp->link, SST1_INTERRUPT_MASK_SET0, MAPI_FIFO_UNDER_FLOW, 0);
	rmw(dp->d2, DECON_GLOBAL_CONTROL, DECON_EN | DECON_EN_F, 0);
	rmw(dp->d2, DECON_SHADOW_REG_UPDATE_REQ, 0, SHADOW_GLOBAL);
	lh_power(dp, false);
	if (!decon2_wait_idle(dp))
		decon2_reset(dp);
	else
		dp_log(dp, "DECON2 did not stop (%08x)", rl(dp->d2, DECON_GLOBAL_CONTROL));
	/* dpp_reg_deinit(reset = false) */
	rmw(dp->idma, IDMA_IRQ, 0, 0x01fb0000);
	rmw(dp->dpp, DPP_IRQ, 0, 0x00210000);
	rmw(dp->idma, IDMA_IRQ, 0, 0x3f6);
	rmw(dp->dpp, DPP_IRQ, 0, 0x42);
	decon0_shared(dp, false);
	dp->streaming = false;
	dp->d2_irq_armed = false;
	dp_log(dp, "DECON2 stopped");
}

/* ------------------------------------------------------------------ */

void s9p_dp_dump(struct s9p_dp *dp, struct seq_file *s)
{
	static const struct { u32 off; const char *name; } lr[] = {
		{ 0x0000, "version" }, { 0x0008, "clk_control" }, { 0x000c, "bandwidth" },
		{ 0x0010, "lane_count" }, { 0x0014, "sw_func_en" }, { 0x0018, "common_func_en" },
		{ 0x001c, "sst1_func_en" }, { 0x0028, "hpd_control" }, { 0x002c, "pll_lock" },
		{ 0x0108, "irq_common" }, { 0x0204, "debug_lh_pch" }, { 0x1000, "aux_control" },
		{ 0x1018, "aux_monitor_1" }, { 0x101c, "aux_monitor_2" }, { 0x3000, "pcs_control" },
		{ 0x3004, "pcs_lane" }, { 0x5000, "sst1_main" }, { 0x5024, "sst1_int0" },
		{ 0x5054, "mvid_monitor" }, { 0x5400, "video_control" }, { 0x5404, "video_enable" },
		{ 0x5408, "master_tg" }, { 0x5414, "htotal" }, { 0x5418, "vtotal" },
		{ 0x5424, "hactive" }, { 0x5430, "vactive" }, { 0x5450, "bist" },
	};
	int i;

	seq_printf(s, "pmu 704 %08x 72c %08x dpu %08x | usbcon link %08x clkrst %08x pma %08x\n",
		   rl(dp->pmu, PMU_USB3DP_PHY_CTRL), rl(dp->pmu, PMU_USB20_PHY_CTRL),
		   rl(dp->pmu, PMU_DPU_STATUS), rl(dp->usbcon, USBCON_LINK_CTRL),
		   rl(dp->usbcon, USBCON_CLKRST), rl(dp->usbcon, USBCON_COMBO_PMA_CTRL));
	seq_printf(s, "qch dp_link %08x dpu %08x usbdpphy %08x\n",
		   rl(dp->cmu_fsys0, 0x3014), rl(dp->cmu_dpu, 0x3020), rl(dp->cmu_fsys0, 0x3054));
	if (!(rl(dp->pmu, PMU_USB3DP_PHY_CTRL) & 1)) {
		seq_puts(s, "PHY isolated: not reading the DP block\n");
		return;
	}
	for (i = 0; i < ARRAY_SIZE(lr); i++)
		seq_printf(s, "dp %04x %-16s %08x\n", lr[i].off, lr[i].name,
			   rl(dp->link, lr[i].off));
	seq_printf(s, "phy REG_0 %02x REG_1 %02x REG_3 %02x REG_4 %02x REG2C %02x REG2D %02x\n",
		   rl(dp->pma, DP_REG_0), rl(dp->pma, DP_REG_1), rl(dp->pma, DP_REG_3),
		   rl(dp->pma, DP_REG_4), rl(dp->pma, CMN_REG2C), rl(dp->pma, CMN_REG2D));
	if (dp->streaming) {
		seq_printf(s, "decon2 global %08x frames %u req %08x int %08x path %08x/%08x/%08x\n",
			   rl(dp->d2, DECON_GLOBAL_CONTROL), rl(dp->d2, DECON_FRAME_COUNT),
			   rl(dp->d2, DECON_SHADOW_REG_UPDATE_REQ),
			   rl(dp->d2, DECON_INTERRUPT_PENDING),
			   rl(dp->d2, DECON_DATA_PATH_CONTROL_0),
			   rl(dp->d2, DECON_DATA_PATH_CONTROL_1),
			   rl(dp->d2, DECON_DATA_PATH_CONTROL_2));
		seq_printf(s, "idma %s in_con %08x base %08x active %08x irq %08x err %08x\n",
			   dp->chan->name, rl(dp->idma, IDMA_IN_CON),
			   rl(dp->idma, IDMA_IN_BASE_ADDR_Y),
			   rl(dp->idma, IDMA_SHADOW + IDMA_IN_BASE_ADDR_Y),
			   rl(dp->idma, IDMA_IRQ), rl(dp->idma, IDMA_CFG_ERR_STATE));
	}
}

/* ------------------------------------------------------------------ */
/* SST1 audio (vendor displayport_audio_config / displayport_reg.c)     */

#define DP_SST1_FUNC_EN			0x001c
#define  SST1_AUDIO_FUNC_EN		BIT(1)
#define  SST1_AUDIO_FIFO_FUNC_EN	BIT(2)
#define DP_SST1_MAIN_CONTROL		0x5000
#define  MAUD_MODE			BIT(10)
#define DP_SST1_MAUD_MASTER		0x505c
#define DP_SST1_NAUD_MASTER		0x5060
#define DP_SST1_MAUD_SFR		0x5064
#define DP_SST1_NAUD_SFR		0x5068
#define DP_AUDIO_CONTROL		0x5800
#define  AUD_DMA_IF_LTNCY_TRG_MODE	BIT(26)
#define  AUD_DMA_IF_MODE_CONFIG		BIT(25)
#define  DMA_BURST_SEL			GENMASK(20, 18)
#define  AUDIO_BIT_MAPPING_TYPE		GENMASK(17, 16)
#define  PCM_SIZE			GENMASK(14, 13)
#define  AUDIO_CH_STATUS_SAME		BIT(5)
#define DP_AUDIO_ENABLE			0x5804
#define  AUDIO_EN			BIT(0)
#define DP_AUDIO_MASTER_TIMING_GEN	0x5808
#define  AUDIO_MASTER_TIME_GEN		BIT(0)
#define DP_AUDIO_DMA_REQ_LATENCY	0x580c
#define  AUD_DMA_FORCE_REQ_VAL		BIT(17)
#define  AUD_DMA_FORCE_REQ_SEL		BIT(16)
#define DP_AUDIO_BIST_CH_STATUS0	0x5834
#define DP_AUDIO_BIST_CH_STATUS1	0x5838
#define DP_AUDIO_BUFFER_CONTROL		0x583c
#define  MASTER_AUDIO_CHANNEL_COUNT	GENMASK(16, 12)
#define  MASTER_AUDIO_BUFFER_EMPTY_INT	BIT(1)
#define DP_AUDIO_CH_1_4_REMAP		0x5840
#define DP_AUDIO_CH_5_8_REMAP		0x5844
#define DP_AUDIO_CH12_STATUS0		0x5860
#define DP_AUDIO_CH12_STATUS1		0x5864
#define  CS_CLK_ACCUR			GENMASK(29, 28)
#define  CS_FS_FREQ			GENMASK(27, 24)
#define  CS_CH_NUM			GENMASK(23, 20)
#define  CS_SOURCE_NUM			GENMASK(19, 16)
#define  CS_WORD_LENGTH			GENMASK(3, 1)
#define  CS_WORD_MAX			BIT(0)
#define DP_INFOFRAME_UPDATE		0x5c04
#define DP_INFOFRAME_SEND		0x5c08
#define  AUDIO_INFO_BIT			BIT(3)
#define DP_INFOFRAME_AUDIO0		0x5c50
#define DP_INFOFRAME_AUDIO1		0x5c54
#define DP_INFOFRAME_AUDIO2		0x5c58

/* vendor tables: rows 1.62 / 2.7 / 5.4 Gbps, columns 32k..192k */
static const u32 maud_async[3][7] = {
	{ 3314, 4567, 4971, 9134, 9942, 18269, 19884 },
	{ 1988, 2740, 2983, 5481, 5695, 10961, 11930 },
	{  994, 1370, 1491, 2740, 2983,  5481,  5965 },
};
static const u32 aud_fs[7] = { 32000, 44100, 48000, 88200, 96000, 176000, 192000 };
static const u32 naud_master[3] = { 81000000, 135000000, 270000000 };
/* IEC 60958 sampling frequency codes, same order */
static const u8 cs_fs[7] = { 0x03, 0x00, 0x02, 0x08, 0x0a, 0x0c, 0x0e };

void s9p_dp_audio_dma_req(struct s9p_dp *dp, bool run)
{
	if (run) {
		rmw(dp->link, DP_AUDIO_DMA_REQ_LATENCY, AUD_DMA_FORCE_REQ_SEL, 0);
	} else {
		rmw(dp->link, DP_AUDIO_DMA_REQ_LATENCY, AUD_DMA_FORCE_REQ_VAL, 0);
		rmw(dp->link, DP_AUDIO_DMA_REQ_LATENCY, 0, AUD_DMA_FORCE_REQ_SEL);
	}
}

int s9p_dp_audio_on(struct s9p_dp *dp, unsigned int rate, unsigned int channels,
		    unsigned int bits, unsigned int phys_bits)
{
	int fs, row, burst = phys_bits * channels / 32;
	u8 bitcode = bits == 24 ? 2 : bits == 20 ? 1 : 0;
	u32 cs_len = bits == 24 ? 0x05 : bits == 20 ? 0x05 : 0x01;

	if (!dp->trained || !(rl(dp->pmu, PMU_USB3DP_PHY_CTRL) & 1))
		return -ENOLINK;
	for (fs = 0; fs < ARRAY_SIZE(aud_fs); fs++)
		if (aud_fs[fs] == rate || (rate == 176400 && fs == 5))
			break;
	if (fs == ARRAY_SIZE(aud_fs) || burst < 1 || burst > 8)
		return -EINVAL;
	row = dp->rate == 0x06 ? 0 : dp->rate == 0x0a ? 1 : 2;

	s9p_dp_audio_dma_req(dp, false);
	/* channel mapping: FL, FR, C, SW, RL, RR */
	wl(dp->link, DP_AUDIO_CH_1_4_REMAP, 1 | 2 << 8 | 4 << 16 | 3 << 24);
	wl(dp->link, DP_AUDIO_CH_5_8_REMAP, 5 | 6 << 8 | 7 << 16 | 8 << 24);
	/* asynchronous Maud/Naud */
	rmw(dp->link, DP_SST1_MAIN_CONTROL, MAUD_MODE, 0);
	wl(dp->link, DP_SST1_MAUD_SFR, maud_async[row][fs]);
	wl(dp->link, DP_SST1_NAUD_SFR, 32768);
	rmw(dp->link, DP_SST1_FUNC_EN, 0, SST1_AUDIO_FUNC_EN);
	rmw(dp->link, DP_AUDIO_CONTROL, AUD_DMA_IF_LTNCY_TRG_MODE, AUD_DMA_IF_MODE_CONFIG);
	wf(dp->link, DP_AUDIO_CONTROL, DMA_BURST_SEL, burst - 1);
	/* 32-bit containers: normal; 16-bit: two samples per word (PACKED_MODE2) */
	wf(dp->link, DP_AUDIO_CONTROL, AUDIO_BIT_MAPPING_TYPE, phys_bits == 32 ? 0 : 2);
	wf(dp->link, DP_AUDIO_CONTROL, PCM_SIZE, bitcode);
	wf(dp->link, DP_AUDIO_BUFFER_CONTROL, MASTER_AUDIO_CHANNEL_COUNT, channels - 1);
	rmw(dp->link, DP_SST1_FUNC_EN, 0, SST1_AUDIO_FIFO_FUNC_EN);
	rmw(dp->link, DP_AUDIO_CONTROL, 0, AUDIO_CH_STATUS_SAME);
	wl(dp->link, DP_SST1_MAUD_MASTER, aud_fs[fs]);
	wl(dp->link, DP_SST1_NAUD_MASTER, naud_master[row]);
	rmw(dp->link, DP_AUDIO_ENABLE, 0, AUDIO_EN);

	/* audio InfoFrame: CC = channels - 1, the rest "refer to stream" */
	wl(dp->link, DP_INFOFRAME_AUDIO0, channels - 1);
	wl(dp->link, DP_INFOFRAME_AUDIO1, 0);
	wl(dp->link, DP_INFOFRAME_AUDIO2, 0);
	rmw(dp->link, DP_INFOFRAME_UPDATE, 0, AUDIO_INFO_BIT);
	rmw(dp->link, DP_INFOFRAME_SEND, 0, AUDIO_INFO_BIT);

	/* IEC 60958 channel status, BIST and channel 1/2 copies */
	wf(dp->link, DP_AUDIO_BIST_CH_STATUS0, CS_CH_NUM, channels);
	wf(dp->link, DP_AUDIO_BIST_CH_STATUS0, CS_SOURCE_NUM, channels);
	wf(dp->link, DP_AUDIO_CH12_STATUS0, CS_CH_NUM, channels);
	wf(dp->link, DP_AUDIO_CH12_STATUS0, CS_SOURCE_NUM, channels);
	wf(dp->link, DP_AUDIO_BIST_CH_STATUS1, CS_WORD_MAX, bits == 24);
	wf(dp->link, DP_AUDIO_BIST_CH_STATUS1, CS_WORD_LENGTH, cs_len);
	wf(dp->link, DP_AUDIO_CH12_STATUS1, CS_WORD_MAX, bits == 24);
	wf(dp->link, DP_AUDIO_CH12_STATUS1, CS_WORD_LENGTH, cs_len);
	wf(dp->link, DP_AUDIO_BIST_CH_STATUS0, CS_FS_FREQ, cs_fs[fs]);
	wf(dp->link, DP_AUDIO_CH12_STATUS0, CS_FS_FREQ, cs_fs[fs]);
	wf(dp->link, DP_AUDIO_BIST_CH_STATUS0, CS_CLK_ACCUR, 3);
	wf(dp->link, DP_AUDIO_CH12_STATUS0, CS_CLK_ACCUR, 3);

	rmw(dp->link, DP_AUDIO_MASTER_TIMING_GEN, 0, AUDIO_MASTER_TIME_GEN);
	dp_log(dp, "audio on: %u Hz %u ch %u/%u bit, burst %d, M %u (row %d), ctl %08x",
	       rate, channels, bits, phys_bits, burst, maud_async[row][fs], row,
	       rl(dp->link, DP_AUDIO_CONTROL));
	return 0;
}

void s9p_dp_audio_off(struct s9p_dp *dp)
{
	int i;

	if (!(rl(dp->pmu, PMU_USB3DP_PHY_CTRL) & 1) || !(rl(dp->link, DP_AUDIO_ENABLE) & AUDIO_EN))
		return;
	s9p_dp_audio_dma_req(dp, false);
	udelay(200);
	rmw(dp->link, DP_AUDIO_ENABLE, AUDIO_EN, 0);
	rmw(dp->link, DP_SST1_FUNC_EN, SST1_AUDIO_FIFO_FUNC_EN, 0);
	wf(dp->link, DP_AUDIO_BUFFER_CONTROL, MASTER_AUDIO_CHANNEL_COUNT, 0);
	rmw(dp->link, DP_AUDIO_MASTER_TIMING_GEN, 0, AUDIO_MASTER_TIME_GEN);
	for (i = 0; i < 200 && !(rl(dp->link, DP_AUDIO_BUFFER_CONTROL) &
				   MASTER_AUDIO_BUFFER_EMPTY_INT); i++)
		usleep_range(100, 200);
	rmw(dp->link, DP_AUDIO_MASTER_TIMING_GEN, AUDIO_MASTER_TIME_GEN, 0);
	rmw(dp->link, DP_SST1_FUNC_EN, SST1_AUDIO_FUNC_EN, 0);
	dp_log(dp, "audio off (buffer %s)", i < 200 ? "drained" : "NOT drained");
}
