// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): audio over the USB-C DisplayPort output.
 *
 * Same path as the vendor kernel (sound/soc/samsung/dp_dma.c): no ABOX, no
 * firmware. The PL330 PDMA0 (0x1a2e0000, request 28) copies the PCM buffer
 * into the SST1 audio FIFO of the DP controller (0x11095818); s9p-dp sets up
 * Maud/Naud, the audio InfoFrame and the IEC 60958 channel status.
 *
 * The port's device tree has no PDMA0 node: it is added with an OF changeset
 * and registered on the AMBA bus by hand, together with a small consumer
 * node that holds the "dmas" reference.
 */

#include <linux/amba/bus.h>
#include <linux/dma-mapping.h>
#include <linux/of_address.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "s9p-dpaudio.h"

#define PDMA0_BASE	0x1a2e0000
#define PDMA0_IRQ	92		/* GIC SPI */
#define DP_DMA_REQ	28

static struct {
	struct of_changeset cs;
	struct device_node *pdma_np, *user_np;
	struct amba_device *adev;
	struct dma_chan *chan;
	struct snd_card *card;
	bool on;		/* DP audio programmed */
	struct snd_pcm_substream *ss;
	dma_cookie_t cookie;
	struct snd_jack *jack;
	struct delayed_work jack_work;
	int plugged;
} a;

/* monitor present: the "HDMI/DP,pcm=0 Jack" control the sound servers watch */
static void jack_work(struct work_struct *w)
{
	int now = s9p_dp_audio_ready();

	if (now != a.plugged) {
		a.plugged = now;
		snd_jack_report(a.jack, now ? SND_JACK_AVOUT : 0);
		pr_info("s9p-dpaudio: monitor %s\n", now ? "connected" : "gone");
		if (!now && a.ss) {
			unsigned long flags;

			snd_pcm_stream_lock_irqsave(a.ss, flags);
			if (snd_pcm_running(a.ss))
				snd_pcm_stop(a.ss, SNDRV_PCM_STATE_DISCONNECTED);
			snd_pcm_stream_unlock_irqrestore(a.ss, flags);
		}
	}
	schedule_delayed_work(&a.jack_work, HZ / 2);
}

/* ------------------------------------------------------------------ */
/* PDMA0 on the AMBA bus                                                */

static struct property *mkprop(const char *name, const void *val, int len)
{
	struct property *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return NULL;
	p->name = kstrdup(name, GFP_KERNEL);
	p->value = len ? kmemdup(val, len, GFP_KERNEL) : kzalloc(1, GFP_KERNEL);
	p->length = len;
	if (!p->name || !p->value) {
		kfree(p->name);
		kfree(p->value);
		kfree(p);
		return NULL;
	}
	return p;
}

static int add_prop(struct device_node *np, const char *name, const void *v, int len)
{
	struct property *p = mkprop(name, v, len);

	return p ? of_changeset_add_property(&a.cs, np, p) : -ENOMEM;
}

static int add_u32s(struct device_node *np, const char *name, const u32 *v, int n)
{
	__be32 b[8];
	int i;

	for (i = 0; i < n; i++)
		b[i] = cpu_to_be32(v[i]);
	return add_prop(np, name, b, n * 4);
}

static u32 free_phandle(void)
{
	struct device_node *np;
	u32 max = 0;

	for (np = of_find_all_nodes(NULL); np; np = of_find_all_nodes(np))
		max = max(max, np->phandle);
	return max + 1;
}

static int pdma_create(void)
{
	static const char compat[] = "arm,pl330\0arm,primecell";
	struct device_node *soc, *root, *stub;
	struct resource res;
	u32 ph;
	int ret, irq;

	soc = of_find_node_by_path("/soc@0");
	root = of_find_node_by_path("/");
	stub = of_find_node_by_path("/clock-hsi2c-stub");
	if (!soc || !root || !stub) {
		ret = -ENODEV;
		goto out;
	}
	of_changeset_init(&a.cs);
	a.pdma_np = of_changeset_create_node(&a.cs, soc, "dma-controller@1a2e0000");
	a.user_np = of_changeset_create_node(&a.cs, root, "s9p-dpaudio");
	if (!a.pdma_np || !a.user_np) {
		ret = -ENOMEM;
		goto destroy;
	}
	ph = free_phandle();
	ret = add_prop(a.pdma_np, "compatible", compat, sizeof(compat)) ?:
	      add_u32s(a.pdma_np, "reg", (u32[]){ PDMA0_BASE, 0x1000 }, 2) ?:
	      add_u32s(a.pdma_np, "interrupts", (u32[]){ 0, PDMA0_IRQ, 4 }, 3) ?:
	      add_u32s(a.pdma_np, "clocks", (u32[]){ stub->phandle }, 1) ?:
	      add_prop(a.pdma_np, "clock-names", "apb_pclk", sizeof("apb_pclk")) ?:
	      add_u32s(a.pdma_np, "#dma-cells", (u32[]){ 1 }, 1) ?:
	      add_prop(a.pdma_np, "arm,pl330-broken-no-flushp", "", 0) ?:
	      add_u32s(a.pdma_np, "phandle", &ph, 1) ?:
	      add_u32s(a.user_np, "dmas", (u32[]){ ph, DP_DMA_REQ }, 2) ?:
	      add_prop(a.user_np, "dma-names", "tx", sizeof("tx"));
	if (ret)
		goto destroy;
	ret = of_changeset_apply(&a.cs);
	if (ret)
		goto destroy;
	/* the attach ran before the "phandle" property was added */
	a.pdma_np->phandle = ph;

	a.adev = amba_device_alloc("1a2e0000.dma-controller", PDMA0_BASE, 0x1000);
	if (!a.adev) {
		ret = -ENOMEM;
		goto revert;
	}
	a.adev->dev.of_node = of_node_get(a.pdma_np);
	a.adev->dev.fwnode = of_fwnode_handle(a.pdma_np);
	a.adev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	a.adev->dev.dma_mask = &a.adev->dev.coherent_dma_mask;
	irq = irq_of_parse_and_map(a.pdma_np, 0);
	if (!irq) {
		ret = -EINVAL;
		goto put_adev;
	}
	a.adev->irq[0] = irq;
	ret = of_address_to_resource(a.pdma_np, 0, &res);
	if (ret)
		goto put_adev;
	a.adev->res = res;
	a.adev->res.name = "pdma0";
	ret = amba_device_add(a.adev, &iomem_resource);
	if (ret) {
		pr_err("s9p-dpaudio: amba_device_add: %d\n", ret);
		goto put_adev;
	}
	pr_info("s9p-dpaudio: PDMA0 on the AMBA bus (periphid %08x, irq %d)\n",
		a.adev->periphid, irq);
	ret = 0;
	goto out;

put_adev:
	amba_device_put(a.adev);
	a.adev = NULL;
revert:
	of_changeset_revert(&a.cs);
destroy:
	of_changeset_destroy(&a.cs);
	a.pdma_np = a.user_np = NULL;
out:
	of_node_put(soc);
	of_node_put(root);
	of_node_put(stub);
	return ret;
}

static void pdma_destroy(void)
{
	if (a.adev)
		amba_device_unregister(a.adev);
	of_changeset_revert(&a.cs);
	of_changeset_destroy(&a.cs);
}

/* ------------------------------------------------------------------ */
/* ALSA PCM                                                             */

static const struct snd_pcm_hardware dpa_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
	.rate_min = 32000,
	.rate_max = 96000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 128 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 32 * 1024,
	.periods_min = 2,
	.periods_max = 64,
};

static int dpa_open(struct snd_pcm_substream *ss)
{
	int ret;

	/* no link check here: the sound server probes the device at start */
	ss->runtime->hw = dpa_hw;
	ret = snd_pcm_hw_constraint_integer(ss->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	a.ss = ss;
	return 0;
}

static int dpa_close(struct snd_pcm_substream *ss)
{
	if (a.on) {
		s9p_dp_audio_stop();
		a.on = false;
	}
	dmaengine_terminate_sync(a.chan);
	a.ss = NULL;
	return 0;
}

static int dpa_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *p)
{
	int phys = snd_pcm_format_physical_width(params_format(p));
	struct dma_slave_config cfg = {
		.direction = DMA_MEM_TO_DEV,
		.dst_addr = S9P_DP_AUDIO_FIFO,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_maxburst = phys * params_channels(p) / 32,
	};

	return dmaengine_slave_config(a.chan, &cfg);
}

static int dpa_prepare(struct snd_pcm_substream *ss)
{
	struct snd_pcm_runtime *rt = ss->runtime;
	int ret;

	if (a.on) {
		s9p_dp_audio_stop();
		a.on = false;
	}
	/*
	 * alsa-lib prepares right after hw_params, also when the sound server
	 * only probes the device: without a monitor, accept the setup and
	 * fail at trigger time instead.
	 */
	if (!s9p_dp_audio_ready())
		return 0;
	ret = s9p_dp_audio_start(rt->rate, rt->channels,
				 snd_pcm_format_width(rt->format),
				 snd_pcm_format_physical_width(rt->format));
	if (!ret)
		a.on = true;
	return ret == -ENOLINK ? 0 : ret;
}

static int dpa_hw_free(struct snd_pcm_substream *ss)
{
	if (a.on) {
		s9p_dp_audio_stop();
		a.on = false;
	}
	return 0;
}

static void dpa_period(void *arg)
{
	struct snd_pcm_substream *ss = arg;

	snd_pcm_period_elapsed(ss);
}

static int dpa_dma_start(struct snd_pcm_substream *ss)
{
	struct snd_pcm_runtime *rt = ss->runtime;
	struct dma_async_tx_descriptor *d;

	d = dmaengine_prep_dma_cyclic(a.chan, rt->dma_addr,
				      snd_pcm_lib_buffer_bytes(ss),
				      snd_pcm_lib_period_bytes(ss),
				      DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
	if (!d)
		return -ENOMEM;
	d->callback = dpa_period;
	d->callback_param = ss;
	a.cookie = dmaengine_submit(d);
	dma_async_issue_pending(a.chan);
	return 0;
}

static int dpa_trigger(struct snd_pcm_substream *ss, int cmd)
{
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!a.on)
			return -ENOLINK;	/* no monitor when prepared */
		ret = dpa_dma_start(ss);
		if (!ret)
			s9p_dp_audio_run(true);	/* release the forced-low DMA request */
		return ret;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		dmaengine_resume(a.chan);
		s9p_dp_audio_run(true);
		return 0;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		s9p_dp_audio_run(false);
		return dmaengine_pause(a.chan);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		s9p_dp_audio_run(false);
		return dmaengine_terminate_async(a.chan);
	}
	return -EINVAL;
}

static snd_pcm_uframes_t dpa_pointer(struct snd_pcm_substream *ss)
{
	struct snd_pcm_runtime *rt = ss->runtime;
	struct dma_tx_state st;
	unsigned int buf = snd_pcm_lib_buffer_bytes(ss), pos = 0;

	if (dmaengine_tx_status(a.chan, a.cookie, &st) != DMA_ERROR &&
	    st.residue && st.residue <= buf)
		pos = buf - st.residue;
	return bytes_to_frames(rt, pos);
}

static int dpa_sync_stop(struct snd_pcm_substream *ss)
{
	dmaengine_synchronize(a.chan);
	return 0;
}

static const struct snd_pcm_ops dpa_ops = {
	.open = dpa_open,
	.close = dpa_close,
	.hw_params = dpa_hw_params,
	.hw_free = dpa_hw_free,
	.prepare = dpa_prepare,
	.trigger = dpa_trigger,
	.pointer = dpa_pointer,
	.sync_stop = dpa_sync_stop,
};

static int __init s9p_dpaudio_init(void)
{
	struct snd_pcm *pcm;
	int ret;

	ret = pdma_create();
	if (ret)
		return ret;
	a.chan = of_dma_request_slave_channel(a.user_np, "tx");
	if (IS_ERR(a.chan)) {
		ret = PTR_ERR(a.chan);
		pr_err("s9p-dpaudio: no PDMA0 channel: %d\n", ret);
		a.chan = NULL;
		goto err_pdma;
	}
	ret = snd_card_new(&a.adev->dev, SNDRV_DEFAULT_IDX1, "S9pDP", THIS_MODULE, 0, &a.card);
	if (ret)
		goto err_chan;
	strscpy(a.card->driver, "s9p-dp", sizeof(a.card->driver));
	strscpy(a.card->shortname, "Galaxy S9+ DP", sizeof(a.card->shortname));
	strscpy(a.card->longname, "Galaxy S9+ USB-C DisplayPort audio", sizeof(a.card->longname));
	ret = snd_pcm_new(a.card, "DisplayPort", 0, 1, 0, &pcm);
	if (ret)
		goto err_card;
	strscpy(pcm->name, "DisplayPort", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &dpa_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, a.chan->device->dev,
				       64 * 1024, dpa_hw.buffer_bytes_max);
	ret = snd_jack_new(a.card, "HDMI/DP,pcm=0", SND_JACK_AVOUT, &a.jack, true, false);
	if (ret)
		goto err_card;
	ret = snd_card_register(a.card);
	if (ret)
		goto err_card;
	a.plugged = -1;
	INIT_DELAYED_WORK(&a.jack_work, jack_work);
	schedule_delayed_work(&a.jack_work, 0);
	pr_info("s9p-dpaudio: card registered (%s)\n", dma_chan_name(a.chan));
	return 0;

err_card:
	snd_card_free(a.card);
err_chan:
	dma_release_channel(a.chan);
err_pdma:
	pdma_destroy();
	return ret;
}

static void __exit s9p_dpaudio_exit(void)
{
	cancel_delayed_work_sync(&a.jack_work);
	snd_card_free(a.card);
	dma_release_channel(a.chan);
	pdma_destroy();
}

module_init(s9p_dpaudio_init);
module_exit(s9p_dpaudio_exit);
MODULE_DESCRIPTION("Galaxy S9+ DisplayPort audio (PL330 -> SST1 audio FIFO)");
MODULE_LICENSE("GPL");
