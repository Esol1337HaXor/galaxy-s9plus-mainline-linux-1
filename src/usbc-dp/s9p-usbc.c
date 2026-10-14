// SPDX-License-Identifier: GPL-2.0
/*
 * MAX77705 USB-C/PD MCU for the Galaxy S9+ (star2lte): D+/D- routing and
 * DisplayPort alternate mode.
 *
 * Supersedes s9p-max77705-muic (same compatible, same I2C client 0x25).
 *
 * The MCU inside the MAX77705 runs the whole USB-PD stack itself. For DP
 * alt mode it performs Discover Identity / SVIDs / Modes and DP Status on
 * its own; the AP has to read every VDM it stored (opcode 0x4B [id]),
 * send Enter Mode after Discover Modes and DP Configure after the first
 * DP Status / Attention (opcode 0x48), and source VBUS through the
 * charger's OTG boost when the partner is a sink (an unpowered adapter).
 * Register and opcode semantics follow Samsung's max77705_usbc.c /
 * max77705_alternate.c (Linux 4.9 for SM-G965F).
 *
 * Design: one worker owns the I2C client. It polls the four interrupt
 * registers (reading clears them; bits are accumulated while an opcode
 * waits for its APCmdResI), so the shared level IRQ of the MFD is never
 * involved and no two opcodes can overlap in the single 0x21/0x51 window.
 *
 * Consumers (the DP driver) subscribe with s9p_usbc_register_notifier()
 * and read the state with s9p_usbc_get_dp_state().
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mfd/max77705-private.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "s9p-usbc.h"

#define REG_UIC_HW_REV		0x00
#define REG_UIC_FW_REV		0x01
#define REG_UIC_INT		0x02
#define  UIC_INT_APCMDRES	BIT(7)
#define  UIC_INT_SYSMSG		BIT(6)
#define  UIC_INT_VBUSDET	BIT(5)
#define REG_CC_INT		0x03
#define  CC_INT_CCPINSTAT	BIT(3)
#define  CC_INT_CCSTAT		BIT(0)
#define REG_PD_INT		0x04
#define  PD_INT_DATAROLE	BIT(5)
#define  PD_INT_ATTENTION	BIT(4)
#define  PD_INT_DP_CONFIGURE	BIT(3)
#define  PD_INT_DP_STATUS	BIT(2)
#define REG_VDM_INT		0x05
#define  VDM_INT_DISC_ID	BIT(0)
#define  VDM_INT_DISC_SVIDS	BIT(1)
#define  VDM_INT_DISC_MODES	BIT(2)
#define  VDM_INT_ENTER_MODE	BIT(3)
#define REG_USBC_STATUS2	0x07	/* SYSMsg */
#define REG_BC_STATUS		0x08
#define  BC_STATUS_VBUSDET	BIT(7)
#define REG_UIC_FW_MINOR	0x09
#define REG_CC_STATUS0		0x0a
#define  CC_STATUS0_PIN(x)	(((x) >> 6) & 3)	/* 1 CC1, 2 CC2 */
#define  CC_STATUS0_STAT(x)	((x) & 7)		/* 1 SNK, 2 SRC */
#define  CC_STATUS0_CCI(x)	(((x) >> 4) & 3)	/* Rp current: 1 0.5 A, 2 1.5 A, 3 3 A */
#define  CCSTAT_NONE		0
#define  CCSTAT_SINK		1
#define  CCSTAT_SOURCE		2
#define REG_CC_STATUS1		0x0b
#define REG_PD_STATUS0		0x0c
#define REG_PD_STATUS1		0x0d
#define  PD_STATUS1_DFP		BIT(7)
#define REG_UIC_INT_M		0x0e
#define REG_CC_INT_M		0x0f
#define REG_PD_INT_M		0x10
#define REG_VDM_INT_M		0x11
#define REG_AP_DATAOUT0		0x21
#define REG_AP_DATAOUT32	0x41
#define REG_AP_DATAIN0		0x51

/* the vendor's "INIT" interrupt masks (max77705.h) */
#define UIC_INT_M_INIT		0x04
#define CC_INT_M_INIT		0x20
#define PD_INT_M_INIT		0x00
#define VDM_INT_M_INIT		0xf0

#define OPCODE_CONTROL1_WRITE	0x06
#define OPCODE_SET_VDM_REQ	0x48
#define OPCODE_GET_VDM_RESP	0x4b
#define OPCODE_AUTO_VBUS	0x54
#define OPCODE_ALT_MODE		0x55
#define  ALT_MODE_SRC_VDM	0x03

#define VDM_ID_DISC_ID		0x01
#define VDM_ID_DISC_SVIDS	0x02
#define VDM_ID_DISC_MODES	0x03
#define VDM_ID_ENTER_MODE	0x04
#define VDM_ID_ATTENTION	0x06
#define VDM_ID_DP_STATUS	0x10
#define VDM_ID_DP_CONFIGURE	0x11

#define SYSMSG_BOOT_WDT		0x03
#define SYSMSG_BOOT_POR		0x05
#define SYSMSG_POWER_NEGO	0x80

#define SVID_DP			0xff01

/* CONTROL1: COMP2SW(D+)[5:3] COMN1SW(D-)[2:0]; 001 = USB to the AP */
#define COM_USB			0x09

/* charger (0x69): its regmap has reg_base 0xB0, offsets are relative */
#define CHG_CNFG_00		MAX77705_CHG_REG_CNFG_00
#define  CHG_CNFG_00_OTG_CTRL	(BIT(1) | BIT(3))	/* OTG | BOOST */
#define  CHG_CNFG_00_MODE	GENMASK(3, 0)		/* BOOST OTG BUCK CHG */
#define  CHG_CNFG_00_MODE_CHG	0x05			/* charger + buck */
#define  CHG_CNFG_00_MODE_CHG_OTG 0x0f			/* + OTG boost, as before */

static int path = COM_USB;
module_param(path, int, 0644);
MODULE_PARM_DESC(path, "CONTROL1 switch value (0x09 USB, -1 leave alone)");

static int vbus_mode;
module_param(vbus_mode, int, 0444);
MODULE_PARM_DESC(vbus_mode, "0: AP sources VBUS via OTG (default), 2: MCU auto-VBUS, -1: do not touch");

static bool altmode = true;
module_param(altmode, bool, 0444);
MODULE_PARM_DESC(altmode, "enable DP alternate mode in the MCU");

static int poll_ms = 50;
module_param(poll_ms, int, 0644);
MODULE_PARM_DESC(poll_ms, "interrupt register poll period");

struct s9p_usbc {
	struct i2c_client *client;
	struct regmap *chg;		/* charger regmap, for OTG */
	struct delayed_work work;
	struct mutex lock;		/* the client and the state */
	struct blocking_notifier_head nh;
	struct dentry *dbg;
	u8 pend[4];			/* accumulated UIC/CC/PD/VDM int */
	u8 last_bc;
	u8 last_opcode;
	u8 last_cmd[33];
	int last_cmd_len;
	bool otg_on;
	int in_limit;			/* input current limit set as sink (uA) */
	int nego_retries;		/* POWER_NEGO resends of the current 0x48 */
	bool init_done;
	u16 vid, pid;
	u8 pin_caps;			/* UFP_D pin assignments of the sink */
	bool sent_config;
	unsigned long attach_jiffies;	/* CC attach as DFP, for the discovery fallback */
	bool disc_sent;
	struct s9p_usbc_dp_state dp;
	char log[4096];
	size_t log_len;
};

static struct s9p_usbc *the_usbc;
static DEFINE_MUTEX(the_usbc_lock);	/* the_usbc vs probe/remove */

static __printf(2, 3) void ulog(struct s9p_usbc *u, const char *fmt, ...)
{
	va_list ap;
	char line[160];
	int n;

	va_start(ap, fmt);
	n = vscnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	dev_info(&u->client->dev, "%s\n", line);
	if (u->log_len + n + 2 >= sizeof(u->log)) {
		/* drop the older half */
		size_t keep = u->log_len / 2;

		memmove(u->log, u->log + u->log_len - keep, keep);
		u->log_len = keep;
	}
	memcpy(u->log + u->log_len, line, n);
	u->log_len += n;
	u->log[u->log_len++] = '\n';
}

static int rd(struct s9p_usbc *u, u8 reg)
{
	return i2c_smbus_read_byte_data(u->client, reg);
}

static int wr(struct s9p_usbc *u, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(u->client, reg, val);
}

/* read and accumulate the four (clear-on-read) interrupt registers */
static void fetch_ints(struct s9p_usbc *u)
{
	u8 v[4];
	int ret, i;

	ret = i2c_smbus_read_i2c_block_data(u->client, REG_UIC_INT, 4, v);
	if (ret != 4)
		return;
	for (i = 0; i < 4; i++)
		u->pend[i] |= v[i];
}

/*
 * Run one opcode and wait for its APCmdResI. @resp receives rlen + 1 bytes
 * from 0x51 (resp[0] echoes the opcode). Caller holds u->lock.
 */
static int usbc_cmd(struct s9p_usbc *u, u8 opcode, const u8 *data, int len,
		    u8 *resp, int rlen)
{
	u8 buf[33];
	int ret, i;

	if (len > 31 || rlen > 31)
		return -EMSGSIZE;
	buf[0] = opcode;
	if (len)
		memcpy(buf + 1, data, len);
	if (opcode != OPCODE_SET_VDM_REQ || u->last_cmd_len != len + 1 ||
	    memcmp(u->last_cmd, buf, len + 1))
		u->nego_retries = 0;
	memcpy(u->last_cmd, buf, len + 1);
	u->last_cmd_len = len + 1;
	u->last_opcode = opcode;

	/* collect anything pending so it is not mistaken for our answer */
	fetch_ints(u);
	u->pend[0] &= ~UIC_INT_APCMDRES;

	ret = i2c_smbus_write_i2c_block_data(u->client, REG_AP_DATAOUT0,
					     len + 1, buf);
	if (ret < 0)
		return ret;
	if (len < 32) {
		ret = wr(u, REG_AP_DATAOUT32, 0);
		if (ret < 0)
			return ret;
	}
	for (i = 0; i < 300; i++) {
		usleep_range(5000, 6000);
		fetch_ints(u);
		if (u->pend[0] & UIC_INT_APCMDRES)
			break;
	}
	u->pend[0] &= ~UIC_INT_APCMDRES;
	if (i == 300) {
		ulog(u, "opcode %02x: no APCmdResI (sysmsg %02x)", opcode,
		     rd(u, REG_USBC_STATUS2));
		return -ETIMEDOUT;
	}
	if (resp && rlen >= 0) {
		ret = i2c_smbus_read_i2c_block_data(u->client, REG_AP_DATAIN0,
						    rlen + 1, resp);
		if (ret < 0)
			return ret;
		if (resp[0] != opcode) {
			ulog(u, "opcode %02x answered as %02x", opcode, resp[0]);
			return -EIO;
		}
	}
	return 0;
}

static void notify(struct s9p_usbc *u, unsigned long ev)
{
	struct s9p_usbc_dp_state st = u->dp;

	st.host = u->otg_on;
	mutex_unlock(&u->lock);
	blocking_notifier_call_chain(&u->nh, ev, &st);
	mutex_lock(&u->lock);
}

static void find_charger(struct s9p_usbc *u);

static void set_otg(struct s9p_usbc *u, bool on)
{
	int ret;

	if (vbus_mode != 0 || u->otg_on == on)
		return;
	if (!u->chg)
		find_charger(u);
	if (!u->chg) {
		ulog(u, "no charger regmap: cannot %s VBUS", on ? "source" : "stop");
		return;
	}
	/*
	 * The MCU resets the D+/D- switch on detach. Route USB to the AP
	 * before the partner (hub, receiver) gets VBUS, or it powers up with
	 * open data lines and does not enumerate.
	 */
	if (on && path >= 0) {
		u8 d = path, r[4];

		ulog(u, "CONTROL1 <= %02x before VBUS: %d", d,
		     usbc_cmd(u, OPCODE_CONTROL1_WRITE, &d, 1, r, 1));
	}
	/*
	 * Off: back to charge + buck explicitly (not only clearing the OTG
	 * bits), so that no earlier mode can leave the battery not charging.
	 */
	ret = regmap_update_bits(u->chg, CHG_CNFG_00, CHG_CNFG_00_MODE,
				 on ? CHG_CNFG_00_MODE_CHG_OTG : CHG_CNFG_00_MODE_CHG);
	ulog(u, "VBUS (OTG boost) %s: %d", on ? "on" : "off", ret);
	if (!ret) {
		u->otg_on = on;
		notify(u, S9P_USBC_ROLE);
	}
}

/*
 * Sink: the port's MUIC driver used to set the charger's input limit; with it
 * replaced, nobody did and the phone sat at 500 mA, less than it draws with a
 * desktop. Follow the Type-C current the source advertises on CC (capped at
 * 1.5 A: no PD contract, 5 V only).
 */
static void set_input_limit(struct s9p_usbc *u, int cc0)
{
	static const int ua[] = { 0, 500000, 1500000, 1500000 };
	union power_supply_propval v = { .intval = ua[CC_STATUS0_CCI(cc0)] };
	struct power_supply *psy;
	int ret;

	if (!v.intval || v.intval == u->in_limit)
		return;
	psy = power_supply_get_by_name("max77705-charger");
	if (!psy)
		return;
	ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &v);
	power_supply_put(psy);
	ulog(u, "input current limit %d mA (Rp %u): %d", v.intval / 1000, CC_STATUS0_CCI(cc0), ret);
	if (!ret)
		u->in_limit = v.intval;
}

static void reset_dp(struct s9p_usbc *u)
{
	bool was = u->dp.connected;

	u->sent_config = false;
	u->disc_sent = false;
	u->attach_jiffies = 0;
	u->pin_caps = 0;
	u->vid = u->pid = 0;
	memset(&u->dp, 0, sizeof(u->dp));
	if (was)
		notify(u, S9P_USBC_DP_DETACH);
}

static int usbc_init_mcu(struct s9p_usbc *u)
{
	u8 r[4];
	u8 d;
	int ret;

	/* APCmdResI must be able to latch while the init opcodes run */
	wr(u, REG_UIC_INT_M, UIC_INT_M_INIT);
	wr(u, REG_CC_INT_M, 0xff);
	wr(u, REG_PD_INT_M, 0xff);
	wr(u, REG_VDM_INT_M, 0xff);
	memset(u->pend, 0, sizeof(u->pend));
	fetch_ints(u);
	memset(u->pend, 0, sizeof(u->pend));

	if (path >= 0) {
		d = path;
		ret = usbc_cmd(u, OPCODE_CONTROL1_WRITE, &d, 1, r, 1);
		ulog(u, "CONTROL1 <= %02x: %d", d, ret);
	}
	if (vbus_mode >= 0) {
		d = vbus_mode;
		ret = usbc_cmd(u, OPCODE_AUTO_VBUS, &d, 1, r, 1);
		ulog(u, "auto-VBUS <= %02x: %d", d, ret);
	}
	if (altmode) {
		d = ALT_MODE_SRC_VDM;
		ret = usbc_cmd(u, OPCODE_ALT_MODE, &d, 1, r, 1);
		ulog(u, "alt mode enable <= %02x: %d (%02x %02x)", d, ret, r[0], r[1]);
	}
	wr(u, REG_UIC_INT_M, UIC_INT_M_INIT);
	wr(u, REG_CC_INT_M, CC_INT_M_INIT);
	wr(u, REG_PD_INT_M, PD_INT_M_INIT);
	wr(u, REG_VDM_INT_M, altmode ? VDM_INT_M_INIT : 0xff);
	/* evaluate the CC state once, as if it had just changed */
	u->pend[1] |= CC_INT_CCSTAT;
	u->init_done = true;
	return 0;
}

static u32 le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* VDM header command type [7:6]: 0 REQ 1 ACK 2 NAK 3 BUSY */
#define VDM_CMD_TYPE(h)		(((h) >> 6) & 3)
#define VDM_SVID(h)		((h) >> 16)

/* CC1/CC2 from CC_STATUS0, keeping the last good value on errors */
static u8 cc_pin(struct s9p_usbc *u)
{
	int cc0 = rd(u, REG_CC_STATUS0);

	if (cc0 >= 0 && (CC_STATUS0_PIN(cc0) == 1 || CC_STATUS0_PIN(cc0) == 2))
		return CC_STATUS0_PIN(cc0);
	return u->dp.orientation;
}

static int get_vdm(struct s9p_usbc *u, u8 id, u8 *r, int rlen)
{
	int ret = usbc_cmd(u, OPCODE_GET_VDM_RESP, &id, 1, r, rlen);

	if (ret)
		ulog(u, "read VDM %02x: %d", id, ret);
	return ret;
}

static void send_enter_mode(struct s9p_usbc *u)
{
	static const u8 enter[] = { 0x09, 0x04, 0x81, 0x01, 0xff };
	u8 r[12];
	int ret = usbc_cmd(u, OPCODE_SET_VDM_REQ, enter, sizeof(enter), r, 10);

	ulog(u, "-> Enter Mode DP: %d", ret);
}

/* the vendor's preference: with multi-function D, B, F; else C, E, A, D, B, F */
static u8 pick_pin(u8 caps, bool mfp)
{
	static const u8 mf[] = { S9P_DP_PIN_D, S9P_DP_PIN_B, S9P_DP_PIN_F };
	static const u8 dp[] = { S9P_DP_PIN_C, S9P_DP_PIN_E, S9P_DP_PIN_A,
				 S9P_DP_PIN_D, S9P_DP_PIN_B, S9P_DP_PIN_F };
	int i;

	if (mfp) {
		for (i = 0; i < ARRAY_SIZE(mf); i++)
			if (caps & mf[i])
				return mf[i];
	}
	for (i = 0; i < ARRAY_SIZE(dp); i++)
		if (caps & dp[i])
			return dp[i];
	return 0;
}

static void send_configure(struct s9p_usbc *u, u8 pin)
{
	u8 cfg[] = { 0x0a, 0x11, 0x81, 0x01, 0xff, 0x06, pin, 0x00, 0x00 };
	u8 r[16];
	int ret = usbc_cmd(u, OPCODE_SET_VDM_REQ, cfg, sizeof(cfg), r, 14);

	ulog(u, "-> DP Configure pin mask %02x: %d", pin, ret);
}

/* DP Status Update and Attention carry the same VDO */
static void handle_dp_status(struct s9p_usbc *u, const u8 *r, bool attention)
{
	u32 hdr = le32(r + 4), vdo = le32(r + 8);
	bool hpd = vdo & BIT(7), irq = vdo & BIT(8);
	bool old_hpd = u->dp.hpd;

	ulog(u, "%s: hdr %08x vdo %08x (conn %u mfp %u hpd %u irq %u)",
	     attention ? "Attention" : "DP Status", hdr, vdo, vdo & 3,
	     !!(vdo & BIT(4)), hpd, irq);
	if (VDM_CMD_TYPE(hdr) == 2 || VDM_SVID(hdr) != SVID_DP)
		return;
	if (!u->sent_config && (attention || (vdo & 3))) {
		u8 pin = pick_pin(u->pin_caps, vdo & BIT(4));

		if (!pin) {
			ulog(u, "no usable pin assignment in %02x", u->pin_caps);
		} else {
			u->dp.pin = pin;
			send_configure(u, pin);
			u->sent_config = true;
		}
	}
	u->dp.hpd = hpd;
	u->dp.hpd_irq = irq;
	if (hpd != old_hpd || (hpd && irq))
		notify(u, S9P_USBC_DP_HPD);
}

static void handle_vdm(struct s9p_usbc *u)
{
	u8 v = u->pend[3], p = u->pend[2];
	u8 r[32];
	u32 hdr, vdo;
	int i;

	u->pend[3] = 0;
	u->pend[2] &= ~(PD_INT_ATTENTION | PD_INT_DP_CONFIGURE | PD_INT_DP_STATUS);

	if ((v & VDM_INT_DISC_ID) && !get_vdm(u, VDM_ID_DISC_ID, r, 31)) {
		u->vid = le32(r + 8) & 0xffff;
		u->pid = le32(r + 16) >> 16;
		u->sent_config = false;
		ulog(u, "Discover Identity: hdr %08x VID %04x PID %04x",
		     le32(r + 4), u->vid, u->pid);
	}
	if ((v & VDM_INT_DISC_SVIDS) && !get_vdm(u, VDM_ID_DISC_SVIDS, r, 31)) {
		bool dp = false;

		hdr = le32(r + 4);
		for (i = 0; i < 6; i++) {
			vdo = le32(r + 8 + 4 * i);
			if (!vdo)
				break;
			ulog(u, "SVID: %04x %04x", vdo >> 16, vdo & 0xffff);
			if ((vdo >> 16) == SVID_DP || (vdo & 0xffff) == SVID_DP)
				dp = true;
		}
		if (dp && !u->dp.connected) {
			u->dp.connected = true;
			u->dp.orientation = cc_pin(u);
			ulog(u, "DP SVID present, orientation CC%u", u->dp.orientation);
			notify(u, S9P_USBC_DP_ATTACH);
		}
	}
	if ((v & VDM_INT_DISC_MODES) && !get_vdm(u, VDM_ID_DISC_MODES, r, 11)) {
		hdr = le32(r + 4);
		vdo = le32(r + 8);
		ulog(u, "Discover Modes: hdr %08x vdo %08x", hdr, vdo);
		if (VDM_SVID(hdr) == SVID_DP && VDM_CMD_TYPE(hdr) == 1) {
			unsigned int cap = vdo & 3;
			bool receptacle = vdo & BIT(6);

			if (cap == 2) {
				ulog(u, "partner is DFP_D only: not a display");
			} else {
				u->pin_caps = receptacle ? (vdo >> 16) & 0xff
							 : (vdo >> 8) & 0xff;
				/* some adapters fill only one of the two fields */
				if (!u->pin_caps)
					u->pin_caps = ((vdo >> 8) | (vdo >> 16)) & 0x3f;
				ulog(u, "sink pin assignments %02x", u->pin_caps);
				if (!u->dp.connected) {
					u->dp.connected = true;
					u->dp.orientation = cc_pin(u);
					notify(u, S9P_USBC_DP_ATTACH);
				}
				send_enter_mode(u);
			}
		}
	}
	if ((v & VDM_INT_ENTER_MODE) && !get_vdm(u, VDM_ID_ENTER_MODE, r, 7)) {
		hdr = le32(r + 4);
		ulog(u, "Enter Mode: hdr %08x (%s)", hdr,
		     VDM_CMD_TYPE(hdr) == 1 ? "ACK" : "not ACK");
	}
	if ((p & PD_INT_DP_STATUS) && !get_vdm(u, VDM_ID_DP_STATUS, r, 11))
		handle_dp_status(u, r, false);
	if ((p & PD_INT_DP_CONFIGURE) && !get_vdm(u, VDM_ID_DP_CONFIGURE, r, 11)) {
		hdr = le32(r + 4);
		ulog(u, "DP Configure: hdr %08x vdo %08x", hdr, le32(r + 8));
		/* the vendor only filters NAK (BUSY counts as configured) */
		if (VDM_CMD_TYPE(hdr) != 2) {
			u->dp.link_conf = true;
			notify(u, S9P_USBC_DP_LINK_CONF);
		}
	}
	if ((p & PD_INT_ATTENTION) && !get_vdm(u, VDM_ID_ATTENTION, r, 11))
		handle_dp_status(u, r, true);
}

static void handle_cc(struct s9p_usbc *u)
{
	int cc0 = rd(u, REG_CC_STATUS0), pd1 = rd(u, REG_PD_STATUS1);
	unsigned int st;

	u->pend[1] = 0;
	if (cc0 < 0)
		return;
	st = CC_STATUS0_STAT(cc0);
	ulog(u, "CC: status0 %02x (ccstat %u, CC%u) pd1 %02x (%s)", cc0, st,
	     CC_STATUS0_PIN(cc0), pd1, pd1 >= 0 && (pd1 & PD_STATUS1_DFP) ? "DFP" : "UFP");
	if (CC_STATUS0_PIN(cc0) == 1 || CC_STATUS0_PIN(cc0) == 2)
		u->dp.orientation = CC_STATUS0_PIN(cc0);
	if (st != CCSTAT_NONE && pd1 >= 0 && (pd1 & PD_STATUS1_DFP) && !u->attach_jiffies)
		u->attach_jiffies = jiffies ?: 1;
	switch (st) {
	case CCSTAT_SOURCE:
		set_otg(u, true);
		break;
	case CCSTAT_SINK:
		set_otg(u, false);
		set_input_limit(u, cc0);
		break;
	case CCSTAT_NONE:
		u->in_limit = 0;
		set_otg(u, false);
		reset_dp(u);
		break;
	}
}

static void handle_sysmsg(struct s9p_usbc *u)
{
	int msg = rd(u, REG_USBC_STATUS2);
	u8 r[16];

	u->pend[0] &= ~UIC_INT_SYSMSG;
	if (msg <= 0)
		return;
	ulog(u, "SYSMsg %02x", msg);
	if (msg == SYSMSG_BOOT_WDT || msg == SYSMSG_BOOT_POR) {
		ulog(u, "MCU restarted: initialising again");
		reset_dp(u);
		set_otg(u, false);
		if (msg == SYSMSG_BOOT_POR) {
			/* vendor: reset the MCU once more and clear UIC_INT */
			wr(u, 0x80, 0x0f);
			msleep(100);
			rd(u, REG_UIC_INT);
		}
		usbc_init_mcu(u);
	} else if (msg == SYSMSG_POWER_NEGO && u->last_opcode == OPCODE_SET_VDM_REQ &&
		   u->nego_retries++ < 3) {
		/* the VDM went out before the PD contract: send it again */
		msleep(100);
		usbc_cmd(u, OPCODE_SET_VDM_REQ, u->last_cmd + 1,
			 u->last_cmd_len - 1, r, 14);
	}
}

static void usbc_work(struct work_struct *work)
{
	struct s9p_usbc *u = container_of(work, struct s9p_usbc, work.work);
	bool busy;
	int bc;

	mutex_lock(&u->lock);
	if (!u->init_done)
		usbc_init_mcu(u);
	fetch_ints(u);
	if (u->pend[0] & UIC_INT_SYSMSG)
		handle_sysmsg(u);
	if (u->pend[1] & (CC_INT_CCSTAT | CC_INT_CCPINSTAT))
		handle_cc(u);
	if (u->pend[3] || (u->pend[2] & (PD_INT_ATTENTION | PD_INT_DP_CONFIGURE |
					 PD_INT_DP_STATUS)))
		handle_vdm(u);
	/*
	 * A partner attached before alt mode was enabled (module loaded
	 * with the adapter already in) gets no discovery from the MCU. The
	 * vendor's AP-initiated Discover Identity (SNK->DFP case) covers it.
	 */
	if (altmode && u->attach_jiffies && !u->vid && !u->disc_sent &&
	    time_after(jiffies, u->attach_jiffies + 2 * HZ)) {
		static const u8 disc[] = { 0x09, 0x01, 0x80, 0x00, 0xff };
		u8 r[12];

		u->disc_sent = true;
		ulog(u, "no discovery from the MCU: sending Discover Identity: %d",
		     usbc_cmd(u, OPCODE_SET_VDM_REQ, disc, sizeof(disc), r, 10));
	}
	if (u->pend[2] & PD_INT_DATAROLE) {
		int pd1 = rd(u, REG_PD_STATUS1);

		u->pend[2] &= ~PD_INT_DATAROLE;
		ulog(u, "data role: %s", pd1 >= 0 && (pd1 & PD_STATUS1_DFP) ? "DFP" : "UFP");
		if (pd1 >= 0 && (pd1 & PD_STATUS1_DFP) && !u->attach_jiffies)
			u->attach_jiffies = jiffies ?: 1;
	}
	/* bits nobody handles: drop them so they do not keep us busy */
	u->pend[2] &= PD_INT_ATTENTION | PD_INT_DP_CONFIGURE | PD_INT_DP_STATUS;
	u->pend[1] &= CC_INT_CCSTAT | CC_INT_CCPINSTAT;
	u->pend[0] &= UIC_INT_SYSMSG;

	/* VBUS reappeared: the MCU may have dropped the D+/D- setting */
	bc = rd(u, REG_BC_STATUS);
	if (bc >= 0 && (bc & BC_STATUS_VBUSDET) && !(u->last_bc & BC_STATUS_VBUSDET) &&
	    path >= 0 && !u->otg_on) {
		u8 d = path, r[4];

		usbc_cmd(u, OPCODE_CONTROL1_WRITE, &d, 1, r, 1);
	}
	if (bc >= 0)
		u->last_bc = bc;
	/* interrupts that arrived while opcodes ran are handled right away */
	busy = u->pend[0] || u->pend[1] || u->pend[2] || u->pend[3];
	mutex_unlock(&u->lock);

	schedule_delayed_work(&u->work, busy ? 0 : msecs_to_jiffies(max(poll_ms, 10)));
}

/* ---- consumer API ---- */

int s9p_usbc_register_notifier(struct notifier_block *nb)
{
	int ret = -EPROBE_DEFER;

	mutex_lock(&the_usbc_lock);
	if (the_usbc)
		ret = blocking_notifier_chain_register(&the_usbc->nh, nb);
	mutex_unlock(&the_usbc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(s9p_usbc_register_notifier);

void s9p_usbc_unregister_notifier(struct notifier_block *nb)
{
	mutex_lock(&the_usbc_lock);
	if (the_usbc)
		blocking_notifier_chain_unregister(&the_usbc->nh, nb);
	mutex_unlock(&the_usbc_lock);
}
EXPORT_SYMBOL_GPL(s9p_usbc_unregister_notifier);

int s9p_usbc_get_dp_state(struct s9p_usbc_dp_state *st)
{
	int ret = -ENODEV;

	mutex_lock(&the_usbc_lock);
	if (the_usbc) {
		mutex_lock(&the_usbc->lock);
		*st = the_usbc->dp;
		st->host = the_usbc->otg_on;
		mutex_unlock(&the_usbc->lock);
		ret = 0;
	}
	mutex_unlock(&the_usbc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(s9p_usbc_get_dp_state);

/* ---- debugfs: state, log, raw opcodes ---- */

static int state_show(struct seq_file *s, void *unused)
{
	struct s9p_usbc *u = s->private;
	int i;

	mutex_lock(&u->lock);
	seq_printf(s, "dp.connected=%d pin=%02x orientation=CC%u hpd=%d irq=%d link_conf=%d\n",
		   u->dp.connected, u->dp.pin, u->dp.orientation, u->dp.hpd,
		   u->dp.hpd_irq, u->dp.link_conf);
	seq_printf(s, "vid=%04x pid=%04x pin_caps=%02x otg=%d\n", u->vid, u->pid,
		   u->pin_caps, u->otg_on);
	seq_puts(s, "regs 00..11:");
	for (i = 0; i <= 0x11; i++)
		seq_printf(s, " %02x", rd(u, i) & 0xff);
	seq_putc(s, '\n');
	mutex_unlock(&u->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(state);

static int log_show(struct seq_file *s, void *unused)
{
	struct s9p_usbc *u = s->private;

	mutex_lock(&u->lock);
	seq_write(s, u->log, u->log_len);
	mutex_unlock(&u->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(log);

/* write "OPCODE [DATA..] [rlen=N]" in hex, e.g. "4b 10 rlen=11" */
static ssize_t opcode_write(struct file *f, const char __user *ubuf,
			    size_t n, loff_t *pos)
{
	struct s9p_usbc *u = f->private_data;
	char buf[160], *p, *tok;
	u8 b[33], r[32];
	int len = 0, rlen = 16, ret, i;
	char out[120];
	int o = 0;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	p = buf;
	while ((tok = strsep(&p, " \t\n")) && len < 33) {
		unsigned int v;

		if (!*tok)
			continue;
		if (sscanf(tok, "rlen=%d", &rlen) == 1)
			continue;
		if (kstrtouint(tok, 16, &v) || v > 0xff)
			return -EINVAL;
		b[len++] = v;
	}
	if (!len || rlen < 0 || rlen > 31)
		return -EINVAL;
	mutex_lock(&u->lock);
	ret = usbc_cmd(u, b[0], b + 1, len - 1, r, rlen);
	if (!ret)
		for (i = 0; i <= rlen && o < sizeof(out) - 4; i++)
			o += scnprintf(out + o, sizeof(out) - o, " %02x", r[i]);
	out[o] = 0;
	ulog(u, "debugfs opcode %02x: %d:%s", b[0], ret, out);
	mutex_unlock(&u->lock);
	return ret ? ret : n;
}

static const struct file_operations opcode_fops = {
	.open = simple_open,
	.write = opcode_write,
};

static void find_charger(struct s9p_usbc *u)
{
	struct device_node *np;
	struct i2c_client *c;

	np = of_find_compatible_node(NULL, NULL, "maxim,max77705-charger");
	if (!np)
		return;
	c = of_find_i2c_device_by_node(np);
	of_node_put(np);
	if (!c)
		return;
	u->chg = dev_get_regmap(&c->dev, NULL);
	put_device(&c->dev);
}

static int s9p_usbc_probe(struct i2c_client *client)
{
	struct s9p_usbc *u;
	int hw, fw, minor;

	u = devm_kzalloc(&client->dev, sizeof(*u), GFP_KERNEL);
	if (!u)
		return -ENOMEM;
	u->client = client;
	mutex_init(&u->lock);
	BLOCKING_INIT_NOTIFIER_HEAD(&u->nh);
	i2c_set_clientdata(client, u);

	hw = rd(u, REG_UIC_HW_REV);
	fw = rd(u, REG_UIC_FW_REV);
	minor = rd(u, REG_UIC_FW_MINOR);
	if (hw < 0 || fw < 0)
		return dev_err_probe(&client->dev, hw < 0 ? hw : fw,
				     "MAX77705 USBC does not answer\n");
	find_charger(u);
	ulog(u, "MAX77705 USBC: HW %02x FW %02x.%02x, charger regmap %s",
	     hw, fw, minor, u->chg ? "found" : "MISSING");
	u->last_bc = rd(u, REG_BC_STATUS);

	u->dbg = debugfs_create_dir("s9p-usbc", NULL);
	debugfs_create_file("state", 0444, u->dbg, u, &state_fops);
	debugfs_create_file("log", 0444, u->dbg, u, &log_fops);
	debugfs_create_file("opcode", 0200, u->dbg, u, &opcode_fops);

	mutex_lock(&the_usbc_lock);
	the_usbc = u;
	mutex_unlock(&the_usbc_lock);
	INIT_DELAYED_WORK(&u->work, usbc_work);
	schedule_delayed_work(&u->work, 0);
	return 0;
}

static void s9p_usbc_remove(struct i2c_client *client)
{
	struct s9p_usbc *u = i2c_get_clientdata(client);

	mutex_lock(&the_usbc_lock);
	the_usbc = NULL;
	mutex_unlock(&the_usbc_lock);
	cancel_delayed_work_sync(&u->work);
	mutex_lock(&u->lock);
	set_otg(u, false);
	reset_dp(u);	/* tells a still-registered DP driver the sink is gone */
	mutex_unlock(&u->lock);
	debugfs_remove_recursive(u->dbg);
}

/*
 * Reboot/poweroff with a hub attached: stop sourcing VBUS first. Otherwise
 * the boost stays on into the bootloader, which then sees a powered port.
 */
static void s9p_usbc_shutdown(struct i2c_client *client)
{
	struct s9p_usbc *u = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&u->work);
	mutex_lock(&u->lock);
	set_otg(u, false);
	mutex_unlock(&u->lock);
}

static const struct of_device_id s9p_usbc_of_match[] = {
	{ .compatible = "samsung,s9p-max77705-muic" },
	{ .compatible = "samsung,s9p-max77705-usbc" },
	{ }
};
MODULE_DEVICE_TABLE(of, s9p_usbc_of_match);

static struct i2c_driver s9p_usbc_driver = {
	.driver = {
		.name		= "s9p-max77705-usbc",
		.of_match_table	= s9p_usbc_of_match,
	},
	.probe	= s9p_usbc_probe,
	.remove	= s9p_usbc_remove,
	.shutdown = s9p_usbc_shutdown,
};
module_i2c_driver(s9p_usbc_driver);

MODULE_DESCRIPTION("MAX77705 USB-C/PD: D+/D- routing and DP alt mode (Galaxy S9+)");
MODULE_LICENSE("GPL");
