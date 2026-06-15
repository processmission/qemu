/*
 * Rockchip GPIO bank emulation
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/gpio/rockchip_gpio.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define ROCKCHIP_GPIO_VERSION_V2 0x01000c2b

REG32(SWPORT_DR_L, 0x00)
REG32(SWPORT_DR_H, 0x04)
REG32(SWPORT_DDR_L, 0x08)
REG32(SWPORT_DDR_H, 0x0c)
REG32(INT_EN_L, 0x10)
REG32(INT_EN_H, 0x14)
REG32(INT_MASK_L, 0x18)
REG32(INT_MASK_H, 0x1c)
REG32(INT_TYPE_L, 0x20)
REG32(INT_TYPE_H, 0x24)
REG32(INT_POLARITY_L, 0x28)
REG32(INT_POLARITY_H, 0x2c)
REG32(INT_BOTHEDGE_L, 0x30)
REG32(INT_BOTHEDGE_H, 0x34)
REG32(DEBOUNCE_L, 0x38)
REG32(DEBOUNCE_H, 0x3c)
REG32(DBCLK_DIV_EN_L, 0x40)
REG32(DBCLK_DIV_EN_H, 0x44)
REG32(DBCLK_DIV_CON, 0x48)
REG32(INT_STATUS_L, 0x50)
REG32(INT_STATUS_H, 0x54)
REG32(INT_RAWSTATUS_L, 0x58)
REG32(INT_RAWSTATUS_H, 0x5c)
REG32(PORT_EOI_L, 0x60)
REG32(PORT_EOI_H, 0x64)
REG32(EXT_PORT_L, 0x70)
REG32(EXT_PORT_H, 0x74)
REG32(VERSION_ID, 0x78)

static uint32_t rockchip_gpio_get_pair(RockchipGPIOState *s, hwaddr low_addr)
{
    return (s->regs[(low_addr >> 2)] & 0xffff) |
           ((s->regs[((low_addr + 4) >> 2)] & 0xffff) << 16);
}

static void rockchip_gpio_set_pair(RockchipGPIOState *s, hwaddr low_addr,
                                   uint32_t value)
{
    s->regs[(low_addr >> 2)] = value & 0xffff;
    s->regs[((low_addr + 4) >> 2)] = (value >> 16) & 0xffff;
}

static void rockchip_gpio_update_irq(RockchipGPIOState *s)
{
    uint32_t status = rockchip_gpio_get_pair(s, A_INT_STATUS_L);

    qemu_set_irq(s->irq, status != 0);
}

static void rockchip_gpio_update_outputs(RockchipGPIOState *s)
{
    uint32_t dr = rockchip_gpio_get_pair(s, A_SWPORT_DR_L);
    uint32_t ddr = rockchip_gpio_get_pair(s, A_SWPORT_DDR_L);
    uint32_t ext = (s->input_level & ~ddr) | (dr & ddr);

    rockchip_gpio_set_pair(s, A_EXT_PORT_L, ext);

    for (unsigned int n = 0; n < ROCKCHIP_GPIO_PINS; n++) {
        qemu_set_irq(s->output[n], !!(ext & BIT(n)));
    }
}

static void rockchip_gpio_raise_for_level(RockchipGPIOState *s,
                                          uint32_t old_level,
                                          uint32_t new_level)
{
    uint32_t enabled = rockchip_gpio_get_pair(s, A_INT_EN_L);
    uint32_t ddr = rockchip_gpio_get_pair(s, A_SWPORT_DDR_L);
    uint32_t type = rockchip_gpio_get_pair(s, A_INT_TYPE_L);
    uint32_t polarity = rockchip_gpio_get_pair(s, A_INT_POLARITY_L);
    uint32_t bothedge = rockchip_gpio_get_pair(s, A_INT_BOTHEDGE_L);
    uint32_t changed = old_level ^ new_level;
    uint32_t high = new_level;
    uint32_t rising = changed & new_level;
    uint32_t falling = changed & old_level;
    uint32_t active = enabled & ~ddr;
    uint32_t edge = type & active;
    uint32_t level = ~type & active;
    uint32_t edge_pending;
    uint32_t level_pending;
    uint32_t mask = rockchip_gpio_get_pair(s, A_INT_MASK_L);
    uint32_t raw;

    edge_pending = (bothedge & changed & active) |
                   (edge & polarity & rising) |
                   (edge & ~polarity & falling);
    level_pending = (level & polarity & high) |
                    (level & ~polarity & ~high);

    raw = rockchip_gpio_get_pair(s, A_INT_RAWSTATUS_L);
    raw = (raw & active & ~level) | level_pending | edge_pending;

    rockchip_gpio_set_pair(s, A_INT_RAWSTATUS_L, raw);
    rockchip_gpio_set_pair(s, A_INT_STATUS_L, raw & ~mask);
    rockchip_gpio_update_irq(s);
}

static uint64_t rockchip_gpio_we16_prew(RegisterInfo *reg, uint64_t val)
{
    uint32_t old = *(uint32_t *)reg->data & 0xffff;
    uint32_t mask = extract32(val, 16, 16);
    uint32_t data = val & 0xffff;

    return (old & ~mask) | (data & mask);
}

static void rockchip_gpio_data_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(reg->opaque);
    uint32_t old_ext = rockchip_gpio_get_pair(s, A_EXT_PORT_L);

    rockchip_gpio_update_outputs(s);
    rockchip_gpio_raise_for_level(s, old_ext,
                                  rockchip_gpio_get_pair(s, A_EXT_PORT_L));
}

static void rockchip_gpio_irqcfg_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(reg->opaque);

    rockchip_gpio_raise_for_level(s, rockchip_gpio_get_pair(s, A_EXT_PORT_L),
                                  rockchip_gpio_get_pair(s, A_EXT_PORT_L));
    rockchip_gpio_update_irq(s);
}

static void rockchip_gpio_eoi_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(reg->opaque);
    uint32_t ack = rockchip_gpio_get_pair(s, A_PORT_EOI_L);
    uint32_t mask = rockchip_gpio_get_pair(s, A_INT_MASK_L);
    uint32_t raw = rockchip_gpio_get_pair(s, A_INT_RAWSTATUS_L) & ~ack;

    rockchip_gpio_set_pair(s, A_INT_RAWSTATUS_L, raw);
    rockchip_gpio_set_pair(s, A_INT_STATUS_L, raw & ~mask);
    rockchip_gpio_set_pair(s, A_PORT_EOI_L, 0);
    rockchip_gpio_raise_for_level(s, rockchip_gpio_get_pair(s, A_EXT_PORT_L),
                                  rockchip_gpio_get_pair(s, A_EXT_PORT_L));
    rockchip_gpio_update_irq(s);
}

static void rockchip_gpio_set_input(void *opaque, int n, int level)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(opaque);
    uint32_t old_ext;

    if (n < 0 || n >= ROCKCHIP_GPIO_PINS || level < 0) {
        return;
    }

    old_ext = rockchip_gpio_get_pair(s, A_EXT_PORT_L);
    if (level) {
        s->input_level |= BIT(n);
    } else {
        s->input_level &= ~BIT(n);
    }

    rockchip_gpio_update_outputs(s);
    rockchip_gpio_raise_for_level(s, old_ext,
                                  rockchip_gpio_get_pair(s, A_EXT_PORT_L));
}

static uint64_t rockchip_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    return register_read_memory(opaque, addr, size);
}

static void rockchip_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    register_write_memory(opaque, addr, value, size);
}

static const MemoryRegionOps rockchip_gpio_ops = {
    .read = rockchip_gpio_read,
    .write = rockchip_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

#define WE16(_name, _addr, _postw) \
    { .name = (_name), .addr = (_addr), .pre_write = rockchip_gpio_we16_prew, \
      .post_write = (_postw) }
#define RO_REG(_name, _addr, _reset) \
    { .name = (_name), .addr = (_addr), .reset = (_reset), .ro = 0xffffffff }

static const RegisterAccessInfo rockchip_gpio_regs_info[] = {
    WE16("SWPORT_DR_L", A_SWPORT_DR_L, rockchip_gpio_data_postw),
    WE16("SWPORT_DR_H", A_SWPORT_DR_H, rockchip_gpio_data_postw),
    WE16("SWPORT_DDR_L", A_SWPORT_DDR_L, rockchip_gpio_data_postw),
    WE16("SWPORT_DDR_H", A_SWPORT_DDR_H, rockchip_gpio_data_postw),
    WE16("INT_EN_L", A_INT_EN_L, rockchip_gpio_irqcfg_postw),
    WE16("INT_EN_H", A_INT_EN_H, rockchip_gpio_irqcfg_postw),
    WE16("INT_MASK_L", A_INT_MASK_L, rockchip_gpio_irqcfg_postw),
    WE16("INT_MASK_H", A_INT_MASK_H, rockchip_gpio_irqcfg_postw),
    WE16("INT_TYPE_L", A_INT_TYPE_L, rockchip_gpio_irqcfg_postw),
    WE16("INT_TYPE_H", A_INT_TYPE_H, rockchip_gpio_irqcfg_postw),
    WE16("INT_POLARITY_L", A_INT_POLARITY_L, rockchip_gpio_irqcfg_postw),
    WE16("INT_POLARITY_H", A_INT_POLARITY_H, rockchip_gpio_irqcfg_postw),
    WE16("INT_BOTHEDGE_L", A_INT_BOTHEDGE_L, rockchip_gpio_irqcfg_postw),
    WE16("INT_BOTHEDGE_H", A_INT_BOTHEDGE_H, rockchip_gpio_irqcfg_postw),
    WE16("DEBOUNCE_L", A_DEBOUNCE_L, NULL),
    WE16("DEBOUNCE_H", A_DEBOUNCE_H, NULL),
    WE16("DBCLK_DIV_EN_L", A_DBCLK_DIV_EN_L, NULL),
    WE16("DBCLK_DIV_EN_H", A_DBCLK_DIV_EN_H, NULL),
    { .name = "DBCLK_DIV_CON", .addr = A_DBCLK_DIV_CON },
    RO_REG("INT_STATUS_L", A_INT_STATUS_L, 0),
    RO_REG("INT_STATUS_H", A_INT_STATUS_H, 0),
    RO_REG("INT_RAWSTATUS_L", A_INT_RAWSTATUS_L, 0),
    RO_REG("INT_RAWSTATUS_H", A_INT_RAWSTATUS_H, 0),
    WE16("PORT_EOI_L", A_PORT_EOI_L, rockchip_gpio_eoi_postw),
    WE16("PORT_EOI_H", A_PORT_EOI_H, rockchip_gpio_eoi_postw),
    RO_REG("EXT_PORT_L", A_EXT_PORT_L, 0),
    RO_REG("EXT_PORT_H", A_EXT_PORT_H, 0),
    RO_REG("VERSION_ID", A_VERSION_ID, ROCKCHIP_GPIO_VERSION_V2),
};

#undef RO_REG
#undef WE16

static void rockchip_gpio_reset(DeviceState *dev)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(dev);

    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }

    s->input_level = 0;
    rockchip_gpio_update_outputs(s);
    rockchip_gpio_update_irq(s);
}

static void rockchip_gpio_init(Object *obj)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    s->reg_array = register_init_block32(dev, rockchip_gpio_regs_info,
                                         ARRAY_SIZE(rockchip_gpio_regs_info),
                                         s->regs_info, s->regs,
                                         &rockchip_gpio_ops, false,
                                         ROCKCHIP_GPIO_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in(dev, rockchip_gpio_set_input, ROCKCHIP_GPIO_PINS);
    qdev_init_gpio_out_named(dev, s->output, "gpio-out", ROCKCHIP_GPIO_PINS);
}

static const VMStateDescription vmstate_rockchip_gpio = {
    .name = TYPE_ROCKCHIP_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RockchipGPIOState, ROCKCHIP_GPIO_NR_REGS),
        VMSTATE_UINT32(input_level, RockchipGPIOState),
        VMSTATE_END_OF_LIST(),
    }
};

static void rockchip_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Rockchip GPIO bank";
    device_class_set_legacy_reset(dc, rockchip_gpio_reset);
    dc->vmsd = &vmstate_rockchip_gpio;
}

static const TypeInfo rockchip_gpio_info = {
    .name = TYPE_ROCKCHIP_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipGPIOState),
    .instance_init = rockchip_gpio_init,
    .class_init = rockchip_gpio_class_init,
};

static void rockchip_gpio_register_types(void)
{
    type_register_static(&rockchip_gpio_info);
}

type_init(rockchip_gpio_register_types)
