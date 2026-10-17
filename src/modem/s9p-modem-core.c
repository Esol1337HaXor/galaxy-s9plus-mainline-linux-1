// SPDX-License-Identifier: GPL-2.0
/*
 * One module for Samsung's modem interface on the Galaxy S9+: CP power
 * control, the shared-memory region driver (shm_ipc), the CP mailbox
 * (mcu_ipc) and the modem_v1 core with its SS310AP control and shared-memory
 * link, registered in the order the vendor's initcalls had them.
 */
#include <linux/module.h>
#include <soc/samsung/cal-if.h>

int s9p_shm_ipc_register(void);
void s9p_shm_ipc_unregister(void);
int s9p_mcu_ipc_register(void);
void s9p_mcu_ipc_unregister(void);
int s9p_modem_register(void);
void s9p_modem_unregister(void);

static int __init s9p_modem_init(void)
{
	int ret;

	ret = s9p_cp_pmu_init();
	if (ret)
		return ret;
	ret = s9p_shm_ipc_register();
	if (ret)
		goto err_pmu;
	ret = s9p_mcu_ipc_register();
	if (ret)
		goto err_shm;
	ret = s9p_modem_register();
	if (ret)
		goto err_mcu;
	return 0;

err_mcu:
	s9p_mcu_ipc_unregister();
err_shm:
	s9p_shm_ipc_unregister();
err_pmu:
	s9p_cp_pmu_exit();
	return ret;
}

static void __exit s9p_modem_exit(void)
{
	s9p_modem_unregister();
	s9p_mcu_ipc_unregister();
	s9p_shm_ipc_unregister();
	s9p_cp_pmu_exit();
}

module_init(s9p_modem_init);
module_exit(s9p_modem_exit);
MODULE_DESCRIPTION("Galaxy S9+ modem interface (Samsung modem_v1 on mainline)");
MODULE_LICENSE("GPL");
