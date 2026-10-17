/* SPDX-License-Identifier: GPL-2.0 */
/* Kernel API that changed between Samsung's 4.9 and mainline 7.1, for the vendor modem code. */
#ifndef S9P_MODEM_COMPAT_H
#define S9P_MODEM_COMPAT_H
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/etherdevice.h>
#include <linux/proc_fs.h>
#define timespec timespec64
#define getnstimeofday(ts) ktime_get_real_ts64(ts)
#define random_ether_addr(a) eth_random_addr(a)
#define PDE_DATA(inode) pde_data(inode)
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/sched/clock.h>
#include <linux/arm-smccc.h>
#include <linux/pm_qos.h>
#define strlcpy(d, s, n) strscpy(d, s, n)
#define del_timer(t) timer_delete(t)
#define del_timer_sync(t) timer_delete_sync(t)
#define cpu_clock(cpu) local_clock()
#define time_to_tm(t, off, tm) time64_to_tm(t, off, tm)
static inline unsigned long exynos_smc(unsigned long a, unsigned long b,
				       unsigned long c, unsigned long d)
{
	struct arm_smccc_res r;

	arm_smccc_smc(a, b, c, d, 0, 0, 0, 0, &r);
	return r.a0;
}
/* Samsung's CPU/bus frequency QoS classes: no such thing in mainline, no-ops */
#define PM_QOS_CLUSTER0_FREQ_MIN	0
#define PM_QOS_CLUSTER1_FREQ_MIN	0
#define PM_QOS_CLUSTER1_FREQ_MAX	0
#define PM_QOS_BUS_THROUGHPUT		0
#define PM_QOS_DEVICE_THROUGHPUT	0
#define PM_QOS_CPU_ONLINE_MIN		0
#define pm_qos_add_request(r, c, v)	do { } while (0)
#define pm_qos_update_request(r, v)	do { } while (0)
#define pm_qos_remove_request(r)	do { } while (0)
#define pm_qos_request_active(r)	0
#endif
