/*
 * NXP S32 Mode Entry module
 *
 * This is a small model of the MC_ME register flow used by Zephyr's S32K5
 * early startup code to ungate the startup watchdog block.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/nxp_s32_mc_me.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

REG32(CTL_KEY, 0x000)
    FIELD(CTL_KEY, KEY, 0, 16)
REG32(MODE_CONF, 0x004)
REG32(MODE_UPD, 0x008)
    FIELD(MODE_UPD, MODE_UPD, 0, 1)
REG32(MODE_STAT, 0x00c)
REG32(MAIN_COREID, 0x010)
REG32(PRTN2_PUPD, 0x504)
    FIELD(PRTN2_PUPD, PCUD, 0, 1)
REG32(PRTN2_COFB1_STAT, 0x514)
REG32(PRTN2_COFB1_CLKEN, 0x534)

#define MC_ME_CTL_KEY_DIRECT_KEY   0x5af0
#define MC_ME_CTL_KEY_INVERTED_KEY 0xa50f

static void nxp_s32_mc_me_complete_update(NXPS32MCMEState *s)
{
    if (s->regs[R_PRTN2_PUPD] & R_PRTN2_PUPD_PCUD_MASK) {
        s->regs[R_PRTN2_COFB1_STAT] |= s->regs[R_PRTN2_COFB1_CLKEN];
        s->regs[R_PRTN2_PUPD] &= ~R_PRTN2_PUPD_PCUD_MASK;
    }

    if (s->regs[R_MODE_UPD] & R_MODE_UPD_MODE_UPD_MASK) {
        s->regs[R_MODE_STAT] = s->regs[R_MODE_CONF];
        s->regs[R_MODE_UPD] &= ~R_MODE_UPD_MODE_UPD_MASK;
    }
}

static void nxp_s32_mc_me_ctl_key_post_write(RegisterInfo *reg, uint64_t val)
{
    NXPS32MCMEState *s = NXP_S32_MC_ME(reg->opaque);
    uint32_t key = FIELD_EX32(val, CTL_KEY, KEY);

    if (key == MC_ME_CTL_KEY_DIRECT_KEY) {
        s->ctl_key_state = 1;
        return;
    }

    if (key == MC_ME_CTL_KEY_INVERTED_KEY && s->ctl_key_state == 1) {
        nxp_s32_mc_me_complete_update(s);
    }

    s->ctl_key_state = 0;
}

static const RegisterAccessInfo nxp_s32_mc_me_regs_info[] = {
    {   .name = "CTL_KEY",          .addr = A_CTL_KEY,
        .rsvd = UINT32_MAX & ~R_CTL_KEY_KEY_MASK,
        .post_write = nxp_s32_mc_me_ctl_key_post_write,
    },{ .name = "MODE_CONF",        .addr = A_MODE_CONF,
    },{ .name = "MODE_UPD",         .addr = A_MODE_UPD,
    },{ .name = "MODE_STAT",        .addr = A_MODE_STAT,
        .ro = UINT32_MAX,
    },{ .name = "MAIN_COREID",      .addr = A_MAIN_COREID,
        .ro = UINT32_MAX,
    },{ .name = "PRTN2_PUPD",       .addr = A_PRTN2_PUPD,
    },{ .name = "PRTN2_COFB1_STAT", .addr = A_PRTN2_COFB1_STAT,
        .ro = UINT32_MAX,
    },{ .name = "PRTN2_COFB1_CLKEN", .addr = A_PRTN2_COFB1_CLKEN,
    }
};

static const MemoryRegionOps nxp_s32_mc_me_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void nxp_s32_mc_me_reset(DeviceState *dev)
{
    NXPS32MCMEState *s = NXP_S32_MC_ME(dev);

    s->ctl_key_state = 0;

    for (int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
}

static void nxp_s32_mc_me_init(Object *obj)
{
    NXPS32MCMEState *s = NXP_S32_MC_ME(obj);
    DeviceState *dev = DEVICE(obj);

    s->reg_array = register_init_block32(dev, nxp_s32_mc_me_regs_info,
                                         ARRAY_SIZE(nxp_s32_mc_me_regs_info),
                                         s->regs_info, s->regs,
                                         &nxp_s32_mc_me_ops, false,
                                         NXP_S32_MC_ME_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
}

static const VMStateDescription vmstate_nxp_s32_mc_me = {
    .name = TYPE_NXP_S32_MC_ME,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, NXPS32MCMEState, NXP_S32_MC_ME_R_MAX),
        VMSTATE_UINT8(ctl_key_state, NXPS32MCMEState),
        VMSTATE_END_OF_LIST()
    }
};

static void nxp_s32_mc_me_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, nxp_s32_mc_me_reset);
    dc->vmsd = &vmstate_nxp_s32_mc_me;
}

static const TypeInfo nxp_s32_mc_me_info = {
    .name = TYPE_NXP_S32_MC_ME,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NXPS32MCMEState),
    .instance_init = nxp_s32_mc_me_init,
    .class_init = nxp_s32_mc_me_class_init,
};

static void nxp_s32_mc_me_register_types(void)
{
    type_register_static(&nxp_s32_mc_me_info);
}

type_init(nxp_s32_mc_me_register_types)
