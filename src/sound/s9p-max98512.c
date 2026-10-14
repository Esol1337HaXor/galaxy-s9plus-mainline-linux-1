// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): Maxim MAX98512 speaker amplifier, revision A.
 *
 *   ABOX UAIF (I2S, bit/frame clock provider) -> amp@39 (Left, slot 0)
 *                                             -> amp@3a (Right, slot 1)
 *
 * One instance per amplifier: two nodes on i2c@108c0000 with compatible
 * "s9p,max98512", a sound-name-prefix ("Left"/"Right") and s9p,channel
 * (0 = left, 1 = right; without it 0x39 is left and 0x3a is right).
 *
 * Register map and init values: vendor sound/soc/codecs/max98512.{c,h}
 * (kernel 4.9), revision A addresses only (rev B is the one the vendor
 * remaps with max98512_remap_reg()); summary in audio/analisis/02-codec-amp.md,
 * section 4. The vendor runs the speakers behind the Maxim DSM (speaker
 * protection fed by the I/V sense ADC); there is no DSM here, so the I/V
 * feedback stays off, the TX side is high impedance and the analog gain is
 * kept low. No IRQ.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

/* MAX98512 rev A registers (vendor max98512.h) */
#define PCM_RX_EN_A		0x0018
#define PCM_TX_EN_A		0x001a
#define PCM_TX_EN_B		0x001b
#define PCM_TX_HIZ_CTRL_A	0x001c
#define PCM_TX_HIZ_CTRL_B	0x001d
#define PCM_MODE_CFG		0x0020
#define  PCM_BCLKEDGE		BIT(2)
#define  PCM_FORMAT_MASK	GENMASK(5, 3)
#define  PCM_FORMAT_I2S		(0 << 3)
#define  PCM_CHANSZ_MASK	GENMASK(7, 6)
#define  PCM_CHANSZ_16		(1 << 6)
#define  PCM_CHANSZ_32		(3 << 6)
#define PCM_MASTER_MODE		0x0021
#define  PCM_MASTER_MASK	GENMASK(1, 0)
#define  PCM_MASTER_SLAVE	0
#define PCM_CLK_SETUP		0x0022
#define  PCM_BSEL_MASK		GENMASK(3, 0)
#define PCM_SR_SETUP1		0x0023
#define  PCM_SR1_MASK		GENMASK(3, 0)
#define PCM_SR_SETUP2		0x0024
#define  PCM_SR2_SR_MASK	GENMASK(7, 4)
#define  PCM_SR2_IVADC_MASK	GENMASK(3, 0)
#define  PCM_SR_44100		0x7
#define  PCM_SR_48000		0x8
#define PCM_TO_SPK_MONOMIX_A	0x0025
#define PCM_TO_SPK_MONOMIX_B	0x0026
#define PDM_RX_CTRL		0x0034
#define AMP_VOL_CTRL		0x0035
#define  AMP_VOL_0DB		0x40
#define AMP_DSP_CFG		0x0036
#define  AMP_DSP_DCBLK_EN	BIT(0)
#define AMP_EN			0x0038
#define  AMP_EN_DEM_EN		BIT(7)	/* speaker */
#define  AMP_EN_DEM_OFF_TRIM	BIT(6)	/* receiver (earpiece) */
#define  AMP_EN_EN		BIT(0)
#define SPK_SRC_SEL		0x0039
#define  SPK_SRC_MASK		GENMASK(1, 0)
#define  SPK_SRC_DIGITAL	0
#define SPK_GAIN		0x003a
#define MEAS_EN			0x003c
#define  MEAS_VI_EN		GENMASK(1, 0)
#define MEAS_DSP_CFG		0x003d
#define BOOST_CTRL0		0x003e
#define BOOST_CTRL3		0x003f
#define BOOST_CTRL1		0x0040
#define MEAS_ADC_CFG		0x0041
#define MEAS_ADC_BASE_MSB	0x0042
#define MEAS_ADC_BASE_LSB	0x0043
#define ADC_CH_DIVIDE(n)	(0x0044 + (n))
#define ADC_CH_FILT_CFG(n)	(0x0047 + (n))
#define SQUELCH			0x004e
#define BROWNOUT_EN		0x0050
#define BROWNOUT_LVL_HOLD	0x0053
#define BROWNOUT_LVL_THRESH(n)	(0x0057 + (n))	/* n = 1..4 */
#define BROWNOUT_HYSTERESIS	0x005c
#define BROWNOUT_LIM_ATK_REL	0x005d
#define BROWNOUT_CUR_LIMIT(n)	(0x0070 + 4 * ((n) - 1))
#define BROWNOUT_AMP1_CTRL(n, i) (0x0070 + 4 * ((n) - 1) + (i))
#define ENV_TRACK_VOUT_HEADROOM	0x0080
#define ENV_TRACK_CTRL		0x0084
#define ENV_TRACK_BOOST_VOUT_RD	0x0085
#define BOOST_BYPASS_1		0x0086
#define BOOST_BYPASS_2		0x0087
#define BOOST_BYPASS_3		0x0088
#define CLK_MON			0x0011
#define GLOBAL_SHDN		0x0400
#define  GLOBAL_EN		BIT(0)
#define SOFT_RESET		0x0401
#define REV_ID			0x0402
#define  REV_ID_A_MIN		0x40	/* below: rev B, whose ID is at 0x0600 */

/*
 * SPK_GAIN bits[2:0] (PCM path): 0 = mute, n = +3n dB. The vendor DT and
 * driver use 0x05 ("+15db for PCM") with DSM protection on top; its TLV
 * (300, 300) is off by one step. Without DSM: +3 dB by default and +9 dB at
 * most from userspace.
 */
#define SPK_GAIN_DEFAULT	0x01
#define SPK_GAIN_SAFE_MAX	0x03

#define WAKE_TRIES		5

struct s9p_max {
	struct device *dev;
	struct regmap *regmap;
	unsigned int channel;	/* 0 = left (slot 0), 1 = right (slot 1) */
};

/*
 * amp@39 sometimes NACKs the first access after a while idle and answers
 * the next one: read REV_ID until it does.
 */
static int s9p_max_wake(struct s9p_max *m, unsigned int *rev)
{
	unsigned int val;
	int i, ret;

	for (i = 0; i < WAKE_TRIES; i++) {
		ret = regmap_read(m->regmap, REV_ID, &val);
		if (!ret) {
			if (rev)
				*rev = val;
			return 0;
		}
		usleep_range(2000, 3000);
	}
	return ret;
}

/* max98512_probe(), rev A, without the DSM / I-V sense part */
static const struct reg_sequence init_seq[] = {
	{ MEAS_EN, 0x00 },		/* I/V feedback off */
	{ PCM_TX_EN_A, 0x00 },		/* no I/V slots (max98512_reinit_reg) */
	{ PCM_TX_EN_B, 0x00 },
	{ PCM_TX_HIZ_CTRL_A, 0xff },
	{ PCM_TX_HIZ_CTRL_B, 0xff },
	{ AMP_VOL_CTRL, AMP_VOL_0DB },	/* digital volume 0 dB, at the input */
	{ SPK_GAIN, SPK_GAIN_DEFAULT },
	{ AMP_DSP_CFG, AMP_DSP_DCBLK_EN },	/* DC blocker, ramp on */
	{ MEAS_DSP_CFG, 0xf7 },
	{ BOOST_CTRL0, 0x1c },		/* boost output voltage */
	{ BOOST_CTRL1, 0x3e },		/* boost current limit */
	{ BOOST_CTRL3, 0x01 },
	{ MEAS_ADC_CFG, 0x07 },		/* measurement ADC (thermal, battery) */
	{ MEAS_ADC_BASE_MSB, 0x00 },
	{ MEAS_ADC_BASE_LSB, 0x0b },
	{ ADC_CH_DIVIDE(0), 0x02 },
	{ ADC_CH_DIVIDE(1), 0x02 },
	{ ADC_CH_DIVIDE(2), 0x02 },
	{ ADC_CH_FILT_CFG(0), 0x0c },
	{ ADC_CH_FILT_CFG(1), 0x0c },
	{ ADC_CH_FILT_CFG(2), 0x0c },
	{ SQUELCH, 0x10 },
	{ BROWNOUT_EN, 0x07 },
	{ BROWNOUT_LVL_HOLD, 0xff },
	/*
	 * Levels 2/3: the vendor writes 0x30/0x10 here and, on each speaker
	 * enable, 0x40/0x20 if the battery is under 50 C. There is no battery
	 * temperature here: keep the probe values.
	 */
	{ BROWNOUT_LVL_THRESH(1), 0xa0 },
	{ BROWNOUT_LVL_THRESH(2), 0x30 },
	{ BROWNOUT_LVL_THRESH(3), 0x10 },
	{ BROWNOUT_HYSTERESIS, 0x0f },
	{ BROWNOUT_LIM_ATK_REL, 0x76 },
	/* BROWNOUT_CUR_LIMIT(1..3) depend on the channel, written apart */
	{ BROWNOUT_AMP1_CTRL(1, 1), 0x06 },
	{ BROWNOUT_AMP1_CTRL(1, 2), 0x00 },
	{ BROWNOUT_AMP1_CTRL(1, 3), 0x00 },
	{ BROWNOUT_AMP1_CTRL(2, 1), 0x09 },
	{ BROWNOUT_AMP1_CTRL(2, 2), 0x00 },
	{ BROWNOUT_AMP1_CTRL(2, 3), 0x00 },
	{ BROWNOUT_AMP1_CTRL(3, 1), 0x0c },
	{ BROWNOUT_AMP1_CTRL(3, 2), 0x00 },
	{ BROWNOUT_AMP1_CTRL(3, 3), 0x00 },
	{ BROWNOUT_CUR_LIMIT(4), 0x00 },
	{ BROWNOUT_AMP1_CTRL(4, 1), 0x00 },
	{ ENV_TRACK_VOUT_HEADROOM, 0x0a },	/* envelope tracking */
	{ ENV_TRACK_CTRL, 0x01 },
	{ ENV_TRACK_BOOST_VOUT_RD, 0x10 },
	{ BOOST_BYPASS_1, 0x05 },
	{ BOOST_BYPASS_2, 0x02 },	/* rev A default: bypass off */
	{ BOOST_BYPASS_3, 0x01 },
	{ CLK_MON, 0x00 },
};

/*
 * Stereo routing, as the vendor does with mono_stereo set (max98512_probe
 * and __max98512_spk_enable). MONOMIX_A bits[7:6] pick the speaker source
 * (0 = channel 0, 1 = channel 1, 2 = (ch0 + ch1) / 2, the vendor mono mode
 * 0x80/0x01), bits[3:0] the slot of channel 0; MONOMIX_B the slot of
 * channel 1. Left: channel 0 from slot 0. Right: channel 1 from slot 1.
 */
static const struct {
	u8 mix_a, mix_b, rx_en;
} chan_cfg[2] = {
	{ 0x00, 0x00, BIT(0) },
	{ 0x41, 0x01, BIT(1) },
};

/* BROWNOUT_LVLn_CUR_LIMIT: vendor defaults, the S9+ DT does not set them */
static const u8 cur_limit[2] = { 0x16, 0x3e };

static int s9p_max_hw_init(struct s9p_max *m)
{
	int ret, n;

	/* the reset write may not be acked: the chip resets on it */
	regmap_write(m->regmap, SOFT_RESET, 1);
	usleep_range(10000, 11000);
	ret = s9p_max_wake(m, NULL);
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(m->regmap, init_seq, ARRAY_SIZE(init_seq));
	if (ret)
		return ret;
	for (n = 1; n <= 3; n++) {
		ret = regmap_write(m->regmap, BROWNOUT_CUR_LIMIT(n),
				   cur_limit[m->channel]);
		if (ret)
			return ret;
	}
	ret = regmap_write(m->regmap, PCM_TO_SPK_MONOMIX_A,
			   chan_cfg[m->channel].mix_a);
	if (!ret)
		ret = regmap_write(m->regmap, PCM_TO_SPK_MONOMIX_B,
				   chan_cfg[m->channel].mix_b);
	return ret;
}

/* ---- DAI ---- */

static int s9p_max_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct s9p_max *m = snd_soc_dai_get_drvdata(dai);
	int ret;

	/* ABOX drives BCLK/LRCLK: the amp is a consumer */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S) {
		dev_err(m->dev, "unsupported DAI format 0x%x\n", fmt);
		return -EINVAL;
	}

	ret = s9p_max_wake(m, NULL);
	if (ret)
		return ret;
	ret = regmap_update_bits(m->regmap, PCM_MASTER_MODE, PCM_MASTER_MASK,
				 PCM_MASTER_SLAVE);
	if (!ret)
		ret = regmap_update_bits(m->regmap, PCM_MODE_CFG,
					 PCM_BCLKEDGE | PCM_FORMAT_MASK,
					 PCM_FORMAT_I2S);
	return ret;
}

static int s9p_max_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct s9p_max *m = snd_soc_dai_get_drvdata(dai);
	unsigned int chansz, sr, bsel, width;
	int ret;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		chansz = PCM_CHANSZ_16;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		chansz = PCM_CHANSZ_32;
		break;
	default:
		return -EINVAL;
	}

	switch (params_rate(params)) {
	case 44100:
		sr = PCM_SR_44100;
		break;
	case 48000:
		sr = PCM_SR_48000;
		break;
	default:
		return -EINVAL;
	}

	/* I2S frame: always two slots of the sample width (16 bit: 32 fs) */
	width = params_width(params);
	switch (2 * width) {
	case 32:
		bsel = 2;
		break;
	case 64:
		bsel = 4;
		break;
	default:
		return -EINVAL;
	}

	ret = s9p_max_wake(m, NULL);
	if (ret)
		return ret;

	ret = regmap_update_bits(m->regmap, PCM_MASTER_MODE, PCM_MASTER_MASK,
				 PCM_MASTER_SLAVE);
	if (!ret)
		ret = regmap_update_bits(m->regmap, PCM_MODE_CFG,
					 PCM_CHANSZ_MASK, chansz);
	if (!ret)
		ret = regmap_update_bits(m->regmap, PCM_CLK_SETUP,
					 PCM_BSEL_MASK, bsel);
	if (!ret)
		ret = regmap_update_bits(m->regmap, PCM_SR_SETUP1,
					 PCM_SR1_MASK, sr);
	/* IV ADC rate = speaker rate, as the vendor without interleave */
	if (!ret)
		ret = regmap_update_bits(m->regmap, PCM_SR_SETUP2,
					 PCM_SR2_SR_MASK | PCM_SR2_IVADC_MASK,
					 sr << 4 | sr);
	if (!ret)
		ret = regmap_write(m->regmap, PCM_RX_EN_A,
				   chan_cfg[m->channel].rx_en);
	if (!ret)
		ret = regmap_update_bits(m->regmap, SPK_SRC_SEL, SPK_SRC_MASK,
					 SPK_SRC_DIGITAL);
	if (!ret)
		ret = regmap_write(m->regmap, PDM_RX_CTRL, 0);
	if (ret)
		dev_err(m->dev, "hw_params: %d\n", ret);
	return ret;
}

static const struct snd_soc_dai_ops s9p_max_dai_ops = {
	.set_fmt	= s9p_max_set_fmt,
	.hw_params	= s9p_max_hw_params,
};

static struct snd_soc_dai_driver s9p_max_dai = {
	.name = "s9p-max98512-aif",
	.playback = {
		.stream_name	= "HiFi Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &s9p_max_dai_ops,
};

/* ---- DAPM ---- */

/*
 * __max98512_spk_enable() / max98512_spk_enable(0) for one speaker
 * amplifier: AMP_EN with the speaker DEM, then GLOBAL_EN; off in the
 * reverse order, I/V sense off, and the vendor 15 ms wait.
 */
static int s9p_max_amp_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *c = snd_soc_dapm_to_component(w->dapm);
	struct s9p_max *m = snd_soc_component_get_drvdata(c);
	int ret;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = s9p_max_wake(m, NULL);
		if (!ret)
			ret = regmap_update_bits(m->regmap, MEAS_EN, MEAS_VI_EN, 0);
		if (!ret)
			ret = regmap_update_bits(m->regmap, AMP_EN, AMP_EN_EN,
						 AMP_EN_EN);
		if (!ret)
			ret = regmap_update_bits(m->regmap, AMP_EN,
						 AMP_EN_DEM_EN | AMP_EN_DEM_OFF_TRIM,
						 AMP_EN_DEM_EN);
		if (!ret)
			ret = regmap_update_bits(m->regmap, GLOBAL_SHDN,
						 GLOBAL_EN, GLOBAL_EN);
		if (ret)
			dev_err(m->dev, "amp on: %d\n", ret);
		return ret;
	case SND_SOC_DAPM_PRE_PMD:
		s9p_max_wake(m, NULL);
		regmap_update_bits(m->regmap, GLOBAL_SHDN, GLOBAL_EN, 0);
		regmap_update_bits(m->regmap, AMP_EN, AMP_EN_EN, 0);
		regmap_update_bits(m->regmap, MEAS_EN, MEAS_VI_EN, 0);
		usleep_range(15000, 16000);
		return 0;
	default:
		return 0;
	}
}

static const struct snd_kcontrol_new s9p_max_amp_switch =
	SOC_DAPM_SINGLE_VIRT("Switch", 1);

static const struct snd_soc_dapm_widget s9p_max_widgets[] = {
	SND_SOC_DAPM_DAC_E("Amp Enable", "HiFi Playback", SND_SOC_NOPM, 0, 0,
			   s9p_max_amp_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SWITCH("Amp", SND_SOC_NOPM, 0, 0, &s9p_max_amp_switch),
	SND_SOC_DAPM_OUTPUT("Speaker"),
};

static const struct snd_soc_dapm_route s9p_max_routes[] = {
	{ "Amp", "Switch", "Amp Enable" },
	{ "Speaker", NULL, "Amp" },
};

/* ---- controls ---- */

/* AMP_VOL_CTRL bits[6:0]: -16 dB + 0.25 dB * n; capped at 0x40 = 0 dB */
static const DECLARE_TLV_DB_SCALE(s9p_max_digital_tlv, -1600, 25, 0);
/* SPK_GAIN bits[2:0]: 0 = mute, +3 dB * n */
static const DECLARE_TLV_DB_SCALE(s9p_max_spk_tlv, 0, 300, 1);

static const struct snd_kcontrol_new s9p_max_controls[] = {
	SOC_SINGLE_TLV("Digital Volume", AMP_VOL_CTRL, 0, AMP_VOL_0DB, 0,
		       s9p_max_digital_tlv),
	SOC_SINGLE_TLV("Speaker Volume", SPK_GAIN, 0, SPK_GAIN_SAFE_MAX, 0,
		       s9p_max_spk_tlv),
};

static const struct snd_soc_component_driver s9p_max_component = {
	.controls		= s9p_max_controls,
	.num_controls		= ARRAY_SIZE(s9p_max_controls),
	.dapm_widgets		= s9p_max_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(s9p_max_widgets),
	.dapm_routes		= s9p_max_routes,
	.num_dapm_routes	= ARRAY_SIZE(s9p_max_routes),
	.endianness		= 1,
};

/* ---- regmap ---- */

static bool s9p_max_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x0001 ... 0x0028:
	case 0x002b ... 0x004c:
	case 0x004e ... 0x0053:
	case 0x0058 ... 0x005f:
	case 0x0070 ... 0x008d:
	case GLOBAL_SHDN ... REV_ID:
		return true;
	default:
		return false;
	}
}

static bool s9p_max_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x0001 ... 0x0009:		/* interrupt raw/state/flag */
	case 0x000d ... 0x000f:		/* flag clear */
	case 0x0013:			/* watchdog reset */
	case 0x004a ... 0x004c:		/* measurement ADC readback */
	case 0x004f:			/* brownout status */
	case 0x0052:			/* brownout hold clear */
	case ENV_TRACK_BOOST_VOUT_RD:
	case GLOBAL_SHDN:		/* cleared by the chip on a fault */
	case SOFT_RESET:
	case REV_ID:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config s9p_max_regmap = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= REV_ID,
	.readable_reg	= s9p_max_readable,
	.volatile_reg	= s9p_max_volatile,
	.cache_type	= REGCACHE_MAPLE,
};

/* ---- I2C ---- */

static int s9p_max_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct s9p_max *m;
	unsigned int rev;
	int ret;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = dev;
	i2c_set_clientdata(i2c, m);

	if (of_property_read_u32(dev->of_node, "s9p,channel", &m->channel))
		m->channel = i2c->addr == 0x3a ? 1 : 0;
	if (m->channel > 1)
		return dev_err_probe(dev, -EINVAL, "bad s9p,channel %u\n",
				     m->channel);

	m->regmap = devm_regmap_init_i2c(i2c, &s9p_max_regmap);
	if (IS_ERR(m->regmap))
		return dev_err_probe(dev, PTR_ERR(m->regmap), "regmap\n");

	ret = s9p_max_wake(m, &rev);
	if (ret)
		return dev_err_probe(dev, ret, "no answer at 0x%02x\n", i2c->addr);
	if (rev < REV_ID_A_MIN)
		return dev_err_probe(dev, -ENODEV,
				     "REV_ID 0x%02x: not a MAX98512 rev A\n", rev);

	ret = s9p_max_hw_init(m);
	if (ret)
		return dev_err_probe(dev, ret, "init\n");

	dev_info(dev, "MAX98512 rev A (0x%02x), %s channel\n", rev,
		 m->channel ? "right" : "left");

	return devm_snd_soc_register_component(dev, &s9p_max_component,
					       &s9p_max_dai, 1);
}

static const struct of_device_id s9p_max_of_match[] = {
	{ .compatible = "s9p,max98512" },
	{ }
};
MODULE_DEVICE_TABLE(of, s9p_max_of_match);

static struct i2c_driver s9p_max_driver = {
	.driver = {
		.name = "s9p-max98512",
		.of_match_table = s9p_max_of_match,
	},
	.probe = s9p_max_probe,
};
module_i2c_driver(s9p_max_driver);

MODULE_DESCRIPTION("Galaxy S9+ MAX98512 rev A speaker amplifier");
MODULE_LICENSE("GPL");
