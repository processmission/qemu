/*
 * NXP S32 Mode Entry module
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_NXP_S32_MC_ME_H
#define HW_MISC_NXP_S32_MC_ME_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"

#define TYPE_NXP_S32_MC_ME "nxp-s32-mc-me"
OBJECT_DECLARE_SIMPLE_TYPE(NXPS32MCMEState, NXP_S32_MC_ME)

#define NXP_S32_MC_ME_SIZE 0x4000
#define NXP_S32_MC_ME_R_MAX (NXP_S32_MC_ME_SIZE / 4)

struct NXPS32MCMEState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[NXP_S32_MC_ME_R_MAX];
    uint32_t regs[NXP_S32_MC_ME_R_MAX];
    uint8_t ctl_key_state;
};

#endif
