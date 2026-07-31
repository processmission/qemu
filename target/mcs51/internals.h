/*
 * MCS-51 family target internals
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MCS51_INTERNALS_H
#define TARGET_MCS51_INTERNALS_H

#include "cpu.h"

#ifdef TARGET_MCS251
#define MCS_TARGET_ADDR_MASK MCS251_ADDR_MASK
#define MCS_TARGET_RESET_PC MCS251_RESET_PC
#else
#define MCS_TARGET_ADDR_MASK MCS51_ADDR_MASK
#define MCS_TARGET_RESET_PC MCS51_RESET_PC
#endif

static inline hwaddr mcs251_cpu_idata_phys_addr(hwaddr addr)
{
#ifdef TARGET_MCS251
    return addr;
#else
    return MCS51_IDATA_PHYS_BASE + addr;
#endif
}

#endif
