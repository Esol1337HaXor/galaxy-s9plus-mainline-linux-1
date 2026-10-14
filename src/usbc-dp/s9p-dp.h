/* SPDX-License-Identifier: GPL-2.0 */
#ifndef S9P_DP_H
#define S9P_DP_H

#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <drm/display/drm_dp_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_simple_kms_helper.h>

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>

#include "s9p-usbc.h"

/* claim one line of a GPIO device (gpiod_request is not exported) */
static inline struct gpio_desc *s9p_grab_gpio(struct gpio_device *gdev,
					      unsigned int hwnum,
					      const char *label,
					      enum gpiod_flags flags)
{
	struct gpio_chip *gc = gpio_device_get_chip(gdev);

	if (!gc)
		return ERR_PTR(-ENODEV);
	return gpiochip_request_own_desc(gc, hwnum, label, GPIO_LOOKUP_FLAGS_DEFAULT, flags);
}

struct of_changeset;
struct gpio_desc;
struct gpio_device;
struct drm_edid;

/* which DPU DMA channel feeds DECON2 */
struct s9p_dp_chan {
	const char *name;
	u32 idma;		/* IDMA block */
	u32 dpp;		/* DPP block */
	u8 chmap;		/* DECON CHMAP code */
	bool scaler;		/* has the H/V scaler (needs 1:1 coefficients) */
};

struct s9p_dp {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector conn;
	struct drm_dp_aux aux;
	struct device *dev;

	/* MMIO */
	void __iomem *link;	/* DP controller 0x11090000 */
	void __iomem *pma;	/* combo PHY PMA 0x110a0000 */
	void __iomem *pcs;	/* combo PHY PCS 0x110b0000 */
	void __iomem *usbcon;	/* USB DRD control 0x11100000 */
	void __iomem *pmu;	/* PMU 0x14060000 */
	void __iomem *d0;	/* DECON0 0x16030000 (shared regs only) */
	void __iomem *d2;	/* DECON2 0x16050000 */
	void __iomem *idma;	/* chosen IDMA channel */
	void __iomem *dpp;	/* chosen DPP */
	void __iomem *dma;	/* DPU_DMA common 0x16070000 */
	void __iomem *cmu_dpu;	/* 0x16000000 */
	void __iomem *sysreg_dpu; /* 0x16011000 */
	void __iomem *cmu_fsys0; /* 0x11000000 */
	const struct s9p_dp_chan *chan;
	unsigned int win;	/* blender window of DECON2 */

	/* board: SBU/AUX switch on the PCAL6524 */
	struct of_changeset *sbu_cs;
	struct gpio_device *exp_gdev;
	struct gpio_desc *sbu_sel;
	struct gpio_desc *aux_oe;
	struct of_changeset *dev_cs;	/* our own platform node */

	/* Type-C state from s9p-usbc */
	struct notifier_block nb;
	struct s9p_usbc_dp_state st;
	struct work_struct hpd_work;

	struct mutex lock;	/* everything below */
	bool aux_on;		/* SBU switch closed */
	bool phy_ready;		/* PMA + DP PHY + controller up, AUX usable */
	bool connected;		/* sink answered DPCD */
	bool trained;
	bool streaming;
	bool bist;
	bool blanked;		/* CRTC off, link kept up with a black frame */
	u8 pin;			/* S9P_DP_PIN_x */
	bool sw_sel;		/* vendor dp_sw_sel: CC2 (inverted for pin B) */
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	u8 rate;		/* SYSTEM_MAIN_LINK_BANDWIDTH code: 0x06/0x0a/0x14 */
	u8 lanes;
	bool enhanced, tps3;
	u8 swing[4], pre[4];
	const struct drm_edid *edid;
	struct drm_display_mode cur_mode;
	u32 d0_f0_saved;
	bool d0_f0_valid;
	dma_addr_t scanout;
	u32 pitch;
	bool phy_held;		/* told the USB PHY driver (if it can be told) */
	bool d2_irq_armed;	/* DECON2 interrupts enabled: its APB is open */
	bool irq_ok;		/* the frame start IRQ is requested */
	struct delayed_work guard_work;

	/* vblank / page flip */
	int irq_fs;		/* DECON2 frame start, SPI 194 */
	struct drm_pending_vblank_event *event;
	bool vblank_on;

	struct dentry *dbg;
	spinlock_t log_lock;
	char log[8192];
	size_t log_len;
};

#define to_s9p_dp(d) container_of(d, struct s9p_dp, drm)

__printf(2, 3) void dp_log(struct s9p_dp *dp, const char *fmt, ...);

/* s9p-dp-board.c */
int s9p_dp_board_init(struct s9p_dp *dp);
void s9p_dp_board_aux(struct s9p_dp *dp, bool on, u8 orientation);
void s9p_dp_board_exit(struct s9p_dp *dp);

/* s9p-dp-hw.c: PHY, controller, AUX, link training, video, DECON2 */
int s9p_dp_hw_map(struct s9p_dp *dp);
void s9p_dp_hw_unmap(struct s9p_dp *dp);
bool s9p_dp_phy_ok(struct s9p_dp *dp);
int s9p_dp_hw_recover(struct s9p_dp *dp);
int s9p_dp_retrain(struct s9p_dp *dp);
int s9p_dp_hw_up(struct s9p_dp *dp);		/* PMA + DP PHY + controller, AUX on */
void s9p_dp_hw_down(struct s9p_dp *dp);
int s9p_dp_aux_register(struct s9p_dp *dp);
int s9p_dp_probe_sink(struct s9p_dp *dp);	/* DPCD caps, sink count */
int s9p_dp_train(struct s9p_dp *dp, const struct drm_display_mode *mode);
int s9p_dp_video_on(struct s9p_dp *dp, const struct drm_display_mode *mode, bool bist);
void s9p_dp_video_off(struct s9p_dp *dp);
int s9p_dp_decon_on(struct s9p_dp *dp, const struct drm_display_mode *mode,
		    dma_addr_t addr, u32 pitch);
void s9p_dp_decon_flip(struct s9p_dp *dp, dma_addr_t addr, u32 pitch);
void s9p_dp_decon_off(struct s9p_dp *dp);
void s9p_dp_decon_off_force(struct s9p_dp *dp);
bool s9p_dp_decon_latched(struct s9p_dp *dp);
void s9p_dp_decon_vblank(struct s9p_dp *dp, bool on);
bool s9p_dp_decon_irq(struct s9p_dp *dp);	/* true if FRAME_START */
void s9p_dp_dump(struct s9p_dp *dp, struct seq_file *s);
bool s9p_dp_mode_fits(struct s9p_dp *dp, int clock_khz);
int s9p_dp_audio_on(struct s9p_dp *dp, unsigned int rate, unsigned int channels,
		    unsigned int bits, unsigned int phys_bits);
void s9p_dp_audio_off(struct s9p_dp *dp);
void s9p_dp_audio_dma_req(struct s9p_dp *dp, bool run);

#endif
