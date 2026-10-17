/* SPDX-License-Identifier: GPL-2.0 */
/* The CP part of the vendor cal-if API (implemented in s9p-cp-pmu.c). */
#ifndef S9P_CAL_IF_H
#define S9P_CAL_IF_H
extern int cal_cp_init(void);
extern int cal_cp_status(void);
extern int cal_cp_reset_assert(void);
extern int cal_cp_reset_release(void);
extern void cal_cp_active_clear(void);
extern void cal_cp_reset_req_clear(void);
extern void cal_cp_enable_dump_pc_no_pg(void);
extern void cal_cp_disable_dump_pc_no_pg(void);
int s9p_cp_pmu_init(void);
void s9p_cp_pmu_exit(void);
#endif
