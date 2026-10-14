/* SPDX-License-Identifier: GPL-2.0 */
#ifndef S9P_USBC_H
#define S9P_USBC_H

#include <linux/notifier.h>
#include <linux/types.h>

/* DP pin assignment masks (VESA DP alt mode) */
#define S9P_DP_PIN_A	0x01
#define S9P_DP_PIN_B	0x02
#define S9P_DP_PIN_C	0x04
#define S9P_DP_PIN_D	0x08
#define S9P_DP_PIN_E	0x10
#define S9P_DP_PIN_F	0x20

enum {
	S9P_USBC_DP_ATTACH = 1,	/* a DP-capable partner (SVID 0xff01) */
	S9P_USBC_DP_DETACH,
	S9P_USBC_DP_LINK_CONF,	/* DP Configure acknowledged: pin is final */
	S9P_USBC_DP_HPD,	/* hpd and/or hpd_irq changed */
	S9P_USBC_ROLE,		/* host changed: we source VBUS to a partner */
};

struct s9p_usbc_dp_state {
	bool connected;
	u8 pin;			/* S9P_DP_PIN_x */
	u8 orientation;		/* 1 = CC1, 2 = CC2, 0 = unknown */
	bool hpd;
	bool hpd_irq;
	bool link_conf;
	bool host;		/* VBUS sourced: the USB controller should be host */
};

int s9p_usbc_register_notifier(struct notifier_block *nb);
void s9p_usbc_unregister_notifier(struct notifier_block *nb);
int s9p_usbc_get_dp_state(struct s9p_usbc_dp_state *st);

#endif
