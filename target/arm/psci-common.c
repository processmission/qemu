/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Common ARM PSCI helpers.
 *
 * Copyright (c) 2026 Chao Liu
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"

/*
 * Optional board-registered SMC handler. SMC exception paths call this before
 * generic PSCI handling or architectural SMC behavior. Default NULL.
 */
static ArmPsciSmcHandler arm_psci_smc_handler;

void arm_register_psci_smc_handler(ArmPsciSmcHandler handler)
{
    arm_psci_smc_handler = handler;
}

bool arm_handle_psci_smc_handler(ARMCPU *cpu)
{
    return arm_psci_smc_handler && arm_psci_smc_handler(cpu);
}
