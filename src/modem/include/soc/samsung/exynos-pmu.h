/* SPDX-License-Identifier: GPL-2.0 */
/* PMU register access for the vendor CP control (pmu-cp.c, pmucal_cp.c). */
#ifndef S9P_EXYNOS_PMU_H
#define S9P_EXYNOS_PMU_H
#include <linux/types.h>
int exynos_pmu_read(unsigned int offset, unsigned int *val);
int exynos_pmu_write(unsigned int offset, unsigned int val);
int exynos_pmu_update(unsigned int offset, unsigned int mask, unsigned int val);
#endif
