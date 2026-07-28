/*
 * STM32G474 extended interrupts and events controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/misc/stm32g474_exti.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define STM32G474_EXTI_NUM_BANKS 2
#define STM32G474_EXTI_NUM_REGS 14

struct Stm32g474ExtiState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_EXTI_NUM_REGS];
    uint32_t regs[STM32G474_EXTI_NUM_REGS];

    Clock *clk;
    qemu_irq irq[STM32G474_EXTI_NUM_LINES];
    qemu_irq event[STM32G474_EXTI_NUM_LINES];

    uint32_t input_levels[STM32G474_EXTI_NUM_BANKS];
    uint32_t swier_rising[STM32G474_EXTI_NUM_BANKS];
    uint32_t raw_pr_write;
    hwaddr raw_pr_addr;
    bool raw_pr_valid;
    bool resetting;
};

REG32(IMR1, 0x00)
    FIELD(IMR1, IM, 0, 32)
REG32(EMR1, 0x04)
    FIELD(EMR1, EM, 0, 32)
REG32(RTSR1, 0x08)
    FIELD(RTSR1, RT0_17, 0, 18)
    FIELD(RTSR1, RT19_22, 19, 4)
    FIELD(RTSR1, RT29_31, 29, 3)
REG32(FTSR1, 0x0c)
    FIELD(FTSR1, FT0_17, 0, 18)
    FIELD(FTSR1, FT19_22, 19, 4)
    FIELD(FTSR1, FT29_31, 29, 3)
REG32(SWIER1, 0x10)
    FIELD(SWIER1, SWI0_17, 0, 18)
    FIELD(SWIER1, SWI19_22, 19, 4)
    FIELD(SWIER1, SWI29_31, 29, 3)
REG32(PR1, 0x14)
    FIELD(PR1, PIF0_17, 0, 18)
    FIELD(PR1, PIF19_22, 19, 4)
    FIELD(PR1, PIF29_31, 29, 3)
REG32(IMR2, 0x20)
    FIELD(IMR2, IM32_37, 0, 6)
    FIELD(IMR2, IM40_43, 8, 4)
REG32(EMR2, 0x24)
    FIELD(EMR2, EM32_37, 0, 6)
    FIELD(EMR2, EM40_43, 8, 4)
REG32(RTSR2, 0x28)
    FIELD(RTSR2, RT32_33, 0, 2)
    FIELD(RTSR2, RT40_41, 8, 2)
REG32(FTSR2, 0x2c)
    FIELD(FTSR2, FT32_33, 0, 2)
    FIELD(FTSR2, FT40_41, 8, 2)
REG32(SWIER2, 0x30)
    FIELD(SWIER2, SWI32_33, 0, 2)
    FIELD(SWIER2, SWI40_41, 8, 2)
REG32(PR2, 0x34)
    FIELD(PR2, PIF32_33, 0, 2)
    FIELD(PR2, PIF40_41, 8, 2)

#define EXTI_BANK_WIDTH 32
#define EXTI_BANK2_NUM_LINES 12
#define EXTI_BANK2_INPUT_MASK 0x00000fffU
#define EXTI_NUM_DEFINED_REGS 12

static const uint32_t stm32g474_exti_configurable_masks[] = {
    0xe07bffff,
    0x00000303,
};

static unsigned int stm32g474_exti_bank_from_addr(hwaddr addr)
{
    return addr >= A_IMR2;
}

static unsigned int stm32g474_exti_reg(unsigned int bank,
                                       unsigned int bank1_reg)
{
    return bank1_reg + bank * (R_IMR2 - R_IMR1);
}

static unsigned int stm32g474_exti_bank_num_lines(unsigned int bank)
{
    return bank == 0 ? EXTI_BANK_WIDTH : EXTI_BANK2_NUM_LINES;
}

static void stm32g474_exti_sync_irq_line(Stm32g474ExtiState *s,
                                         unsigned int line)
{
    unsigned int bank = line / EXTI_BANK_WIDTH;
    unsigned int bit_index = line % EXTI_BANK_WIDTH;
    uint32_t bit = BIT(bit_index);
    uint32_t pending =
        s->regs[stm32g474_exti_reg(bank, R_PR1)] &
        s->regs[stm32g474_exti_reg(bank, R_IMR1)];

    qemu_set_irq(s->irq[line], (pending & bit) != 0);
}

static void stm32g474_exti_sync_irq_bank(Stm32g474ExtiState *s,
                                         unsigned int bank)
{
    unsigned int first = bank * EXTI_BANK_WIDTH;

    for (unsigned int bit = 0;
         bit < stm32g474_exti_bank_num_lines(bank); bit++) {
        stm32g474_exti_sync_irq_line(s, first + bit);
    }
}

static void stm32g474_exti_sync_irqs(Stm32g474ExtiState *s)
{
    for (unsigned int bank = 0;
         bank < STM32G474_EXTI_NUM_BANKS; bank++) {
        stm32g474_exti_sync_irq_bank(s, bank);
    }
}

static void stm32g474_exti_pulse_events(Stm32g474ExtiState *s,
                                        unsigned int bank, uint32_t events)
{
    unsigned int first = bank * EXTI_BANK_WIDTH;

    for (unsigned int bit = 0;
         bit < stm32g474_exti_bank_num_lines(bank); bit++) {
        if (events & BIT(bit)) {
            qemu_irq_pulse(s->event[first + bit]);
        }
    }
}

static void stm32g474_exti_imr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(reg->opaque);
    unsigned int bank = stm32g474_exti_bank_from_addr(reg->access->addr);

    if (!s->resetting) {
        stm32g474_exti_sync_irq_bank(s, bank);
    }
}

static uint64_t stm32g474_exti_swier_pre_write(RegisterInfo *reg,
                                                uint64_t val)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(reg->opaque);
    unsigned int bank = stm32g474_exti_bank_from_addr(reg->access->addr);
    unsigned int index = stm32g474_exti_reg(bank, R_SWIER1);

    s->swier_rising[bank] =
        val & ~s->regs[index] & stm32g474_exti_configurable_masks[bank];
    return val;
}

static void stm32g474_exti_swier_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(reg->opaque);
    unsigned int bank = stm32g474_exti_bank_from_addr(reg->access->addr);
    uint32_t rising = s->swier_rising[bank];
    uint32_t events;
    uint32_t pending;

    s->swier_rising[bank] = 0;
    if (s->resetting) {
        return;
    }

    events = rising & s->regs[stm32g474_exti_reg(bank, R_EMR1)];
    pending = rising;
    s->regs[stm32g474_exti_reg(bank, R_PR1)] |= pending;
    stm32g474_exti_sync_irq_bank(s, bank);
    stm32g474_exti_pulse_events(s, bank, events);
}

static uint64_t stm32g474_exti_pr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(reg->opaque);
    unsigned int bank = stm32g474_exti_bank_from_addr(reg->access->addr);
    uint32_t requested = 0;

    if (s->raw_pr_valid && s->raw_pr_addr == reg->access->addr) {
        requested =
            s->raw_pr_write & stm32g474_exti_configurable_masks[bank];
    }
    s->regs[stm32g474_exti_reg(bank, R_SWIER1)] &= ~requested;
    return val;
}

static void stm32g474_exti_pr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(reg->opaque);
    unsigned int bank = stm32g474_exti_bank_from_addr(reg->access->addr);

    if (!s->resetting) {
        stm32g474_exti_sync_irq_bank(s, bank);
    }
}

static const RegisterAccessInfo stm32g474_exti_regs_info[] = {
    {
        .name = "IMR1",
        .addr = A_IMR1,
        .reset = 0x1f840000,
        .post_write = stm32g474_exti_imr_post_write,
    }, {
        .name = "EMR1",
        .addr = A_EMR1,
    }, {
        .name = "RTSR1",
        .addr = A_RTSR1,
        .rsvd = 0x1f840000,
    }, {
        .name = "FTSR1",
        .addr = A_FTSR1,
        .rsvd = 0x1f840000,
    }, {
        .name = "SWIER1",
        .addr = A_SWIER1,
        .rsvd = 0x1f840000,
        .pre_write = stm32g474_exti_swier_pre_write,
        .post_write = stm32g474_exti_swier_post_write,
    }, {
        .name = "PR1",
        .addr = A_PR1,
        .w1c = 0xe07bffff,
        .rsvd = 0x1f840000,
        .pre_write = stm32g474_exti_pr_pre_write,
        .post_write = stm32g474_exti_pr_post_write,
    }, {
        .name = "IMR2",
        .addr = A_IMR2,
        .reset = 0x00000c3c,
        .rsvd = 0xfffff0c0,
        .post_write = stm32g474_exti_imr_post_write,
    }, {
        .name = "EMR2",
        .addr = A_EMR2,
        .rsvd = 0xfffff0c0,
    }, {
        .name = "RTSR2",
        .addr = A_RTSR2,
        .rsvd = 0xfffffcfc,
    }, {
        .name = "FTSR2",
        .addr = A_FTSR2,
        .rsvd = 0xfffffcfc,
    }, {
        .name = "SWIER2",
        .addr = A_SWIER2,
        .rsvd = 0xfffffcfc,
        .pre_write = stm32g474_exti_swier_pre_write,
        .post_write = stm32g474_exti_swier_post_write,
    }, {
        .name = "PR2",
        .addr = A_PR2,
        .w1c = 0x00000303,
        .rsvd = 0xfffffcfc,
        .pre_write = stm32g474_exti_pr_pre_write,
        .post_write = stm32g474_exti_pr_post_write,
    },
};

G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_exti_regs_info) ==
                EXTI_NUM_DEFINED_REGS);
G_STATIC_ASSERT(STM32G474_EXTI_NUM_REGS == R_PR2 + 1);

static bool stm32g474_exti_is_pr(hwaddr addr)
{
    return addr == A_PR1 || addr == A_PR2;
}

static void stm32g474_exti_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    Stm32g474ExtiState *s =
        STM32G474_EXTI(register_array_get_owner(reg_array));

    if (s->resetting) {
        return;
    }

    if (stm32g474_exti_is_pr(addr)) {
        s->raw_pr_write = value;
        s->raw_pr_addr = addr;
        s->raw_pr_valid = true;
    }
    register_write_memory(opaque, addr, value, size);
    s->raw_pr_valid = false;
}

static const MemoryRegionOps stm32g474_exti_ops = {
    .read = register_read_memory,
    .write = stm32g474_exti_write,
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

static void stm32g474_exti_input(void *opaque, int n, int level)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(opaque);
    unsigned int bank = n / EXTI_BANK_WIDTH;
    unsigned int bit_index = n % EXTI_BANK_WIDTH;
    unsigned int pr = stm32g474_exti_reg(bank, R_PR1);
    uint32_t bit = BIT(bit_index);
    bool high = level != 0;
    bool old = (s->input_levels[bank] & bit) != 0;
    bool selected;

    if (high == old) {
        return;
    }
    if (high) {
        s->input_levels[bank] |= bit;
    } else {
        s->input_levels[bank] &= ~bit;
    }

    if (s->resetting ||
        !(stm32g474_exti_configurable_masks[bank] & bit)) {
        return;
    }

    selected = high ?
        (s->regs[stm32g474_exti_reg(bank, R_RTSR1)] & bit) != 0 :
        (s->regs[stm32g474_exti_reg(bank, R_FTSR1)] & bit) != 0;
    if (!selected) {
        return;
    }

    s->regs[pr] |= bit;
    stm32g474_exti_sync_irq_line(s, n);
    if (s->regs[stm32g474_exti_reg(bank, R_EMR1)] & bit) {
        qemu_irq_pulse(s->event[n]);
    }
}

static void stm32g474_exti_reset_registers(Stm32g474ExtiState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->swier_rising, 0, sizeof(s->swier_rising));
    s->raw_pr_write = 0;
    s->raw_pr_addr = 0;
    s->raw_pr_valid = false;

    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_exti_regs_info); i++) {
        unsigned int index =
            stm32g474_exti_regs_info[i].addr / sizeof(uint32_t);

        register_reset(&s->regs_info[index]);
    }
}

static void stm32g474_exti_reset_enter(Object *obj, ResetType type)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(obj);

    s->resetting = true;
    stm32g474_exti_reset_registers(s);
}

static void stm32g474_exti_reset_hold(Object *obj, ResetType type)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(obj);

    stm32g474_exti_sync_irqs(s);
}

static void stm32g474_exti_reset_exit(Object *obj, ResetType type)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(obj);

    s->resetting = false;
    stm32g474_exti_sync_irqs(s);
}

static bool stm32g474_exti_migration_valid(Stm32g474ExtiState *s)
{
    for (unsigned int i = R_PR1 + 1; i < R_IMR2; i++) {
        if (s->regs[i]) {
            return false;
        }
    }
    if (s->input_levels[1] & ~EXTI_BANK2_INPUT_MASK) {
        return false;
    }

    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_exti_regs_info); i++) {
        const RegisterAccessInfo *access = &stm32g474_exti_regs_info[i];
        unsigned int index = access->addr / sizeof(uint32_t);

        if (s->regs[index] & access->rsvd) {
            return false;
        }
    }
    return true;
}

static int stm32g474_exti_post_load(void *opaque, int version_id)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(opaque);

    if (!stm32g474_exti_migration_valid(s)) {
        return -EINVAL;
    }

    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_exti_regs_info); i++) {
        const RegisterAccessInfo *access = &stm32g474_exti_regs_info[i];
        unsigned int index = access->addr / sizeof(uint32_t);

        s->regs[index] = (s->regs[index] & ~access->rsvd) |
                         (access->reset & access->rsvd);
    }
    memset(s->swier_rising, 0, sizeof(s->swier_rising));
    s->raw_pr_write = 0;
    s->raw_pr_addr = 0;
    s->raw_pr_valid = false;
    s->resetting = false;
    stm32g474_exti_sync_irqs(s);
    return 0;
}

static const VMStateDescription vmstate_stm32g474_exti = {
    .name = TYPE_STM32G474_EXTI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stm32g474_exti_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474ExtiState,
                             STM32G474_EXTI_NUM_REGS),
        VMSTATE_UINT32_ARRAY(input_levels, Stm32g474ExtiState,
                             STM32G474_EXTI_NUM_BANKS),
        VMSTATE_CLOCK(clk, Stm32g474ExtiState),
        VMSTATE_END_OF_LIST()
    },
};

static void stm32g474_exti_realize(DeviceState *dev, Error **errp)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(dev);

    if (!clock_has_source(s->clk)) {
        error_setg(errp, TYPE_STM32G474_EXTI
                   ": clk clock must be connected");
    }
}

static void stm32g474_exti_init(Object *obj)
{
    Stm32g474ExtiState *s = STM32G474_EXTI(obj);
    DeviceState *dev = DEVICE(obj);

    s->reg_array = register_init_block32(
        dev, stm32g474_exti_regs_info,
        ARRAY_SIZE(stm32g474_exti_regs_info), s->regs_info, s->regs,
        &stm32g474_exti_ops, false, STM32G474_EXTI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
    s->clk = qdev_init_clock_in(dev, "clk", NULL, NULL, 0);
    qdev_init_gpio_in_named(dev, stm32g474_exti_input, "line-in",
                            STM32G474_EXTI_NUM_LINES);
    qdev_init_gpio_out_named(dev, s->event, "event",
                             STM32G474_EXTI_NUM_LINES);
    for (unsigned int i = 0; i < STM32G474_EXTI_NUM_LINES; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void stm32g474_exti_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_exti_realize;
    dc->vmsd = &vmstate_stm32g474_exti;
    dc->user_creatable = false;
    rc->phases.enter = stm32g474_exti_reset_enter;
    rc->phases.hold = stm32g474_exti_reset_hold;
    rc->phases.exit = stm32g474_exti_reset_exit;
}

static const TypeInfo stm32g474_exti_info = {
    .name = TYPE_STM32G474_EXTI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32g474ExtiState),
    .instance_init = stm32g474_exti_init,
    .class_init = stm32g474_exti_class_init,
};

static void stm32g474_exti_register_types(void)
{
    type_register_static(&stm32g474_exti_info);
}

type_init(stm32g474_exti_register_types)
