/*
 * STM32G474 power control
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/misc/stm32g474_pwr.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define STM32G474_PWR_R_MAX (0x84 / sizeof(uint32_t))

struct Stm32g474PwrState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    uint32_t regs[STM32G474_PWR_R_MAX];
    RegisterInfo regs_info[STM32G474_PWR_R_MAX];

    Clock *clk;
    bool peripheral_reset_asserted;
    bool resetting;
};

REG32(PWR_CR1, 0x00)
    FIELD(PWR_CR1, LPMS, 0, 3)
    FIELD(PWR_CR1, DBP, 8, 1)
    FIELD(PWR_CR1, VOS, 9, 2)
    FIELD(PWR_CR1, LPR, 14, 1)
REG32(PWR_CR3, 0x08)
    FIELD(PWR_CR3, EWUP, 0, 5)
    FIELD(PWR_CR3, RRS, 8, 1)
    FIELD(PWR_CR3, APC, 10, 1)
    FIELD(PWR_CR3, UCPD_STDBY, 13, 1)
    FIELD(PWR_CR3, UCPD_DBDIS, 14, 1)
    FIELD(PWR_CR3, EIWUL, 15, 1)
REG32(PWR_SR2, 0x14)
    FIELD(PWR_SR2, FLASH_RDY, 7, 1)
    FIELD(PWR_SR2, REGLPS, 8, 1)
    FIELD(PWR_SR2, REGLPF, 9, 1)
    FIELD(PWR_SR2, VOSF, 10, 1)
    FIELD(PWR_SR2, PVDO, 11, 1)
    FIELD(PWR_SR2, PVMO1, 14, 1)
    FIELD(PWR_SR2, PVMO2, 15, 1)
REG32(PWR_CR5, 0x80)
    FIELD(PWR_CR5, R1MODE, 8, 1)

static uint64_t stm32g474_pwr_held_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474PwrState *s = STM32G474_PWR(reg->opaque);

    return s->peripheral_reset_asserted ?
           *(uint32_t *)reg->data : val;
}

static uint64_t stm32g474_pwr_cr1_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474PwrState *s = STM32G474_PWR(reg->opaque);
    uint32_t vos;

    if (s->peripheral_reset_asserted) {
        return s->regs[R_PWR_CR1];
    }

    vos = FIELD_EX32(val, PWR_CR1, VOS);
    if (vos == 0 || vos == 3) {
        val = FIELD_DP32(val, PWR_CR1, VOS,
                         FIELD_EX32(s->regs[R_PWR_CR1], PWR_CR1, VOS));
    }

    return val;
}

static const RegisterAccessInfo stm32g474_pwr_regs_info[] = {
    {
        .name = "CR1",
        .addr = A_PWR_CR1,
        .reset = 0x00000200,
        .rsvd = 0xffffb8f8,
        .unimp = 0x00004107,
        .pre_write = stm32g474_pwr_cr1_pre_write,
    }, {
        .name = "CR3",
        .addr = A_PWR_CR3,
        .reset = 0x00008000,
        .rsvd = 0xffff1ae0,
        .unimp = 0x0000251f,
        .pre_write = stm32g474_pwr_held_pre_write,
    }, {
        .name = "SR2",
        .addr = A_PWR_SR2,
        .ro = 0x0000cf80,
        .rsvd = 0xffff307f,
        .unimp = 0x0000cf80,
    }, {
        .name = "CR5",
        .addr = A_PWR_CR5,
        .reset = 0x00000100,
        .rsvd = 0xfffffeff,
        .pre_write = stm32g474_pwr_held_pre_write,
    },
};

static const MemoryRegionOps stm32g474_pwr_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void stm32g474_pwr_reset_input(void *opaque, int n, int level)
{
    Stm32g474PwrState *s = STM32G474_PWR(opaque);
    bool asserted = level != 0;

    if (asserted == s->peripheral_reset_asserted) {
        return;
    }

    s->peripheral_reset_asserted = asserted;
    if (asserted) {
        register_reset(&s->regs_info[R_PWR_CR1]);
        register_reset(&s->regs_info[R_PWR_SR2]);
    }
}

static void stm32g474_pwr_reset_enter(Object *obj, ResetType type)
{
    Stm32g474PwrState *s = STM32G474_PWR(obj);

    s->resetting = true;
}

static void stm32g474_pwr_reset_hold(Object *obj, ResetType type)
{
    Stm32g474PwrState *s = STM32G474_PWR(obj);

    for (int i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }
    s->peripheral_reset_asserted = false;
}

static void stm32g474_pwr_reset_exit(Object *obj, ResetType type)
{
    Stm32g474PwrState *s = STM32G474_PWR(obj);

    s->resetting = false;
}

static const VMStateDescription vmstate_stm32g474_pwr = {
    .name = TYPE_STM32G474_PWR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474PwrState,
                             STM32G474_PWR_R_MAX),
        VMSTATE_BOOL(peripheral_reset_asserted, Stm32g474PwrState),
        VMSTATE_END_OF_LIST()
    },
};

static void stm32g474_pwr_realize(DeviceState *dev, Error **errp)
{
    Stm32g474PwrState *s = STM32G474_PWR(dev);

    if (!clock_has_source(s->clk)) {
        error_setg(errp, TYPE_STM32G474_PWR
                   ": clk clock must be connected");
    }
}

static void stm32g474_pwr_init(Object *obj)
{
    Stm32g474PwrState *s = STM32G474_PWR(obj);
    DeviceState *dev = DEVICE(obj);

    s->clk = qdev_init_clock_in(dev, "clk", NULL, NULL, 0);
    s->reg_array = register_init_block32(
        dev, stm32g474_pwr_regs_info,
        ARRAY_SIZE(stm32g474_pwr_regs_info), s->regs_info, s->regs,
        &stm32g474_pwr_ops, false, STM32G474_PWR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
    qdev_init_gpio_in_named(dev, stm32g474_pwr_reset_input, "reset", 1);
}

static void stm32g474_pwr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_pwr_realize;
    dc->vmsd = &vmstate_stm32g474_pwr;
    dc->user_creatable = false;
    rc->phases.enter = stm32g474_pwr_reset_enter;
    rc->phases.hold = stm32g474_pwr_reset_hold;
    rc->phases.exit = stm32g474_pwr_reset_exit;
}

static const TypeInfo stm32g474_pwr_info = {
    .name = TYPE_STM32G474_PWR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32g474PwrState),
    .instance_init = stm32g474_pwr_init,
    .class_init = stm32g474_pwr_class_init,
};

static void stm32g474_pwr_register_types(void)
{
    type_register_static(&stm32g474_pwr_info);
}

type_init(stm32g474_pwr_register_types)
