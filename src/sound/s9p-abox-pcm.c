// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): ALSA card over ABOX for the codec and the speakers.
 *
 *   PCM 0 "Headphones": RDMA0 -> SPUS IN0 -> SIFS0 (mixer) -> UAIF0 (I2S) ->
 *                       Cirrus CS47L93 AIF1 -> HPOUT2 (headset jack)
 *   PCM 1 "Speaker":    RDMA1 -> SPUS IN1 -> SIFS1 -> UAIF1 (I2S) ->
 *                       two MAX98512 (left 0x39, right 0x3a)
 *
 * The RDMA side follows the vendor abox_rdma.c message sequence; routing and
 * UAIF registers are written directly, as the vendor DAPM/abox_if.c do
 * (audio/analisis/01-abox.md, sections 2.3-2.5). ABOX provides the bit and
 * frame clocks. Fixed 48 kHz / S16 / stereo, the rate the SIFS nodes run at,
 * so that no ASRC is needed. No DSM speaker protection (that is firmware
 * DSP work on stock): the amplifier gain stays low.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "s9p-abox.h"

/* madera (sound/soc/codecs/madera.h) */
#define MADERA_FLL1_REFCLK	1
#define MADERA_FLL_SRC_MCLK1	0
#define MADERA_CLK_SYSCLK_1	1
#define MADERA_CLK_SRC_FLL1	0x4

#define MCLK1_HZ		26000000
#define SYSCLK_HZ		98304000

/* ABOX SFR */
#define ROUTE_CTRL0		0x0040		/* UAIFx SPK source, 4 bits each */
#define  ROUTE_SIFS0		1
#define  ROUTE_SIFS1		2
#define SPUS_CTRL0		0x0200		/* per SPUS input: IN b3, OUT b2:1, ASRC b0 */
#define  SPUS_OUT_SIFS1		0
#define  SPUS_OUT_SIFS0		1
#define SPUS_CTRL1		0x0204
#define  MIXP_FORMAT		GENMASK(4, 0)	/* SIFS0 (mixer) format */
#define  SIFS1_SEL		GENMASK(18, 16)	/* SIFS1 <- SPUS OUTn */
#define SPUS_CTRL2		0x0208
#define  MIXP_FLUSH		BIT(0)
#define SPUS_CTRL3		0x020c
#define  SIFS1_FLUSH		BIT(0)
#define UAIF_CTRL0(n)		(0x0500 + 0x10 * (n))
#define  UAIF_MODE_MASTER	BIT(2)
#define  UAIF_SPK_ENABLE	BIT(0)
#define UAIF_CTRL1(n)		(0x0504 + 0x10 * (n))
#define RDMA_STATUS(n)		(0x1030 + 0x100 * (n))
#define  RDMA_PROGRESS		BIT(31)

#define FMT_S16_STEREO		0x09	/* abox_get_format(16, 2) = (ch-1) | 1 << 3 */

enum { PATH_HP, PATH_SPK, NUM_PATHS };

static const struct path_cfg {
	const char *name;
	int rdma, uaif;
	bool mixer;		/* SIFS0 (mixer) or SIFS1 (mux) */
} paths[NUM_PATHS] = {
	[PATH_HP]  = { "Headphones", 0, 0, true },
	[PATH_SPK] = { "Speaker",    1, 1, false },
};

static struct path_state {
	struct snd_pcm_substream *ss;
	u32 iova;
	u32 pointer;		/* last IOVA reported by the firmware, 0: use RDMA_STATUS */
	bool opened;
	/*
	 * The RDMA buffer belongs to s9p-abox: never put it in
	 * substream->dma_buffer, ALSA frees that one with the PCM.
	 */
	struct snd_dma_buffer buf;
} st[NUM_PATHS];

static void __iomem *sfr(void)
{
	return s9p_abox_sfr();
}

static void rmw(u32 off, u32 clr, u32 set)
{
	writel((readl(sfr() + off) & ~clr) | set, sfr() + off);
}

static int path_of(struct snd_pcm_substream *ss)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(ss);

	return snd_soc_rtd_to_cpu(rtd, 0)->id;
}

static int ipc_pcm(int path, int msgtype,
		   void (*fill)(struct s9p_abox_pcmtask_msg *t, void *arg), void *arg)
{
	struct s9p_abox_ipc_msg m = {};

	m.ipcid = IPC_PCMPLAYBACK;
	m.task_id = paths[path].rdma;
	m.msg.pcmtask.msgtype = msgtype;
	m.msg.pcmtask.channel_id = paths[path].rdma;
	if (fill)
		fill(&m.msg.pcmtask, arg);
	return s9p_abox_ipc(&m);
}

static int ipc_config(int msgtype, int p1, int p2)
{
	struct s9p_abox_ipc_msg m = {};

	m.ipcid = IPC_ABOX_CONFIG;
	m.msg.config.msgtype = msgtype;
	m.msg.config.param1 = p1;
	m.msg.config.param2 = p2;
	return s9p_abox_ipc(&m);
}

/* IRQ/poll context, from s9p-abox */
static void abox_period(void *data, int rdma, s64 pointer)
{
	int i;

	for (i = 0; i < NUM_PATHS; i++) {
		if (paths[i].rdma != rdma || !st[i].ss)
			continue;
		/*
		 * RDMAx_BUF_EMPTY SGI: no pointer in it; as the vendor, clear the
		 * saved one so that pointer() reads the RDMA status register.
		 */
		st[i].pointer = pointer >= 0 ? pointer : 0;
		snd_pcm_period_elapsed(st[i].ss);
	}
}

static void abox_ipc_msg(void *data, const struct s9p_abox_ipc_msg *m)
{
	dev_info_ratelimited(s9p_abox_dev(), "pcm: IPC from ABOX %d/%d ch %d param %08x\n",
			     m->ipcid, m->msg.pcmtask.msgtype, m->msg.pcmtask.channel_id,
			     m->msg.pcmtask.param.pointer);
}

static const struct s9p_abox_ops abox_ops = {
	.rdma_period = abox_period,
	.ipc = abox_ipc_msg,
};

/* ------------------------------------------------------------------ */
/* platform (PCM over RDMAn)                                            */

static const struct snd_pcm_hardware abox_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 256 * 1024,
	/* period notifications are polled every 20 ms: keep periods longer */
	.period_bytes_min = 8192,
	.period_bytes_max = 64 * 1024,
	.periods_min = 2,
	.periods_max = 32,
};

static int pcm_open(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	int path = path_of(ss), ret;

	if (!s9p_abox_ready())
		return -ENODEV;
	snd_soc_set_runtime_hwparams(ss, &abox_pcm_hw);
	ret = snd_pcm_hw_constraint_integer(ss->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	ret = ipc_pcm(path, PCM_PLTDAI_OPEN, NULL, NULL);
	if (ret)
		return ret;
	st[path].ss = ss;
	st[path].opened = true;
	return 0;
}

static int pcm_close(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	int path = path_of(ss);

	st[path].ss = NULL;
	if (st[path].opened)
		ipc_pcm(path, PCM_PLTDAI_CLOSE, NULL, NULL);
	st[path].opened = false;
	return 0;
}

struct hw_arg {
	struct snd_pcm_hw_params *hp;
	u32 iova;
};

static void fill_setbuf(struct s9p_abox_pcmtask_msg *t, void *arg)
{
	struct hw_arg *a = arg;

	t->param.setbuff.phyaddr = a->iova;
	t->param.setbuff.size = params_period_bytes(a->hp);
	t->param.setbuff.count = params_periods(a->hp);
}

static void fill_hwparams(struct s9p_abox_pcmtask_msg *t, void *arg)
{
	struct hw_arg *a = arg;

	t->param.hw_params.sample_rate = params_rate(a->hp);
	t->param.hw_params.bit_depth = params_width(a->hp);
	t->param.hw_params.channels = params_channels(a->hp);
}

static int pcm_hw_params(struct snd_soc_component *c, struct snd_pcm_substream *ss,
			 struct snd_pcm_hw_params *hp)
{
	int path = path_of(ss), ret;
	struct path_state *s = &st[path];
	struct snd_dma_buffer *b = &s->buf;
	struct hw_arg arg = { .hp = hp };
	dma_addr_t pa;
	void *va;

	va = s9p_abox_rdma_buffer(paths[path].rdma, &pa, &s->iova);
	if (!va)
		return -ENOMEM;
	arg.iova = s->iova;
	b->dev.type = SNDRV_DMA_TYPE_DEV;
	b->dev.dev = s9p_abox_dev();
	b->area = va;
	b->addr = pa;
	b->bytes = SZ_1M;
	snd_pcm_set_runtime_buffer(ss, b);
	ss->runtime->dma_bytes = params_buffer_bytes(hp);

	/* SIFS node format and rate, then the RDMA buffer */
	if (paths[path].mixer)
		ret = ipc_config(SET_MIXER_SAMPLE_RATE, params_rate(hp), 0) ?:
		      ipc_config(SET_MIXER_FORMAT, params_width(hp), params_channels(hp));
	else
		ret = ipc_config(SET_OUT1_SAMPLE_RATE, params_rate(hp), 0) ?:
		      ipc_config(SET_OUT1_FORMAT, params_width(hp), params_channels(hp));
	ret = ret ?: ipc_pcm(path, PCM_SET_BUFFER, fill_setbuf, &arg) ?:
	      ipc_pcm(path, PCM_PLTDAI_HW_PARAMS, fill_hwparams, &arg);
	dev_info(c->dev, "%s: %u Hz, %u x %u bytes, iova %08x: %d\n", paths[path].name,
		 params_rate(hp), params_periods(hp), params_period_bytes(hp), s->iova, ret);
	return ret;
}

static int pcm_hw_free(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	ipc_pcm(path_of(ss), PCM_PLTDAI_HW_FREE, NULL, NULL);
	snd_pcm_set_runtime_buffer(ss, NULL);
	return 0;
}

static void route(int path)
{
	const struct path_cfg *pc = &paths[path];
	u32 sh = 4 * pc->rdma;

	/* SPUS INn = RDMAn, ASRC off, OUTn = SIFS0/SIFS1 */
	rmw(SPUS_CTRL0, 0xf << sh,
	    (pc->mixer ? SPUS_OUT_SIFS0 : SPUS_OUT_SIFS1) << (sh + 1));
	if (pc->mixer) {
		rmw(SPUS_CTRL1, MIXP_FORMAT, FMT_S16_STEREO);
		writel(MIXP_FLUSH, sfr() + SPUS_CTRL2);
	} else {
		rmw(SPUS_CTRL1, SIFS1_SEL, pc->rdma << 16);	/* SIFS1 <- SPUS OUTn */
		writel(SIFS1_FLUSH, sfr() + SPUS_CTRL3);
	}
	/* UAIFn SPK = SIFS0 / SIFS1 */
	rmw(ROUTE_CTRL0, 0xf << (4 * pc->uaif),
	    (pc->mixer ? ROUTE_SIFS0 : ROUTE_SIFS1) << (4 * pc->uaif));
}

static int pcm_prepare(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	int path = path_of(ss);

	st[path].pointer = st[path].iova;
	route(path);
	return ipc_pcm(path, PCM_PLTDAI_PREPARE, NULL, NULL);
}

static void fill_trigger(struct s9p_abox_pcmtask_msg *t, void *arg)
{
	t->param.trigger = (long)arg;
}

static int pcm_trigger(struct snd_soc_component *c, struct snd_pcm_substream *ss, int cmd)
{
	int path = path_of(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		return ipc_pcm(path, PCM_PLTDAI_TRIGGER, fill_trigger, (void *)1L);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		return ipc_pcm(path, PCM_PLTDAI_TRIGGER, fill_trigger, (void *)0L);
	}
	return -EINVAL;
}

static snd_pcm_uframes_t pcm_pointer(struct snd_soc_component *c, struct snd_pcm_substream *ss)
{
	int path = path_of(ss);
	struct path_state *s = &st[path];
	size_t buf = snd_pcm_lib_buffer_bytes(ss), pos = 0;

	if (s->pointer >= s->iova && s->pointer < s->iova + buf) {
		pos = s->pointer - s->iova;
	} else {
		u32 v = readl(sfr() + RDMA_STATUS(paths[path].rdma));

		if (v & RDMA_PROGRESS)
			pos = ((((v >> 16) & 0x1fff) << 4) + (v & 0x1fff)) % buf;
	}
	return bytes_to_frames(ss->runtime, pos);
}

static int pcm_mmap(struct snd_soc_component *c, struct snd_pcm_substream *ss,
		    struct vm_area_struct *vma)
{
	return snd_pcm_lib_default_mmap(ss, vma);
}

static const struct snd_soc_component_driver abox_pcm_component = {
	.name = "s9p-abox-pcm",
	.open = pcm_open,
	.close = pcm_close,
	.hw_params = pcm_hw_params,
	.hw_free = pcm_hw_free,
	.prepare = pcm_prepare,
	.trigger = pcm_trigger,
	.pointer = pcm_pointer,
	.mmap = pcm_mmap,
};

/* ------------------------------------------------------------------ */
/* UAIF (I2S)                                                           */

static int uaif_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *hp,
			  struct snd_soc_dai *dai)
{
	int u = paths[dai->id].uaif, w = params_width(hp), ch = params_channels(hp);
	int ret = s9p_abox_uaif_bclk(u, params_rate(hp) * ch * w);

	if (ret)
		return ret;
	/* FORMAT [28:24], BCLK_POLARITY b23 (NB_NF), I2S, SLOT_MAX [20:18], SBIT_MAX [17:12] */
	writel(FMT_S16_STEREO << 24 | BIT(23) | (ch - 1) << 18 | (w - 1) << 12,
	       sfr() + UAIF_CTRL1(u));
	rmw(UAIF_CTRL0(u), UAIF_SPK_ENABLE, UAIF_MODE_MASTER);
	return 0;
}

static int uaif_trigger(struct snd_pcm_substream *ss, int cmd, struct snd_soc_dai *dai)
{
	if (cmd == SNDRV_PCM_TRIGGER_START || cmd == SNDRV_PCM_TRIGGER_RESUME ||
	    cmd == SNDRV_PCM_TRIGGER_PAUSE_RELEASE)
		rmw(UAIF_CTRL0(paths[dai->id].uaif), 0, UAIF_SPK_ENABLE);
	return 0;
}

static void uaif_shutdown(struct snd_pcm_substream *ss, struct snd_soc_dai *dai)
{
	/* as the vendor: off at shutdown, not at trigger stop */
	rmw(UAIF_CTRL0(paths[dai->id].uaif), UAIF_SPK_ENABLE, 0);
}

static int uaif_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	/* only what the card uses: I2S, NB_NF, UAIF is the clock provider */
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;
	return 0;
}

static const struct snd_soc_dai_ops uaif_ops = {
	.hw_params = uaif_hw_params,
	.trigger = uaif_trigger,
	.shutdown = uaif_shutdown,
	.set_fmt = uaif_set_fmt,
};

#define UAIF_DAI(_id, _name, _stream)				\
	{							\
		.id = _id,					\
		.name = _name,					\
		.playback = {					\
			.stream_name = _stream,			\
			.channels_min = 2,			\
			.channels_max = 2,			\
			.rates = SNDRV_PCM_RATE_48000,		\
			.formats = SNDRV_PCM_FMTBIT_S16_LE,	\
		},						\
		.ops = &uaif_ops,				\
	}

static struct snd_soc_dai_driver uaif_dais[] = {
	UAIF_DAI(PATH_HP, "s9p-abox-uaif0", "UAIF0 Playback"),
	UAIF_DAI(PATH_SPK, "s9p-abox-uaif1", "UAIF1 Playback"),
};

/* ------------------------------------------------------------------ */
/* card                                                                 */

static int hp_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_component *codec = codec_dai->component;
	int ret;

	ret = snd_soc_component_set_pll(codec, MADERA_FLL1_REFCLK, MADERA_FLL_SRC_MCLK1,
					MCLK1_HZ, SYSCLK_HZ);
	if (ret) {
		dev_err(rtd->dev, "FLL1 %u -> %u Hz: %d\n", MCLK1_HZ, SYSCLK_HZ, ret);
		return ret;
	}
	ret = snd_soc_component_set_sysclk(codec, MADERA_CLK_SYSCLK_1, MADERA_CLK_SRC_FLL1,
					   SYSCLK_HZ, SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(rtd->dev, "SYSCLK: %d\n", ret);
		return ret;
	}
	ret = snd_soc_dai_set_sysclk(codec_dai, MADERA_CLK_SYSCLK_1, 0, 0);
	/*
	 * The DAC digital volume goes up to +31.5 dB (191): at 100 % in the
	 * sound server it clips. Cap it at 0 dB (128).
	 */
	if (!ret)
		ret = snd_soc_limit_volume(rtd->card, "HPOUT2 Digital Volume", 128);
	dev_info(rtd->dev, "CS47L93: FLL1 from MCLK1 26 MHz, SYSCLK %u Hz, AIF1 on SYSCLK: %d\n",
		 SYSCLK_HZ, ret);
	return ret;
}

SND_SOC_DAILINK_DEFS(hp,
	DAILINK_COMP_ARRAY(COMP_CPU("s9p-abox-uaif0")),
	DAILINK_COMP_ARRAY(COMP_CODEC("cs47l92-codec", "cs47l92-aif1")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("s9p-abox-pcm")));

/* the amplifiers are matched by DT node (the I2C bus number changes) */
static struct snd_soc_dai_link_component amp_codecs[] = {
	{ .dai_name = "s9p-max98512-aif" },
	{ .dai_name = "s9p-max98512-aif" },
};

SND_SOC_DAILINK_DEFS(spk,
	DAILINK_COMP_ARRAY(COMP_CPU("s9p-abox-uaif1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("s9p-abox-pcm")));

static struct snd_soc_dai_link links[] = {
	{
		.name = "Headphones",
		.stream_name = "Headphones",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC,
		.init = hp_init,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(hp),
	},
	{
		.name = "Speaker",
		.stream_name = "Speaker",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(spk),
	},
};

static const struct snd_soc_dapm_widget card_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
};

static const struct snd_soc_dapm_route card_routes[] = {
	{ "Headphone", NULL, "HPOUT2L" },
	{ "Headphone", NULL, "HPOUT2R" },
};

static struct snd_soc_card card = {
	.name = "Galaxy S9+",
	.driver_name = "s9p-audio",
	.owner = THIS_MODULE,
	.dai_link = links,
	.num_links = ARRAY_SIZE(links),
	.dapm_widgets = card_widgets,
	.num_dapm_widgets = ARRAY_SIZE(card_widgets),
	.dapm_routes = card_routes,
	.num_dapm_routes = ARRAY_SIZE(card_routes),
};

static struct device_node *amp_np[2];

/* speaker link only if both amplifiers are in the DT (s9p-audio-board) */
static void setup_speaker_link(struct device *dev)
{
	amp_np[0] = of_find_node_by_path("/soc@0/i2c@108c0000/amp@39");
	amp_np[1] = of_find_node_by_path("/soc@0/i2c@108c0000/amp@3a");
	if (amp_np[0] && amp_np[1]) {
		amp_codecs[0].of_node = amp_np[0];
		amp_codecs[1].of_node = amp_np[1];
		links[PATH_SPK].codecs = amp_codecs;
		links[PATH_SPK].num_codecs = ARRAY_SIZE(amp_codecs);
		dev_info(dev, "speaker link: amplifiers %pOF, %pOF\n", amp_np[0], amp_np[1]);
	} else {
		card.num_links = 1;
		dev_info(dev, "no amplifier nodes: headphones only\n");
	}
}

static int s9p_abox_pcm_probe(struct platform_device *pdev)
{
	int ret;

	ret = devm_snd_soc_register_component(&pdev->dev, &abox_pcm_component, uaif_dais,
					      ARRAY_SIZE(uaif_dais));
	if (ret)
		return ret;
	ret = s9p_abox_set_ops(&abox_ops, NULL);
	if (ret)
		return ret;
	setup_speaker_link(&pdev->dev);
	card.dev = &pdev->dev;
	ret = devm_snd_soc_register_card(&pdev->dev, &card);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "card\n");
	return 0;
}

static void s9p_abox_pcm_remove(struct platform_device *pdev)
{
	s9p_abox_set_ops(NULL, NULL);
	of_node_put(amp_np[0]);
	of_node_put(amp_np[1]);
}

static struct platform_driver s9p_abox_pcm_driver = {
	.probe = s9p_abox_pcm_probe,
	.remove = s9p_abox_pcm_remove,
	.driver = { .name = "s9p-abox-pcm" },
};

static struct platform_device *pcm_pdev;

static int __init s9p_abox_pcm_init(void)
{
	int ret;

	if (!s9p_abox_ready())
		return -ENODEV;	/* load s9p-abox first (firmware booted) */
	ret = platform_driver_register(&s9p_abox_pcm_driver);
	if (ret)
		return ret;
	pcm_pdev = platform_device_register_simple("s9p-abox-pcm", -1, NULL, 0);
	if (IS_ERR(pcm_pdev)) {
		platform_driver_unregister(&s9p_abox_pcm_driver);
		return PTR_ERR(pcm_pdev);
	}
	return 0;
}

static void __exit s9p_abox_pcm_exit(void)
{
	platform_device_unregister(pcm_pdev);
	platform_driver_unregister(&s9p_abox_pcm_driver);
}

module_init(s9p_abox_pcm_init);
module_exit(s9p_abox_pcm_exit);
MODULE_DESCRIPTION("Galaxy S9+ ABOX PCM, UAIF0/1, headphone and speaker card");
MODULE_LICENSE("GPL");
