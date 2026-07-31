/*
 * STC32G MCS-251 CPU migration state
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "migration/vmstate.h"

const VMStateDescription vms_mcs251_cpu = {
    .name = "mcs251-cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(env.pc, MCS251CPU),
        VMSTATE_UINT32_ARRAY(env.regs, MCS251CPU,
                             MCS251_NUM_REG_POSITIONS),
        VMSTATE_UINT32_ARRAY(env.dptr, MCS251CPU, 2),
        VMSTATE_UINT32(env.flag_c, MCS251CPU),
        VMSTATE_UINT32(env.flag_ac, MCS251CPU),
        VMSTATE_UINT32(env.flag_ov, MCS251CPU),
        VMSTATE_UINT32(env.flag_n, MCS251CPU),
        VMSTATE_UINT32(env.flag_z, MCS251CPU),
        VMSTATE_UINT32(env.flag_f0, MCS251CPU),
        VMSTATE_UINT32(env.flag_f1, MCS251CPU),
        VMSTATE_UINT32(env.reg_bank, MCS251CPU),
        VMSTATE_UINT32(env.pcon, MCS251CPU),
        VMSTATE_UINT32(env.auxr, MCS251CPU),
        VMSTATE_UINT32(env.intclko, MCS251CPU),
        VMSTATE_UINT32(env.auxr2, MCS251CPU),
        VMSTATE_UINT32(env.p_sw2, MCS251CPU),
        VMSTATE_UINT32(env.dps, MCS251CPU),
        VMSTATE_UINT32(env.ckcon, MCS251CPU),
        VMSTATE_UINT32(env.mxax, MCS251CPU),
        VMSTATE_UINT32(env.ta_stage, MCS251CPU),
        VMSTATE_UINT32(env.tcon, MCS251CPU),
        VMSTATE_UINT32(env.ie, MCS251CPU),
        VMSTATE_UINT32(env.ip, MCS251CPU),
        VMSTATE_UINT32(env.iph, MCS251CPU),
        VMSTATE_UINT32(env.irq_pending, MCS251CPU),
        VMSTATE_UINT32(env.irq_ack, MCS251CPU),
        VMSTATE_UINT32(env.irq_level, MCS251CPU),
        VMSTATE_UINT32(env.irq_depth, MCS251CPU),
        VMSTATE_UINT32_ARRAY(env.irq_level_stack, MCS251CPU,
                             MCS251_MAX_IRQ_DEPTH),
        VMSTATE_BOOL(env.direct_rmw, MCS251CPU),
        VMSTATE_BOOL(env.ta_touched, MCS251CPU),
        VMSTATE_BOOL(env.timer0_mode3, MCS251CPU),
        VMSTATE_BOOL(env.timer0_mode3_armed, MCS251CPU),
        VMSTATE_END_OF_LIST()
    }
};
