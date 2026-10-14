// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): USB host on the Type-C port, for hubs and docks.
 *
 * The port's device tree pins the DWC3 to peripheral (USB gadget networking).
 * This module turns it into a dual-role controller at run time: it unbinds
 * the DWC3, changes its node to dr_mode = "otg" + usb-role-switch (default
 * peripheral) and binds it again. Then it follows s9p-usbc: while we source
 * VBUS to a partner the role is host, otherwise device.
 *
 * The exynos9810 USB PHY driver has no set_mode, so the host settings of the
 * vendor (phy_exynos_usb_v3p1_config_host_mode: D+/D- pull-downs on, no forced
 * VBUS valid) are written here, and undone for device mode.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/usb/role.h>
#include <linux/workqueue.h>

#include "s9p-usbc.h"

#define DWC3_NODE		"/soc@0/usb@10c00000"
#define USBCON_BASE		0x11100000
#define USBCON_UTMI		0x50
#define  UTMI_FORCE_VBUSVALID	BIT(5)
#define  UTMI_FORCE_BVALID	BIT(4)
#define  UTMI_DP_PULLDOWN	BIT(3)
#define  UTMI_DM_PULLDOWN	BIT(2)
#define USBCON_HSP		0x54
#define  HSP_VBUSVLDEXTSEL	BIT(13)
#define  HSP_VBUSVLDEXT		BIT(12)
#define USBCON_HSP_TUNE		0x58
/* TXVREF[31:28] TXRISE[25:24] TXRES[22:21] TXPREEMPA[19:18] SQRX[10:8] COMPDIS[2:0] */
#define HSP_TUNE_MASK		(0xfu << 28 | 0x3 << 24 | 0x3 << 21 | 0x3 << 18 | \
				 0x7 << 8 | 0x7)
/* host column of the stock star2lte hs_tune node: vref 7 rise 1 res 1 pre 3 sqrx 6 compdis 7 */
#define HSP_TUNE_HOST		(0x7u << 28 | 0x1 << 24 | 0x1 << 21 | 0x3 << 18 | \
				 0x6 << 8 | 0x7)

static bool host_tune = true;
module_param(host_tune, bool, 0644);
MODULE_PARM_DESC(host_tune, "use the stock host-mode HS PHY tuning while host");

static int force = -1;
static int force_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops force_ops = {
	.set = force_set,
	.get = param_get_int,
};
module_param_cb(force, &force_ops, &force, 0644);
MODULE_PARM_DESC(force, "-1 follow the Type-C state, 0 always device, 1 always host");

static struct {
	struct device_node *np;
	struct platform_device *pdev;
	struct of_changeset cs;
	bool cs_applied;
	struct usb_role_switch *sw;
	void __iomem *usbcon;
	struct notifier_block nb;
	struct work_struct work;
	struct mutex lock;
	enum usb_role role;
	u32 dev_tune;		/* HSP_TUNE as the PHY driver left it (device column) */
	bool ready;
} h;

static int force_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret && h.ready)
		schedule_work(&h.work);
	return ret;
}

static void rmw(u32 off, u32 clr, u32 set)
{
	writel((readl(h.usbcon + off) & ~clr) | set, h.usbcon + off);
}

static void phy_role(bool host)
{
	if (host) {
		rmw(USBCON_UTMI, UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID,
		    UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
		rmw(USBCON_HSP, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT, 0);
		if (host_tune)
			rmw(USBCON_HSP_TUNE, HSP_TUNE_MASK, HSP_TUNE_HOST);
	} else {
		rmw(USBCON_HSP_TUNE, HSP_TUNE_MASK, h.dev_tune & HSP_TUNE_MASK);
		rmw(USBCON_UTMI, UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN,
		    UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
		rmw(USBCON_HSP, 0, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT);
	}
}

static void set_role(enum usb_role role)
{
	int ret;

	if (role == h.role)
		return;
	ret = usb_role_switch_set_role(h.sw, role);
	/* the DWC3 switches in its own work item; the PHY follows after it */
	msleep(50);
	phy_role(role == USB_ROLE_HOST);
	pr_info("s9p-usbhost: role %s: %d (utmi %08x hsp %08x tune %08x)\n",
		usb_role_string(role), ret, readl(h.usbcon + USBCON_UTMI),
		readl(h.usbcon + USBCON_HSP), readl(h.usbcon + USBCON_HSP_TUNE));
	if (!ret)
		h.role = role;
}

static void role_work(struct work_struct *w)
{
	struct s9p_usbc_dp_state st = {};
	bool host;

	if (force >= 0)
		host = force;
	else
		host = !s9p_usbc_get_dp_state(&st) && st.host;
	mutex_lock(&h.lock);
	set_role(host ? USB_ROLE_HOST : USB_ROLE_DEVICE);
	mutex_unlock(&h.lock);
}

static int usbc_event(struct notifier_block *nb, unsigned long ev, void *data)
{
	schedule_work(&h.work);
	return NOTIFY_OK;
}

static void reattach(void)
{
	if (device_attach(&h.pdev->dev) != 1)
		pr_err("s9p-usbhost: DWC3 did not bind again\n");
}

static int __init s9p_usbhost_init(void)
{
	int ret;

	mutex_init(&h.lock);
	INIT_WORK(&h.work, role_work);
	h.np = of_find_node_by_path(DWC3_NODE);
	if (!h.np)
		return -ENODEV;
	h.pdev = of_find_device_by_node(h.np);
	if (!h.pdev) {
		ret = -ENODEV;
		goto put_np;
	}
	h.usbcon = ioremap(USBCON_BASE, 0x100);
	if (!h.usbcon) {
		ret = -ENOMEM;
		goto put_dev;
	}

	of_changeset_init(&h.cs);
	ret = of_changeset_update_prop_string(&h.cs, h.np, "dr_mode", "otg") ?:
	      of_changeset_add_prop_bool(&h.cs, h.np, "usb-role-switch") ?:
	      of_changeset_add_prop_string(&h.cs, h.np, "role-switch-default-mode",
					   "peripheral");
	if (ret)
		goto destroy_cs;

	device_release_driver(&h.pdev->dev);
	ret = of_changeset_apply(&h.cs);
	if (ret) {
		reattach();
		goto destroy_cs;
	}
	h.cs_applied = true;
	if (device_attach(&h.pdev->dev) != 1) {
		pr_err("s9p-usbhost: DWC3 did not bind as dual-role\n");
		ret = -ENODEV;
		goto revert;
	}
	h.sw = usb_role_switch_find_by_fwnode(of_fwnode_handle(h.np));
	if (!h.sw) {
		pr_err("s9p-usbhost: no role switch on the DWC3\n");
		ret = -ENODEV;
		goto revert;
	}
	h.role = USB_ROLE_DEVICE;
	h.dev_tune = readl(h.usbcon + USBCON_HSP_TUNE);
	h.ready = true;
	pr_info("s9p-usbhost: DWC3 is dual-role (default device)\n");

	h.nb.notifier_call = usbc_event;
	ret = s9p_usbc_register_notifier(&h.nb);
	if (ret)
		pr_warn("s9p-usbhost: no s9p-usbc (%d): set force=1 for host\n", ret);
	schedule_work(&h.work);
	return 0;

revert:
	if (h.sw)
		usb_role_switch_put(h.sw);
	device_release_driver(&h.pdev->dev);
	of_changeset_revert(&h.cs);
	h.cs_applied = false;
	reattach();
destroy_cs:
	of_changeset_destroy(&h.cs);
	iounmap(h.usbcon);
put_dev:
	put_device(&h.pdev->dev);
put_np:
	of_node_put(h.np);
	return ret;
}

static void __exit s9p_usbhost_exit(void)
{
	h.ready = false;
	s9p_usbc_unregister_notifier(&h.nb);
	cancel_work_sync(&h.work);
	mutex_lock(&h.lock);
	set_role(USB_ROLE_DEVICE);
	mutex_unlock(&h.lock);
	usb_role_switch_put(h.sw);
	device_release_driver(&h.pdev->dev);
	of_changeset_revert(&h.cs);
	of_changeset_destroy(&h.cs);
	reattach();
	iounmap(h.usbcon);
	put_device(&h.pdev->dev);
	of_node_put(h.np);
}

module_init(s9p_usbhost_init);
module_exit(s9p_usbhost_exit);
MODULE_DESCRIPTION("Galaxy S9+ Type-C USB host role (hubs, docks)");
MODULE_LICENSE("GPL");
