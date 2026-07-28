/*
 * STM32G474 flash memory interface
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/misc/stm32g474_flash.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define STM32G474_FLASH_R_MAX (0x78 / sizeof(uint32_t))

struct Stm32g474FlashState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    uint32_t regs[STM32G474_FLASH_R_MAX];
    RegisterInfo regs_info[STM32G474_FLASH_R_MAX];

    Clock *clk;
    qemu_irq irq;
    MemoryRegion main_flash;
    MemoryRegion flash_size;
    uint8_t *storage;

    bool peripheral_reset_asserted;
    bool resetting;
};

REG32(FLASH_ACR, 0x00)
    FIELD(FLASH_ACR, LATENCY, 0, 4)
    FIELD(FLASH_ACR, PRFTEN, 8, 1)
    FIELD(FLASH_ACR, ICEN, 9, 1)
    FIELD(FLASH_ACR, DCEN, 10, 1)
    FIELD(FLASH_ACR, ICRST, 11, 1)
    FIELD(FLASH_ACR, DCRST, 12, 1)
    FIELD(FLASH_ACR, RUN_PD, 13, 1)
    FIELD(FLASH_ACR, SLEEP_PD, 14, 1)
    FIELD(FLASH_ACR, DBG_SWEN, 18, 1)
REG32(FLASH_PDKEYR, 0x04)
REG32(FLASH_KEYR, 0x08)
REG32(FLASH_OPTKEYR, 0x0c)
REG32(FLASH_SR, 0x10)
    FIELD(FLASH_SR, EOP, 0, 1)
    FIELD(FLASH_SR, OPERR, 1, 1)
    FIELD(FLASH_SR, PROGERR, 3, 1)
    FIELD(FLASH_SR, WRPERR, 4, 1)
    FIELD(FLASH_SR, PGAERR, 5, 1)
    FIELD(FLASH_SR, SIZERR, 6, 1)
    FIELD(FLASH_SR, PGSERR, 7, 1)
    FIELD(FLASH_SR, MISSERR, 8, 1)
    FIELD(FLASH_SR, FASTERR, 9, 1)
    FIELD(FLASH_SR, RDERR, 14, 1)
    FIELD(FLASH_SR, OPTVERR, 15, 1)
    FIELD(FLASH_SR, BSY, 16, 1)
REG32(FLASH_CR, 0x14)
    FIELD(FLASH_CR, PG, 0, 1)
    FIELD(FLASH_CR, PER, 1, 1)
    FIELD(FLASH_CR, MER1, 2, 1)
    FIELD(FLASH_CR, PNB, 3, 7)
    FIELD(FLASH_CR, BKER, 11, 1)
    FIELD(FLASH_CR, MER2, 15, 1)
    FIELD(FLASH_CR, STRT, 16, 1)
    FIELD(FLASH_CR, OPTSTRT, 17, 1)
    FIELD(FLASH_CR, FSTPG, 18, 1)
    FIELD(FLASH_CR, EOPIE, 24, 1)
    FIELD(FLASH_CR, ERRIE, 25, 1)
    FIELD(FLASH_CR, RDERRIE, 26, 1)
    FIELD(FLASH_CR, OBL_LAUNCH, 27, 1)
    FIELD(FLASH_CR, SEC_PROT1, 28, 1)
    FIELD(FLASH_CR, SEC_PROT2, 29, 1)
    FIELD(FLASH_CR, OPTLOCK, 30, 1)
    FIELD(FLASH_CR, LOCK, 31, 1)
REG32(FLASH_OPTR, 0x20)
    FIELD(FLASH_OPTR, RDP, 0, 8)
    FIELD(FLASH_OPTR, DBANK, 22, 1)

static void stm32g474_flash_update_irq(Stm32g474FlashState *s)
{
    bool level;

    if (s->resetting) {
        return;
    }

    level = ((s->regs[R_FLASH_SR] & R_FLASH_SR_EOP_MASK) &&
             (s->regs[R_FLASH_CR] & R_FLASH_CR_EOPIE_MASK)) ||
            ((s->regs[R_FLASH_SR] & R_FLASH_SR_OPERR_MASK) &&
             (s->regs[R_FLASH_CR] & R_FLASH_CR_ERRIE_MASK)) ||
            ((s->regs[R_FLASH_SR] & R_FLASH_SR_RDERR_MASK) &&
             (s->regs[R_FLASH_CR] & R_FLASH_CR_RDERRIE_MASK));
    qemu_set_irq(s->irq, level);
}

static uint64_t stm32g474_flash_acr_pre_write(RegisterInfo *reg,
                                               uint64_t val)
{
    Stm32g474FlashState *s = STM32G474_FLASH(reg->opaque);
    uint32_t old = s->regs[R_FLASH_ACR];

    if (s->peripheral_reset_asserted) {
        return old;
    }

    val = (val & ~R_FLASH_ACR_RUN_PD_MASK) |
          (old & R_FLASH_ACR_RUN_PD_MASK);
    if ((val & R_FLASH_ACR_ICEN_MASK) &&
        (val & R_FLASH_ACR_ICRST_MASK)) {
        val = (val & ~R_FLASH_ACR_ICRST_MASK) |
              (old & R_FLASH_ACR_ICRST_MASK);
    }
    if ((val & R_FLASH_ACR_DCEN_MASK) &&
        (val & R_FLASH_ACR_DCRST_MASK)) {
        val = (val & ~R_FLASH_ACR_DCRST_MASK) |
              (old & R_FLASH_ACR_DCRST_MASK);
    }

    return val;
}

static uint64_t stm32g474_flash_cr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FlashState *s = STM32G474_FLASH(reg->opaque);
    uint32_t old = s->regs[R_FLASH_CR];
    uint32_t set_only = R_FLASH_CR_SEC_PROT1_MASK |
                        R_FLASH_CR_SEC_PROT2_MASK |
                        R_FLASH_CR_OPTLOCK_MASK |
                        R_FLASH_CR_LOCK_MASK;

    if (s->peripheral_reset_asserted || (old & R_FLASH_CR_LOCK_MASK)) {
        return old;
    }

    return val | (old & set_only);
}

static void stm32g474_flash_irq_post_write(RegisterInfo *reg, uint64_t val)
{
    stm32g474_flash_update_irq(STM32G474_FLASH(reg->opaque));
}

static const RegisterAccessInfo stm32g474_flash_regs_info[] = {
    {
        .name = "ACR",
        .addr = A_FLASH_ACR,
        .reset = 0x00040601,
        .rsvd = 0xfffb80f0,
        .unimp = 0x00006000,
        .pre_write = stm32g474_flash_acr_pre_write,
    }, {
        .name = "PDKEYR",
        .addr = A_FLASH_PDKEYR,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "KEYR",
        .addr = A_FLASH_KEYR,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "OPTKEYR",
        .addr = A_FLASH_OPTKEYR,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "SR",
        .addr = A_FLASH_SR,
        .ro = 0x00010000,
        .w1c = 0x0000c3fb,
        .rsvd = 0xfffe3c04,
        .post_write = stm32g474_flash_irq_post_write,
    }, {
        .name = "CR",
        .addr = A_FLASH_CR,
        .reset = 0xc0000000,
        .rsvd = 0x00f87400,
        .unimp = 0x3f078bff,
        .pre_write = stm32g474_flash_cr_pre_write,
        .post_write = stm32g474_flash_irq_post_write,
    }, {
        .name = "OPTR",
        .addr = A_FLASH_OPTR,
        .reset = 0x00400000,
        .ro = 0x7fdf77ff,
        .rsvd = 0x80208800,
    },
};

static const MemoryRegionOps stm32g474_flash_regs_ops = {
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

static uint64_t stm32g474_flash_storage_read(void *opaque, hwaddr offset,
                                             unsigned size)
{
    g_assert_not_reached();
}

static void stm32g474_flash_storage_write(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_STM32G474_FLASH
                  ": programming unavailable at offset 0x%" HWADDR_PRIx
                  " (value 0x%" PRIx64 ", size %u)\n",
                  offset, value, size);
}

static const MemoryRegionOps stm32g474_flash_storage_ops = {
    .read = stm32g474_flash_storage_read,
    .write = stm32g474_flash_storage_write,
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

static void stm32g474_flash_reset_input(void *opaque, int n, int level)
{
    Stm32g474FlashState *s = STM32G474_FLASH(opaque);
    bool asserted = level != 0;
    bool was_resetting;

    if (asserted == s->peripheral_reset_asserted) {
        return;
    }

    s->peripheral_reset_asserted = asserted;
    if (!asserted) {
        return;
    }

    was_resetting = s->resetting;
    s->resetting = true;
    register_reset(&s->regs_info[R_FLASH_ACR]);
    register_reset(&s->regs_info[R_FLASH_PDKEYR]);
    register_reset(&s->regs_info[R_FLASH_KEYR]);
    register_reset(&s->regs_info[R_FLASH_OPTKEYR]);
    register_reset(&s->regs_info[R_FLASH_SR]);
    register_reset(&s->regs_info[R_FLASH_CR]);
    s->resetting = was_resetting;
    stm32g474_flash_update_irq(s);
}

static void stm32g474_flash_reset_enter(Object *obj, ResetType type)
{
    Stm32g474FlashState *s = STM32G474_FLASH(obj);

    s->resetting = true;
}

static void stm32g474_flash_reset_hold(Object *obj, ResetType type)
{
    Stm32g474FlashState *s = STM32G474_FLASH(obj);

    for (int i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }
    s->peripheral_reset_asserted = false;
    qemu_set_irq(s->irq, 0);
}

static void stm32g474_flash_reset_exit(Object *obj, ResetType type)
{
    Stm32g474FlashState *s = STM32G474_FLASH(obj);

    s->resetting = false;
    stm32g474_flash_update_irq(s);
}

static int stm32g474_flash_post_load(void *opaque, int version_id)
{
    stm32g474_flash_update_irq(STM32G474_FLASH(opaque));
    return 0;
}

static const VMStateDescription vmstate_stm32g474_flash = {
    .name = TYPE_STM32G474_FLASH,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stm32g474_flash_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474FlashState,
                             STM32G474_FLASH_R_MAX),
        VMSTATE_BOOL(peripheral_reset_asserted, Stm32g474FlashState),
        VMSTATE_END_OF_LIST()
    },
};

static void stm32g474_flash_realize(DeviceState *dev, Error **errp)
{
    Stm32g474FlashState *s = STM32G474_FLASH(dev);

    if (!clock_has_source(s->clk)) {
        error_setg(errp, TYPE_STM32G474_FLASH
                   ": clk clock must be connected");
        return;
    }

    if (!memory_region_init_rom_device(&s->main_flash, OBJECT(dev),
                                       &stm32g474_flash_storage_ops, s,
                                       "stm32g474.flash",
                                       STM32G474_FLASH_SIZE, errp)) {
        return;
    }
    s->storage = memory_region_get_ram_ptr(&s->main_flash);
    memset(s->storage, 0xff, STM32G474_FLASH_SIZE);

    if (!memory_region_init_rom(&s->flash_size, OBJECT(dev),
                                "stm32g474.flash-size", sizeof(uint32_t),
                                errp)) {
        return;
    }
    stl_le_p(memory_region_get_ram_ptr(&s->flash_size),
             STM32G474_FLASH_SIZE_WORD);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->main_flash);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->flash_size);
}

static void stm32g474_flash_init(Object *obj)
{
    Stm32g474FlashState *s = STM32G474_FLASH(obj);
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->clk = qdev_init_clock_in(dev, "clk", NULL, NULL, 0);
    s->reg_array = register_init_block32(
        dev, stm32g474_flash_regs_info,
        ARRAY_SIZE(stm32g474_flash_regs_info), s->regs_info, s->regs,
        &stm32g474_flash_regs_ops, false, STM32G474_FLASH_IF_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    qdev_init_gpio_in_named(dev, stm32g474_flash_reset_input, "reset", 1);
    sysbus_init_irq(sbd, &s->irq);
}

static void stm32g474_flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_flash_realize;
    dc->vmsd = &vmstate_stm32g474_flash;
    dc->user_creatable = false;
    rc->phases.enter = stm32g474_flash_reset_enter;
    rc->phases.hold = stm32g474_flash_reset_hold;
    rc->phases.exit = stm32g474_flash_reset_exit;
}

static const TypeInfo stm32g474_flash_info = {
    .name = TYPE_STM32G474_FLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32g474FlashState),
    .instance_init = stm32g474_flash_init,
    .class_init = stm32g474_flash_class_init,
};

static void stm32g474_flash_register_types(void)
{
    type_register_static(&stm32g474_flash_info);
}

type_init(stm32g474_flash_register_types)
