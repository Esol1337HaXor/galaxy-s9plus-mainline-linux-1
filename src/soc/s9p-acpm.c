// SPDX-License-Identifier: GPL-2.0
/*
 * ACPM (APM firmware) mailbox access for the Exynos 9810.
 *
 * The S2MPS18 main PMIC hangs off the APM's "speedy" bus, so the AP reaches
 * its registers only by asking the APM firmware: a 4-word command goes into a
 * shared-SRAM ring queue, the mailbox at 0x14100000 raises the APM's
 * interrupt, the answer comes back in the mirror queue. This is the vendor
 * kernel's acpm_ipc.c / acpm_mfd.c (Samsung 4.9) protocol, reduced to what
 * this port needs: channel 2, the MFD channel, functions read/write/update
 * on the PMIC block. The sequence numbers follow the vendor layout so the
 * userspace s9p-acpm tool and this driver can share the channel.
 *
 * Users of this module: s9p-cpufreq (BUCK2/BUCK3) and s9p-g3d (BUCK6).
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define MBOX_PHYS		0x14100000
#define SRAM_PHYS		0x02039000
#define SRAM_SIZE		0x30000
/* The first three SRAM pages fault on access; everything this driver
 * needs lies above 0x3000, so the mapping starts there. */
#define SRAM_MAP_OFF		0x3000
#define INITDATA		0x7f00

#define INTGR0			0x08
#define INTCR1			0x20
#define INTSR1			0x28
#define SEQ_SHIFT		16

#define MFD_CHANNEL		2
#define ACPM_TYPE_PMIC		1

#define FUNC_READ		0
#define FUNC_WRITE		1
#define FUNC_UPDATE		2

/* struct acpm_framework, u32 field indices */
#define FW_IPC_CHANNELS		2	/* ipc_channels */
#define FW_IPC_AP_MAX		6	/* ipc_ap_max */

/*
 * struct ipc_channel: id, field, owner, type, then channel_info
 * { rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base,
 *   rx_indr_buf_size, tx_rear, tx_front, tx_base, q_len, q_elem_size,
 *   credit, mbox_addr }, ap_poll. Every rear/front/base word is the SRAM
 * offset of the pointer or ring, not the pointer itself.
 *
 * The rx_/tx_ names are the APM's: the AP TRANSMITS into the APM's rx ring
 * (fields 4..6, on this device the ring at 0x82f0) and RECEIVES from the
 * APM's tx ring (fields 10..12, the ring at 0x8290). Measured: the APM
 * consumes commands from 0x82f0 and only from there. An earlier revision
 * had the two swapped -- its commands landed in the reply ring, the ring
 * filled up after four of them, and the APM, spinning on its own copy of
 * the front index, never answered anything again until the next reboot
 * (every BUCK write timed out, so both clusters stayed at their lowest
 * step and the touch rails were never cycled).
 */
#define CH_SIZE			(18 * 4)
#define CH_ID			0
#define CH_TX_REAR		4	/* AP -> APM ring (named rx_ on the APM) */
#define CH_TX_FRONT		5
#define CH_TX_BASE		6
#define CH_RX_REAR		10	/* APM -> AP ring (named tx_ on the APM) */
#define CH_RX_FRONT		11
#define CH_RX_BASE		12
#define CH_Q_LEN		13
#define CH_Q_ELEM_SIZE		14

/* the PMIC sits on a slow speedy bus behind the APM; the userspace tool
 * waits 500 ms per attempt and has never needed more */
#define REPLY_TIMEOUT_MS	600
#define XFER_RETRIES		3

static void __iomem *mbox;
static void __iomem *sram;	/* SRAM_PHYS + SRAM_MAP_OFF */

struct s9p_ch {
	u32 tx_rear, tx_front, tx_base;
	u32 rx_rear, rx_front, rx_base;
	u32 q_len, q_elem_size;
	u8 id;
};

static struct s9p_ch mfd_ch;
static DEFINE_MUTEX(s9p_acpm_lock);
static u32 seq = 1;
static bool acpm_ready;

static inline u32 sram_read(u32 off)
{
	return readl_relaxed(sram + (off - SRAM_MAP_OFF));
}

static inline void sram_write(u32 off, u32 v)
{
	writel_relaxed(v, sram + (off - SRAM_MAP_OFF));
}

static int s9p_find_channel(u8 id, struct s9p_ch *ch)
{
	u32 fw_ipc_channels, ap_max, table;
	u32 i;

	fw_ipc_channels = sram_read(INITDATA + FW_IPC_CHANNELS * 4);
	ap_max = sram_read(INITDATA + FW_IPC_AP_MAX * 4);
	table = fw_ipc_channels;

	for (i = 0; i < ap_max; i++) {
		u32 base = table + i * CH_SIZE;

		if ((sram_read(base + CH_ID * 4) & 0xff) == id) {
			ch->id = id;
			ch->tx_rear = sram_read(base + CH_TX_REAR * 4);
			ch->tx_front = sram_read(base + CH_TX_FRONT * 4);
			ch->tx_base = sram_read(base + CH_TX_BASE * 4);
			ch->rx_rear = sram_read(base + CH_RX_REAR * 4);
			ch->rx_front = sram_read(base + CH_RX_FRONT * 4);
			ch->rx_base = sram_read(base + CH_RX_BASE * 4);
			ch->q_len = sram_read(base + CH_Q_LEN * 4);
			ch->q_elem_size = sram_read(base + CH_Q_ELEM_SIZE * 4);
			return 0;
		}
	}
	return -ENODEV;
}

/*
 * Consume every entry of the reply ring and acknowledge the mailbox.
 * Transactions are serialised by s9p_acpm_lock, so anything in the ring
 * that is not the reply being waited for is stale (a reply to a timed-out
 * command, or one meant for the userspace tool). A reply ring that fills
 * up wedges the APM for the rest of the boot (see the channel comment), so
 * it is never left non-empty.
 */
static void s9p_consume_rx(u32 want_seq, u32 *out, bool *found)
{
	u32 front = sram_read(mfd_ch.rx_front);
	u32 rear = sram_read(mfd_ch.rx_rear);
	u32 i, k;

	for (i = rear; i != front; i = (i + 1) % mfd_ch.q_len) {
		u32 w0 = sram_read(mfd_ch.rx_base + mfd_ch.q_elem_size * i);

		if (out && ((w0 >> SEQ_SHIFT) & 0x3f) == want_seq) {
			for (k = 0; k < 4; k++)
				out[k] = sram_read(mfd_ch.rx_base +
						   mfd_ch.q_elem_size * i + 4 * k);
			*found = true;
		}
	}
	if (rear != front)
		sram_write(mfd_ch.rx_rear, front);
	writel_relaxed(1 << mfd_ch.id, mbox + INTCR1);
}

static int s9p_send(const u32 *cmd)
{
	u32 rear, front, nxt, i;
	ktime_t t0 = ktime_get();

	front = sram_read(mfd_ch.tx_front);
	nxt = (front + 1) % mfd_ch.q_len;
	while (nxt == (rear = sram_read(mfd_ch.tx_rear))) {
		if (ktime_ms_delta(ktime_get(), t0) > 20)
			return -EBUSY;	/* the APM is not draining commands */
		usleep_range(50, 100);
	}
	for (i = 0; i < 4; i++)
		sram_write(mfd_ch.tx_base + mfd_ch.q_elem_size * front + 4 * i,
			   cmd[i]);
	/* the ring words must be visible before the front index moves and
	 * the APM is told about it */
	wmb();
	sram_write(mfd_ch.tx_front, nxt);
	wmb();
	writel_relaxed((1 << mfd_ch.id) << 16, mbox + INTGR0);
	return 0;
}

static int s9p_receive(u32 want_seq, u32 *out, unsigned int timeout_ms)
{
	ktime_t t0 = ktime_get();
	bool found = false;

	while (ktime_ms_delta(ktime_get(), t0) < timeout_ms) {
		bool pending = readl_relaxed(mbox + INTSR1) & (1 << mfd_ch.id);

		/* a reply that landed between a previous ring read and the
		 * interrupt acknowledge has no interrupt left: look at the
		 * ring itself as well */
		if (!pending &&
		    sram_read(mfd_ch.rx_rear) == sram_read(mfd_ch.rx_front)) {
			usleep_range(50, 100);
			continue;
		}
		s9p_consume_rx(want_seq, out, &found);
		if (found)
			return 0;
	}
	return -ETIMEDOUT;
}

static int s9p_acpm_mfd_xfer(u8 func, u8 reg, u8 val, u8 mask, u8 *retval)
{
	u32 cmd[4], reply[4];
	int ret = -ENODEV, retry;

	if (!mbox)
		return -ENODEV;

	mutex_lock(&s9p_acpm_lock);
	cmd[0] = ((ACPM_TYPE_PMIC & 0xf) << 8) | (reg & 0xff);
	cmd[1] = (func & 0xff) | ((val & 0xff) << 8) | ((mask & 0xff) << 16);
	cmd[2] = 0;
	cmd[3] = 0;

	for (retry = 0; retry < XFER_RETRIES; retry++) {
		bool stale = false;

		/* never send into a ring with stale replies queued */
		s9p_consume_rx(0, NULL, &stale);

		seq = seq % 63 + 1;
		cmd[0] &= ~(0x3f << SEQ_SHIFT);
		cmd[0] |= (seq & 0x3f) << SEQ_SHIFT;
		ret = s9p_send(cmd);
		if (ret) {
			usleep_range(1000, 2000);
			continue;
		}
		ret = s9p_receive(seq, reply, REPLY_TIMEOUT_MS);
		if (ret == 0)
			break;
	}
	mutex_unlock(&s9p_acpm_lock);
	if (ret)
		return ret;

	acpm_ready = true;
	if (retval && func == FUNC_READ)
		*retval = (reply[1] >> 8) & 0xff;
	return (reply[1] >> 24) & 0xff;	/* firmware return code, 0 = ok */
}

int s9p_acpm_pmic_write(u8 reg, u8 val)
{
	return s9p_acpm_mfd_xfer(FUNC_WRITE, reg, val, 0, NULL);
}
EXPORT_SYMBOL_GPL(s9p_acpm_pmic_write);

int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask)
{
	return s9p_acpm_mfd_xfer(FUNC_UPDATE, reg, val, mask, NULL);
}
EXPORT_SYMBOL_GPL(s9p_acpm_pmic_update);

int s9p_acpm_pmic_read(u8 reg, u8 *val)
{
	return s9p_acpm_mfd_xfer(FUNC_READ, reg, 0, 0, val);
}
EXPORT_SYMBOL_GPL(s9p_acpm_pmic_read);

bool s9p_acpm_is_ready(void)
{
	return acpm_ready;
}
EXPORT_SYMBOL_GPL(s9p_acpm_is_ready);

/*
 * Touch LDOs (LDO35 tsp_io, LDO43 tsp_avdd) of the S2MPS18. The previous
 * kernel power-cycles both rails at handover, and the controller stays
 * dead until it sees a power-on reset -- so the touch controller's I2C
 * probe would time out for ever unless the rails are cycled and the
 * driver probed again afterwards.
 *
 * Sequence (the validated userspace s9p-touch-power one): rails off for
 * two seconds, rails on, then the controller needs a few seconds before
 * it acknowledges its address -- so the reprobe is retried every two
 * seconds for a while, and if the controller still does not bind, the
 * whole cycle is repeated (three times at most). A driver that is already
 * bound is the proof the rails are fine, nothing is touched then.
 */
#define S2MPS18_L35CTRL		0x6c
#define S2MPS18_L43CTRL		0x74
#define LDO_ON_MODE		0xc0
#define TOUCH_LDO_OFF_MS	2000
#define TOUCH_APM_TRIES		30	/* one per second until the APM answers */
#define TOUCH_PROBES_PER_CYCLE	12	/* every 2 s: 24 s per LDO cycle */
#define TOUCH_MAX_CYCLES	3

static void s9p_touch_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(s9p_touch_dwork, s9p_touch_work);

static int s9p_touch_ldo_power_cycle(void)
{
	u8 v = 0;
	int ret;

	ret = s9p_acpm_pmic_update(S2MPS18_L35CTRL, 0, LDO_ON_MODE);
	if (ret)
		return ret;
	ret = s9p_acpm_pmic_update(S2MPS18_L43CTRL, 0, LDO_ON_MODE);
	if (ret)
		return ret;
	msleep(TOUCH_LDO_OFF_MS);
	ret = s9p_acpm_pmic_update(S2MPS18_L35CTRL, LDO_ON_MODE, LDO_ON_MODE);
	if (ret)
		return ret;
	ret = s9p_acpm_pmic_update(S2MPS18_L43CTRL, LDO_ON_MODE, LDO_ON_MODE);
	if (ret)
		return ret;
	s9p_acpm_pmic_read(S2MPS18_L35CTRL, &v);
	pr_info("s9p-acpm: touch LDOs power-cycled (L35=%02x)\n", v);
	return 0;
}

static int s9p_touch_match(struct device *dev, const void *data)
{
	return dev->of_node &&
	       of_device_is_compatible(dev->of_node, "samsung,s6sy761");
}

static void s9p_touch_work(struct work_struct *work)
{
	static unsigned int apm_tries, cycles, probes;
	struct device *dev;
	unsigned long next = 2 * HZ;
	int ret;

	dev = bus_find_device(&i2c_bus_type, NULL, NULL, s9p_touch_match);
	if (dev && dev->driver) {
		pr_info("s9p-acpm: touch controller alive after %u LDO cycle(s)\n",
			cycles);
		goto out;
	}

	if (cycles == 0 || probes >= TOUCH_PROBES_PER_CYCLE) {
		if (cycles >= TOUCH_MAX_CYCLES) {
			pr_err("s9p-acpm: touch controller never bound, giving up\n");
			goto out;
		}
		ret = s9p_touch_ldo_power_cycle();
		if (ret) {
			/* the APM is not up yet, or busy: once a second */
			if (++apm_tries >= TOUCH_APM_TRIES) {
				pr_err("s9p-acpm: touch LDO cycle gave up (%d)\n",
				       ret);
				goto out;
			}
			next = HZ;
			goto again;
		}
		cycles++;
		probes = 0;
		/* the controller needs a moment after power-on */
		next = HZ;
		goto again;
	}

	probes++;
	if (dev) {
		ret = device_reprobe(dev);
		if (ret)
			pr_info("s9p-acpm: touch reprobe %u: %d\n", probes, ret);
	}

again:
	schedule_delayed_work(&s9p_touch_dwork, next);
out:
	if (dev)
		put_device(dev);
}

/*
 * Park the APM before the SoC is reset or powered off.
 *
 * The vendor kernel never resets the SoC with the APM running:
 * exynos_reboot() and exynos_power_off() both call exynos_acpm_reboot()
 * first, which sends the framework's STOP command (bit 22 of word 0) on the
 * ACPM node's own channel (acpm-ipc-channel = <4> in the stock DT) and waits
 * for the answer. The firmware then finishes what it was doing -- a speedy
 * transfer to the S2MPS18, a DVFS step -- and sits in WFI, so the reset
 * cannot land in the middle of a PMIC transaction.
 *
 * Runs from the restart/power-off chain, with interrupts off: no sleeping,
 * no mutex wait. A transfer of our own still in flight on the MFD channel
 * is simply waited out by the APM before it reads this command. Whatever
 * happens, the next handler (syscon-reboot, 192) still runs.
 */
#define FW_CHANNEL		4
#define ACPM_IPC_STOP		BIT(22)
#define STOP_TIMEOUT_US		50000

static int s9p_acpm_park(struct sys_off_data *data)
{
	struct s9p_ch ch;
	u32 front, nxt, w0, i;
	int t;

	if (!mbox || s9p_find_channel(FW_CHANNEL, &ch) || ch.q_len < 2 ||
	    ch.q_elem_size < 16)
		return NOTIFY_DONE;

	front = sram_read(ch.tx_front);
	nxt = (front + 1) % ch.q_len;
	if (nxt == sram_read(ch.tx_rear)) {
		pr_emerg("s9p-acpm: APM command ring full, resetting anyway\n");
		return NOTIFY_DONE;
	}
	/* drop stale replies, so the one we wait for is the only one */
	sram_write(ch.rx_rear, sram_read(ch.rx_front));
	writel_relaxed(1 << ch.id, mbox + INTCR1);

	w0 = ACPM_IPC_STOP | (0x3f << SEQ_SHIFT);
	sram_write(ch.tx_base + ch.q_elem_size * front, w0);
	for (i = 1; i < 4; i++)
		sram_write(ch.tx_base + ch.q_elem_size * front + 4 * i, 0);
	wmb();
	sram_write(ch.tx_front, nxt);
	wmb();
	writel_relaxed((1 << ch.id) << 16, mbox + INTGR0);

	for (t = 0; t < STOP_TIMEOUT_US; t += 10) {
		if (sram_read(ch.rx_rear) != sram_read(ch.rx_front)) {
			pr_emerg("s9p-acpm: APM parked after %d us\n", t);
			return NOTIFY_DONE;
		}
		udelay(10);
	}
	pr_emerg("s9p-acpm: APM did not answer STOP, resetting anyway\n");
	return NOTIFY_DONE;
}

static int __init s9p_acpm_init(void)
{
	int ret;

	mbox = ioremap(MBOX_PHYS, 0x1000);
	if (!mbox)
		return -ENOMEM;
	sram = ioremap(SRAM_PHYS + SRAM_MAP_OFF, SRAM_SIZE - SRAM_MAP_OFF);
	if (!sram) {
		iounmap(mbox);
		mbox = NULL;
		return -ENOMEM;
	}

	ret = s9p_find_channel(MFD_CHANNEL, &mfd_ch);
	if (ret || mfd_ch.q_len < 2 || mfd_ch.q_elem_size < 16) {
		pr_err("s9p-acpm: no usable MFD channel %d in APM table\n",
		       MFD_CHANNEL);
		iounmap(sram);
		iounmap(mbox);
		mbox = NULL;
		return ret ? ret : -ENODEV;
	}

	pr_info("s9p-acpm: MFD channel %d: q_len=%u elem=%u commands@%08x replies@%08x\n",
		MFD_CHANNEL, mfd_ch.q_len, mfd_ch.q_elem_size,
		mfd_ch.tx_base, mfd_ch.rx_base);

	register_sys_off_handler(SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_FIRMWARE,
				 s9p_acpm_park, NULL);
	register_sys_off_handler(SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_FIRMWARE,
				 s9p_acpm_park, NULL);

	schedule_delayed_work(&s9p_touch_dwork, 2 * HZ);
	return 0;
}
subsys_initcall(s9p_acpm_init);

MODULE_DESCRIPTION("ACPM MFD (S2MPS18) mailbox access for Exynos 9810");
MODULE_LICENSE("GPL v2");
