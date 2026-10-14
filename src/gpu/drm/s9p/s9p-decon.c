// SPDX-License-Identifier: GPL-2.0
/*
 * DECON scanout for the Galaxy S9+ (star2lte, Exynos 9810) under mainline.
 *
 * Samsung's bootloader leaves DECON0 (0x16030000) scanning out the boot
 * framebuffer at 0xcc000000 through DSIM0, with the panel fully configured
 * and running. This driver adopts that pipeline unchanged and owns exactly
 * two things afterwards:
 *
 *  1. the IDMA scanout base address of the live window, and
 *  2. the update/trigger handshake that makes a new base address visible.
 *
 * The stock panel runs MIPI command mode with hardware (TE) trigger
 * (stock DT: psr_mode=2, trig_mode=0, dsi_mode=0). In that mode a new
 * frame is transferred only after the shadow update has been requested
 * AND the hardware trigger has been unmasked; the next TE edge starts
 * the transfer. The handshake follows Samsung's exynos9 CAL model
 * (cal_9845 decon_reg.c: decon_reg_update_req_and_unmask /
 * wait_update_done_and_mask):
 *
 *      wait until shadow request idle
 *      write IDMA_IN_BASE_ADDR_Y(ch)
 *      wmb
 *      SHADOW_REG_UPDATE_REQ = BIT(31) | BIT(win)   (global + window)
 *      RMW HW_SW_TRIG_CONTROL: set HW_TRIG_EN, clear HW_TRIG_MASK_DECON
 *      next TE edge latches and transfers; FRAME_START IRQ completes flips
 *
 * Everything else -- window geometry, format, size, channel map, DSC,
 * OUTFIFO, DSIM, panel, clocks, power -- is inherited from sboot and
 * never written. The trigger control register is only ever modified
 * read-modify-write: uniLoader's proven unlock value 0x1281 sets bits
 * (12|9|7|0) that this generation's header does not fully name, so the
 * unknown bits must survive every write.
 *
 * The live (window, channel) pair is identified by consistency, never by
 * one enable bit: window enabled && channel == CHMAP(win) &&
 * IDMA base == 0xcc000000. The driver name is "exynos" so Mesa pairs it
 * with Panfrost through kmsro.
 */
#include <linux/aperture.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_module.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>
#include <linux/iopoll.h>

#define DRIVER_NAME "exynos"

/* The framebuffer sboot scans out (its boot logo), from the port's DT. */
#define S9P_SBOOT_FB_BASE	0xcc000000

/* DECON0, 0x16030000 */
#define DECON_INTERRUPT_ENABLE		0x0040
#define  DECON_FRAME_START_INT_EN	BIT(12)
#define  DECON_INT_EN			BIT(0)
#define DECON_INTERRUPT_PENDING		0x004c
#define  DECON_FRAME_START_INT_PEND	BIT(12)
#define  DECON_FRAME_DONE_INT_PEND	BIT(13)
#define  DECON_EXTRA_INT_PEND		BIT(4)
#define DECON_EXTRA_INTERRUPT_PENDING	0x0050
#define DECON_SHADOW_REG_UPDATE_REQ	0x0060
#define  DECON_SHADOW_REQ_GLOBAL	BIT(31)
#define  DECON_SHADOW_REQ_WIN(win)	BIT(win)
#define DECON_HW_SW_TRIG_CONTROL	0x0070
#define  DECON_HW_TRIG_EN		BIT(0)
#define  DECON_HW_TRIG_MASK_DECON	BIT(4)
#define DECON_DATA_PATH_CONTROL_0	0x0214
#define  DECON_WIN_EN(win)		BIT(4 * (win) + 0)
#define DECON_DATA_PATH_CONTROL_1	0x0218
#define DECON_MAX_WIN			6

/* Blender window registers, per window inside the DECON block */
#define WIN_START_POSITION(win)		(0x1008 + (win) * 0x30)
#define WIN_END_POSITION(win)		(0x100c + (win) * 0x30)

/* DPU_DMA channels, 0x16071000 + 0x1000 * channel.
 * Each channel has TWO banks: the normal registers (0x000.., the queued
 * state a flip writes) and a hardware shadow copy at +0x800 mirroring the
 * ACTIVE scanout state (the vendor dumps it as "SHADOW SFR").
 *
 * NOTE (measured live on this device, bootfb build, display active):
 * the fetch channel behind the sboot framebuffer is channel 0 -- exactly
 * what the stock DT's decon_f node declares with default_idma = <0x0>.
 * Channel 0 reads IN_CON=02000000 (format 0 = ARGB8888, IC_MAX 64),
 * SRC/IMG_SIZE=0b9005a0 (2960x1440) and BASE=0xcc000000 in BOTH banks;
 * channels 1..5 -- including the one the window's CHMAP nibble (5) would
 * suggest -- are all zero. The CHMAP value is a DMA *type* (G0/G1/VG0/
 * VG1/VGF0/VGF1), not the hardware channel index; sboot bypassed that
 * mapping and wired the window straight to channel 0. The driver must
 * therefore INHERIT the channel configuration untouched (programming
 * base=0 into the live channel kills the scanout) and only ever write
 * the base address for a flip. */
#define IDMA_SHD_BANK			0x800
#define IDMA_IN_CON			0x0008
#define  IDMA_IMG_FORMAT_XRGB8888	(4u << 11)
#define  IDMA_IMG_FORMAT_MASK		(0x1fu << 11)
#define IDMA_SRC_SIZE			0x0010
#define IDMA_IMG_SIZE			0x0018
#define IDMA_IN_BASE_ADDR_Y		0x0040

/*
 * DSIM0 (0x16080000), the MIPI host the bootloader configured and left
 * running. The driver never reconfigures it; it only pushes single DCS
 * short writes through the packet FIFO, which is what turns the panel's
 * emission off and on. Layout from the vendor register header
 * (dpu_9810/regs-dsim.h).
 */
#define DSIM_PHYS			0x16080000
#define DSIM_INTSRC			0x0050
#define  DSIM_INTSRC_PH_FIFO_EMPTY	BIT(28)
#define DSIM_PKTHDR			0x0058
#define  DSIM_PKTHDR_ID(x)		((x) << 0)
#define  DSIM_PKTHDR_DATA0(x)		((x) << 8)
#define  DSIM_PKTHDR_DATA1(x)		((x) << 16)
#define MIPI_DCS_SHORT_WRITE		0x05
#define MIPI_DCS_ENTER_SLEEP_MODE	0x10
#define MIPI_DCS_EXIT_SLEEP_MODE	0x11
#define MIPI_DCS_SET_DISPLAY_OFF	0x28
#define MIPI_DCS_SET_DISPLAY_ON		0x29

/*
 * DCS timing from the MIPI DCS specification, which every Samsung AMOLED
 * controller in this family follows: after enter_sleep_mode the panel needs
 * a frame before its supplies are down, and after exit_sleep_mode the
 * internal boost converter needs 120 ms before it can be told to display
 * again. Sending set_display_on too early leaves the panel dark.
 */
#define PANEL_SLEEP_DELAY_MS		120
#define PANEL_FRAME_DELAY_MS		20

/*
 * Sending the panel DCS display-off is the difference between a black
 * screen and a dark one: the pixels stop emitting instead of being told to
 * show black. Switchable because it is the one thing here that writes to a
 * block the bootloader owns.
 */
static bool s9p_panel_dcs = true;
module_param_named(panel_dcs, s9p_panel_dcs, bool, 0644);
MODULE_PARM_DESC(panel_dcs, "send the panel DCS display off/on when blanking");

/*
 * set_display_off alone stops the emission but leaves the panel controller,
 * its gate driver and its boost converter running -- perhaps 150 mW, and on
 * a panel with a damaged spot it is still visibly lit from behind.
 * enter_sleep_mode is what actually shuts those down.
 *
 * It is a separate switch from panel_dcs because it is the riskier half: a
 * panel that does not come back out of sleep stays dark until the next
 * reboot, and this port does not carry the panel's power-on sequence (sboot
 * owns it).
 */
static bool s9p_panel_sleep = true;
module_param_named(panel_sleep, s9p_panel_sleep, bool, 0644);
MODULE_PARM_DESC(panel_sleep, "also put the panel into sleep mode when blanking");

#define PANEL_W				1440
#define PANEL_H				2960

struct s9p_decon {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	void __iomem *decon;
	void __iomem *dpp;		/* DPU_DMA region 0x16070000; the IDMA
					 * channel registers sit at
					 * 0x16071000 + 0x1000 * channel */
	unsigned int win;		/* the window sboot left scanning */
	unsigned int chan;		/* DPU_DMA channel behind that window */
	unsigned int width, height;
	bool command_mode;		/* stock DT psr_mode=2: TE-driven */
	bool hw_trigger;		/* stock DT trig_mode=0 */
	bool enabled;
	u32 inherited_trig_ctrl;
	u32 inherited_path0;
	u32 inherited_path1;
	int irq_frame_start;
	/* one all-black frame, for display-off (see s9p_decon_blank) */
	void __iomem *dsim;		/* DSIM0, for DCS display off/on */
	void *black_cpu;
	dma_addr_t black_dma;
};

#define to_s9p_decon(x) container_of(x, struct s9p_decon, drm)

static void __iomem *s9p_chan_regs(const struct s9p_decon *priv,
				   unsigned int ch)
{
	/* per the stock DT: dpp@0x16021000 carries three ranges, the IDMA
	 * one starting at 0x16071000, one page per channel */
	return priv->dpp + 0x1000 + 0x1000 * ch;
}

static const u32 s9p_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* --------------------------------------------------------------- scanout */

static u32 s9p_shadow_mask(const struct s9p_decon *priv)
{
	/* the vendor pan path requests the window bit only
	 * (decon_reg_update_req_window); the global bit is for GLOBAL_CONTROL
	 * changes, which this driver never makes. */
	return DECON_SHADOW_REQ_WIN(priv->win);
}

/*
 * Poll interval for the shadow handshake.
 *
 * Before a flip the request is almost always already clear, so the first read
 * ends it. After a flip the wait is for the next TE edge, up to 16.7 ms away,
 * and a 10 us interval would put roughly 1600 timer wake-ups into every one
 * of the 60 frames a second. 250 us costs a quarter of a millisecond of
 * latency and turns that into 64.
 */
#define SHADOW_POLL_US		250
#define SHADOW_TIMEOUT_US	50000

static int s9p_wait_shadow_idle(struct s9p_decon *priv)
{
	u32 v;

	return readl_poll_timeout(priv->decon + DECON_SHADOW_REG_UPDATE_REQ,
				  v, (v & s9p_shadow_mask(priv)) == 0,
				  SHADOW_POLL_US, SHADOW_TIMEOUT_US);
}

static void s9p_hw_trigger_unmask(struct s9p_decon *priv)
{
	u32 v;

	if (!priv->command_mode || !priv->hw_trigger)
		return;

	/* read-modify-write: the inherited bits (incl. uniLoader's 0x1281
	 * bits 7/9/12) must survive; only the two proven bits change. */
	v = readl_relaxed(priv->decon + DECON_HW_SW_TRIG_CONTROL);
	v |= DECON_HW_TRIG_EN;
	v &= ~DECON_HW_TRIG_MASK_DECON;
	writel_relaxed(v, priv->decon + DECON_HW_SW_TRIG_CONTROL);
}

static void s9p_decon_program_address(struct s9p_decon *priv, dma_addr_t addr)
{
	void __iomem *ch = s9p_chan_regs(priv, priv->chan);

	/* packed XRGB8888: the vendor writes the primary Y/C pair only;
	 * Y2/C2 are extra bitplanes of 10-bit formats, not ping-pong. */
	writel_relaxed((u32)addr, ch + IDMA_IN_BASE_ADDR_Y);
	writel_relaxed((u32)addr, ch + IDMA_IN_BASE_ADDR_Y + 4);
	wmb();
	/*
	 * Read-modify-write, as the vendor's decon_reg_update_req_window():
	 * the global request DECON2's DisplayPort setup leaves pending in
	 * this register (DP_CONNECTION_CONTROL lives in DECON0's globally
	 * shadowed range) must survive until the next TE latches it.
	 */
	writel_relaxed(readl_relaxed(priv->decon + DECON_SHADOW_REG_UPDATE_REQ) |
		       s9p_shadow_mask(priv),
		       priv->decon + DECON_SHADOW_REG_UPDATE_REQ);
	s9p_hw_trigger_unmask(priv);
}

/*
 * The fetch channel is left EXACTLY as sboot configured it (format, sizes,
 * alpha, the lot): it is the live pipeline feeding the panel, and the boot
 * framebuffer node describes the same layout we hand out (XRGB8888,
 * 1440*4 stride, 1440x2960). A flip only ever moves the base address.
 */

static irqreturn_t s9p_decon_irq(int irq, void *data)
{
	struct s9p_decon *priv = data;
	static bool mapped[3];
	u32 pend = readl_relaxed(priv->decon + DECON_INTERRUPT_PENDING);
	int i;

	/* First fire per IRQ line proves the 182/183/188 mapping. */
	for (i = 0; i < 3; i++) {
		if (irq == priv->irq_frame_start + i && !mapped[i]) {
			mapped[i] = true;
			dev_info(priv->drm.dev,
				 "irq %d (index %d) pending %08x\n",
				 irq, i, pend);
		}
	}

	if (pend & DECON_FRAME_START_INT_PEND) {
		writel_relaxed(DECON_FRAME_START_INT_PEND,
			       priv->decon + DECON_INTERRUPT_PENDING);
		drm_crtc_handle_vblank(&priv->pipe.crtc);
	}
	if (pend & DECON_FRAME_DONE_INT_PEND)
		writel_relaxed(DECON_FRAME_DONE_INT_PEND,
			       priv->decon + DECON_INTERRUPT_PENDING);
	if (pend & DECON_EXTRA_INT_PEND) {
		u32 ext = readl_relaxed(priv->decon +
					DECON_EXTRA_INTERRUPT_PENDING);

		writel_relaxed(DECON_EXTRA_INT_PEND,
			       priv->decon + DECON_INTERRUPT_PENDING);
		writel_relaxed(ext, priv->decon + DECON_EXTRA_INTERRUPT_PENDING);
	}
	return IRQ_HANDLED;
}

static void s9p_frame_start_irq_set(struct s9p_decon *priv, bool on)
{
	u32 v = readl_relaxed(priv->decon + DECON_INTERRUPT_ENABLE);

	if (on)
		v |= DECON_INT_EN | DECON_FRAME_START_INT_EN;
	else
		v &= ~(DECON_INT_EN | DECON_FRAME_START_INT_EN);
	writel_relaxed(v, priv->decon + DECON_INTERRUPT_ENABLE);
}

static int s9p_decon_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	s9p_frame_start_irq_set(container_of(pipe, struct s9p_decon, pipe),
				true);
	return 0;
}

static void s9p_decon_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	s9p_frame_start_irq_set(container_of(pipe, struct s9p_decon, pipe),
				false);
}

/*
 * One DCS short write. The command FIFO is shared with the pixel path, so
 * the caller does this between frames; the status bit is cleared first and
 * then polled, exactly as the vendor driver does.
 */
static void s9p_decon_dcs(struct s9p_decon *priv, u8 command)
{
	u32 v;

	if (!priv->dsim || !s9p_panel_dcs)
		return;
	writel_relaxed(DSIM_INTSRC_PH_FIFO_EMPTY, priv->dsim + DSIM_INTSRC);
	writel_relaxed(DSIM_PKTHDR_ID(MIPI_DCS_SHORT_WRITE) |
		       DSIM_PKTHDR_DATA0(command) | DSIM_PKTHDR_DATA1(0),
		       priv->dsim + DSIM_PKTHDR);
	if (readl_poll_timeout_atomic(priv->dsim + DSIM_INTSRC, v,
				      v & DSIM_INTSRC_PH_FIFO_EMPTY, 10, 20000))
		dev_warn_once(priv->drm.dev,
			      "panel did not take DCS %02x (INTSRC=%08x)\n",
			      command, v);
}

/*
 * Panel down: stop the emission, then power the controller down. The order
 * is the one the MIPI DCS specification gives -- set_display_off first, so
 * the panel does not latch a half-drawn frame, then enter_sleep_mode.
 */
static void s9p_panel_down(struct s9p_decon *priv)
{
	s9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_OFF);
	if (!s9p_panel_sleep)
		return;
	msleep(PANEL_FRAME_DELAY_MS);
	s9p_decon_dcs(priv, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(PANEL_SLEEP_DELAY_MS);
}

/* Panel up: the reverse, with the boost converter's settling time. */
static void s9p_panel_up(struct s9p_decon *priv)
{
	if (s9p_panel_sleep) {
		s9p_decon_dcs(priv, MIPI_DCS_EXIT_SLEEP_MODE);
		msleep(PANEL_SLEEP_DELAY_MS);
	}
	s9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_ON);
}

/* ----------------------------------------------------------------- pipe */

static void s9p_decon_pipe_enable(struct drm_simple_display_pipe *pipe,
				  struct drm_crtc_state *crtc_state,
				  struct drm_plane_state *plane_state)
{
	struct s9p_decon *priv = container_of(pipe, struct s9p_decon, pipe);
	struct drm_gem_dma_object *obj;

	/*
	 * DECON and DSIM have been running since the bootloader; "enabling"
	 * the pipeline means pointing the inherited fetch channel at our
	 * buffer (the first flip below) and arming the trigger. The channel
	 * configuration itself is never touched.
	 */
	s9p_panel_up(priv);
	obj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);
	s9p_wait_shadow_idle(priv);
	s9p_decon_program_address(priv, obj->dma_addr);
	s9p_frame_start_irq_set(priv, true);
	priv->enabled = true;
	drm_crtc_vblank_on(&pipe->crtc);
}


/*
 * Turning the display off is three steps, because this is a MIPI command-mode
 * panel with its own frame memory: a panel that simply stops receiving frames
 * keeps showing the last one it got.
 *
 *  1. push one all-black frame, so the panel's own memory holds black if
 *     anything ever brings it back without a flip,
 *  2. set_display_off, which stops the emission,
 *  3. enter_sleep_mode, which powers the panel controller, its gate driver
 *     and its boost converter down -- this is the step that makes the panel
 *     actually dark instead of merely black. Measured against a damaged spot
 *     in this device's panel, step 2 alone still leaves it visibly lit.
 */
static void s9p_decon_blank(struct s9p_decon *priv)
{
	struct drm_framebuffer *fb = priv->pipe.plane.state->fb;
	struct drm_gem_dma_object *obj;

	if (!priv->black_dma)
		return;
	if (s9p_wait_shadow_idle(priv))
		return;
	s9p_decon_program_address(priv, priv->black_dma);
	/*
	 * One frame has to leave for the panel to store it. The trigger is
	 * TE-driven at 60 Hz; 50 ms covers a frame even if TE is late.
	 */
	msleep(50);
	s9p_panel_down(priv);
	if (fb) {
		/* leave the plane's own buffer as the programmed source again,
		 * so a re-enable without a flip shows the client's content */
		obj = to_drm_gem_dma_obj(fb->obj[0]);
		if (!s9p_wait_shadow_idle(priv))
			writel_relaxed((u32)obj->dma_addr,
				       s9p_chan_regs(priv, priv->chan) +
				       IDMA_IN_BASE_ADDR_Y);
	}
}

static void s9p_decon_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct s9p_decon *priv = container_of(pipe, struct s9p_decon, pipe);

	/*
	 * Before the interrupt goes: the vblank core accounts for an enabled
	 * CRTC and warns ("driver forgot to call drm_crtc_vblank_off") on the
	 * first real DPMS off otherwise. It also releases anything waiting on
	 * a vblank that will now never arrive.
	 */
	drm_crtc_vblank_off(&pipe->crtc);
	s9p_decon_blank(priv);
	s9p_frame_start_irq_set(priv, false);
	priv->enabled = false;
}

static void s9p_decon_pipe_update(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *old_state)
{
	struct s9p_decon *priv = container_of(pipe, struct s9p_decon, pipe);
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_gem_dma_object *obj;
	struct drm_framebuffer *fb = pipe->plane.state->fb;

	if (fb && priv->enabled) {
		if (s9p_wait_shadow_idle(priv)) {
			drm_err(&priv->drm,
				"previous shadow update stuck, skipping flip\n");
		} else {
			obj = to_drm_gem_dma_obj(fb->obj[0]);
			s9p_decon_program_address(priv, obj->dma_addr);
			/*
			 * The vendor's wait_update_done_and_mask half of the
			 * handshake. The new base address only becomes the one
			 * DECON reads when the shadow request is latched at a
			 * TE edge; until then the block is still fetching the
			 * previous framebuffer.
			 *
			 * Returning before the latch lets the atomic helper
			 * finish the commit and hand that previous framebuffer
			 * back to the compositor, which then draws the next
			 * frame into it while DECON is still reading it. On
			 * screen that is a torn, half-drawn frame right after
			 * every click, followed by the correct one.
			 */
			if (s9p_wait_shadow_idle(priv))
				drm_err_ratelimited(&priv->drm,
					"flip did not latch within 50 ms\n");
		}
	}

	if (crtc->state->event) {
		unsigned long flags;

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		if (crtc->state->active &&
		    drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static const struct drm_simple_display_pipe_funcs s9p_pipe_funcs = {
	.enable = s9p_decon_pipe_enable,
	.disable = s9p_decon_pipe_disable,
	.update = s9p_decon_pipe_update,
	.enable_vblank = s9p_decon_enable_vblank,
	.disable_vblank = s9p_decon_disable_vblank,
};

/* ------------------------------------------------------------- connector */

static int s9p_decon_get_modes(struct drm_connector *connector)
{
	struct s9p_decon *priv = container_of(connector, struct s9p_decon,
					      connector);
	struct drm_display_mode *mode;

	mode = drm_cvt_mode(connector->dev, priv->width, priv->height, 60,
			    false, false, false);
	if (!mode)
		return 0;
	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	/* 6.4 inch diagonal; userspace uses this for its scale factor. */
	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 140;

	return 1;
}

static enum drm_connector_status
s9p_decon_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_helper_funcs s9p_conn_help = {
	.get_modes = s9p_decon_get_modes,
};

static const struct drm_connector_funcs s9p_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.detect = s9p_decon_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ------------------------------------------------------------- drm device */

static const struct drm_mode_config_funcs s9p_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(s9p_fops);

static const struct drm_driver s9p_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS,
	.fbdev_probe = drm_fbdev_dma_driver_fbdev_probe,
	.fops = &s9p_fops,
	.name = DRIVER_NAME,
	.desc = "Galaxy S9+ DECON scanout",
	.major = 1,
	.minor = 1,
};

/* ---------------------------------------------------------------- probe */

static int s9p_decon_probe(struct platform_device *pdev)
{
	struct s9p_decon *priv;
	struct drm_device *drm;
	u32 win_en, win_chmap, base, src, img;
	int ret, i, nirq = 0;

	priv = devm_drm_dev_alloc(&pdev->dev, &s9p_drm_driver,
				  struct s9p_decon, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);
	drm = &priv->drm;

	priv->decon = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->decon))
		return PTR_ERR(priv->decon);
	priv->dpp = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->dpp))
		return PTR_ERR(priv->dpp);

	/*
	 * Identify the live window by geometry -- NEVER by reading DPP
	 * registers of arbitrary windows here: a clock-gated channel bus-
	 * aborts on read and wedges the SoC silently (measured). The sboot
	 * scanout window is the enabled fullscreen one.
	 */
	win_en = readl_relaxed(priv->decon + DECON_DATA_PATH_CONTROL_0);
	win_chmap = readl_relaxed(priv->decon + DECON_DATA_PATH_CONTROL_1);
	priv->inherited_path0 = win_en;
	priv->inherited_path1 = win_chmap;
	priv->inherited_trig_ctrl =
		readl_relaxed(priv->decon + DECON_HW_SW_TRIG_CONTROL);

	priv->win = DECON_MAX_WIN;
	for (i = 0; i < DECON_MAX_WIN; i++) {
		u32 start, end;

		if (!(win_en & DECON_WIN_EN(i)))
			continue;
		start = readl_relaxed(priv->decon + WIN_START_POSITION(i));
		end = readl_relaxed(priv->decon + WIN_END_POSITION(i));
		dev_info(&pdev->dev,
			 "window %u: enabled, chmap %u, start %08x end %08x\n",
			 i, (win_chmap >> (4 * i)) & 0x7, start, end);
		if (start == 0 && end == ((PANEL_H - 1) << 16 | (PANEL_W - 1))) {
			priv->win = i;
			priv->chan = (win_chmap >> (4 * i)) & 0x7;
		}
	}

	/*
	 * The channel comes from live evidence, not from the CHMAP nibble:
	 * CHMAP is a DMA type (G0..VGF1), and sboot wired the window to
	 * channel 0 (stock DT default_idma = <0x0>), whose shadow bank holds
	 * the scanout address 0xcc000000. Candidate channels are 0 and the
	 * CHMAP-derived one -- both are proven readable in a fresh boot
	 * (channel 0 is the active fetch, channel 5 was read throughout
	 * earlier probes without a bus wedge). Anything else is not touched;
	 * no blind scans: a read into a really clock-gated channel wedges
	 * the SoC and arm64 does not fix external aborts via the exception
	 * table.
	 */
	if (priv->win == DECON_MAX_WIN) {
		dev_err(&pdev->dev,
			"no fullscreen DECON window (path0=%08x path1=%08x)\n",
			win_en, win_chmap);
		return -ENODEV;
	}

	{
		static const unsigned int candidates[] = { 0 };
		unsigned int chmap_chan = (win_chmap >> (4 * priv->win)) & 0x7;
		bool found = false;
		unsigned int ci, ch;

		for (ci = 0; ci < ARRAY_SIZE(candidates) && !found; ci++) {
			ch = candidates[ci];
			if (readl_relaxed(s9p_chan_regs(priv, ch) +
					  IDMA_SHD_BANK + IDMA_IN_BASE_ADDR_Y)
			    == S9P_SBOOT_FB_BASE) {
				priv->chan = ch;
				found = true;
			}
		}
		if (!found && chmap_chan < DECON_MAX_WIN &&
		    readl_relaxed(s9p_chan_regs(priv, chmap_chan) +
				  IDMA_SHD_BANK + IDMA_IN_BASE_ADDR_Y)
		    == S9P_SBOOT_FB_BASE) {
			priv->chan = chmap_chan;
			found = true;
		}
		if (!found) {
			dev_err(&pdev->dev,
				"no fetch channel carries the sboot framebuffer (chmap_chan=%u)\n",
				chmap_chan);
			return -ENODEV;
		}
	}

	/*
	 * Identification evidence. Both banks should carry the sboot
	 * configuration; log it once so a mismatch is visible in the log.
	 */
	base = readl_relaxed(s9p_chan_regs(priv, priv->chan) +
			     IDMA_SHD_BANK + IDMA_IN_BASE_ADDR_Y);
	src = readl_relaxed(s9p_chan_regs(priv, priv->chan) + IDMA_SRC_SIZE);
	img = readl_relaxed(s9p_chan_regs(priv, priv->chan) + IDMA_IMG_SIZE);
	dev_info(&pdev->dev,
		 "inherited channel %u: shadow_base=%08x normal_base=%08x src_size=%08x img_size=%08x con=%08x\n",
		 priv->chan, base,
		 readl_relaxed(s9p_chan_regs(priv, priv->chan) +
			       IDMA_IN_BASE_ADDR_Y), src, img,
		 readl_relaxed(s9p_chan_regs(priv, priv->chan) + IDMA_IN_CON));

	/*
	 * Stock DT: psr_mode=2 (MIPI command mode), trig_mode=0 (hardware
	 * trigger, TE-driven), dsi_mode=0 (single DSI). In command mode a
	 * frame only transfers after the trigger is unmasked and TE fires.
	 */
	priv->command_mode = true;
	priv->hw_trigger = true;
	priv->width = PANEL_W;
	priv->height = PANEL_H;

	/* Enable only our observation interrupt, read-modify-write. */
	s9p_frame_start_irq_set(priv, false);
	for (i = 0; i < 3; i++) {
		int irq = platform_get_irq(pdev, i);

		if (irq < 0)
			continue;
		ret = devm_request_irq(&pdev->dev, irq, s9p_decon_irq, 0,
				       "s9p-decon", priv);
		if (ret == 0) {
			if (nirq == 0)
				priv->irq_frame_start = irq;
			nirq++;
		} else {
			dev_warn(&pdev->dev, "irq %d: %d\n", i, ret);
		}
	}
	dev_info(&pdev->dev,
		 "take over win=%u ch=%u trig=%08x, %d irq(s), command-mode\n",
		 priv->win, priv->chan, priv->inherited_trig_ctrl, nirq);

	/*
	 * Take the display away from simpledrm. The /chosen framebuffer node
	 * stays enabled so the early boot is identical to the bootfb builds
	 * that are proven on this device; this evicts the generic driver
	 * (and with it the phantom second KMS device mutter would otherwise
	 * drive as a second monitor). fbcon moves over to this driver's fbdev.
	 */
	/*
	 * One black frame, allocated once from CMA. Writing black into the
	 * client's own buffer instead would corrupt what it hands back on
	 * the next flip.
	 */
	priv->dsim = devm_ioremap(&pdev->dev, DSIM_PHYS, 0x100);

	priv->black_cpu = dma_alloc_wc(&pdev->dev, PANEL_W * PANEL_H * 4,
				       &priv->black_dma, GFP_KERNEL);
	if (priv->black_cpu)
		memset(priv->black_cpu, 0, PANEL_W * PANEL_H * 4);
	else
		dev_warn(&pdev->dev, "no black frame: display-off keeps the last image\n");

	aperture_remove_conflicting_devices(S9P_SBOOT_FB_BASE,
					    PANEL_W * PANEL_H * 4, DRIVER_NAME);

	drm_mode_config_init(drm);
	drm->mode_config.min_width = priv->width;
	drm->mode_config.max_width = priv->width;
	drm->mode_config.min_height = priv->height;
	drm->mode_config.max_height = priv->height;
	drm->mode_config.funcs = &s9p_mode_funcs;

	ret = drm_connector_init(drm, &priv->connector, &s9p_conn_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		goto err_config;
	drm_connector_helper_add(&priv->connector, &s9p_conn_help);

	ret = drm_simple_display_pipe_init(drm, &priv->pipe, &s9p_pipe_funcs,
					   s9p_formats,
					   ARRAY_SIZE(s9p_formats), NULL,
					   &priv->connector);
	if (ret)
		goto err_config;

	drm_mode_config_reset(drm);
	ret = drm_vblank_init(drm, 1);
	if (ret)
		goto err_config;

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_config;

	drm_client_setup_with_fourcc(drm, s9p_formats[0]);
	drm_info(drm, "DECON scanout %ux%u on window %u\n",
		 priv->width, priv->height, priv->win);
	return 0;

err_config:
	drm_mode_config_cleanup(drm);
	return ret;
}

static const struct of_device_id s9p_decon_of_match[] = {
	{ .compatible = "samsung,exynos9810-decon" },
	{ },
};
MODULE_DEVICE_TABLE(of, s9p_decon_of_match);

static struct platform_driver s9p_decon_driver = {
	.probe = s9p_decon_probe,
	.driver = {
		.name = "s9p-decon",
		.of_match_table = s9p_decon_of_match,
	},
};
/* after simpledrm (a device_initcall) has bound, so the eviction above
 * finds it and it cannot come back afterwards */
static int __init s9p_decon_init(void)
{
	return platform_driver_register(&s9p_decon_driver);
}
late_initcall(s9p_decon_init);

MODULE_DESCRIPTION("Galaxy S9+ DECON scanout (bootloader display takeover)");
MODULE_LICENSE("GPL v2");
