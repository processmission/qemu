/*
 * K230 GPIO register block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_gpio.h"

#define K230_GPIO_SWPORTA_DR       0x00
#define K230_GPIO_SWPORTB_DR       0x0c
#define K230_GPIO_SWPORTC_DR       0x18
#define K230_GPIO_SWPORTD_DR       0x24
#define K230_GPIO_INTSTATUS        0x40
#define K230_GPIO_PORTA_EOI        0x4c
#define K230_GPIO_EXT_PORTA        0x50
#define K230_GPIO_EXT_PORTB        0x54
#define K230_GPIO_EXT_PORTC        0x58
#define K230_GPIO_EXT_PORTD        0x5c
#define K230_GPIO_INTSTATUS_V2     0x3c
#define K230_GPIO_PORTA_EOI_V2     0x40

static uint64_t k230_gpio_read_bytes(uint8_t *regs, hwaddr addr,
                                     unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_gpio_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                  unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_gpio_reg_read32(K230GpioState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_gpio_reg_write32(K230GpioState *s, hwaddr addr, uint32_t val)
{
    stl_le_p(s->regs + addr, val);
}

static uint64_t k230_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230GpioState *s = K230_GPIO(opaque);

    if (size == 4) {
        switch (addr) {
        case K230_GPIO_EXT_PORTA:
            return k230_gpio_reg_read32(s, K230_GPIO_SWPORTA_DR);
        case K230_GPIO_EXT_PORTB:
            return k230_gpio_reg_read32(s, K230_GPIO_SWPORTB_DR);
        case K230_GPIO_EXT_PORTC:
            return k230_gpio_reg_read32(s, K230_GPIO_SWPORTC_DR);
        case K230_GPIO_EXT_PORTD:
            return k230_gpio_reg_read32(s, K230_GPIO_SWPORTD_DR);
        default:
            break;
        }
    }

    return k230_gpio_read_bytes(s->regs, addr, size);
}

static void k230_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    K230GpioState *s = K230_GPIO(opaque);

    k230_gpio_write_bytes(s->regs, addr, val, size);

    if (size == 4 && addr == K230_GPIO_PORTA_EOI) {
        k230_gpio_reg_write32(s, K230_GPIO_INTSTATUS,
                              k230_gpio_reg_read32(s, K230_GPIO_INTSTATUS) &
                              ~(uint32_t)val);
    } else if (size == 4 && addr == K230_GPIO_PORTA_EOI_V2) {
        k230_gpio_reg_write32(s, K230_GPIO_INTSTATUS_V2,
                              k230_gpio_reg_read32(s, K230_GPIO_INTSTATUS_V2) &
                              ~(uint32_t)val);
    }
}

static const MemoryRegionOps k230_gpio_ops = {
    .read = k230_gpio_read,
    .write = k230_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_gpio_reset(DeviceState *dev)
{
    K230GpioState *s = K230_GPIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_k230_gpio = {
    .name = TYPE_K230_GPIO,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230GpioState, K230_GPIO_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_gpio_realize(DeviceState *dev, Error **errp)
{
    K230GpioState *s = K230_GPIO(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_gpio_ops, s,
                          TYPE_K230_GPIO, K230_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_gpio_realize;
    device_class_set_legacy_reset(dc, k230_gpio_reset);
    dc->vmsd = &vmstate_k230_gpio;
    dc->desc = "K230 GPIO register block";
}

static const TypeInfo k230_gpio_type_info = {
    .name = TYPE_K230_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230GpioState),
    .class_init = k230_gpio_class_init,
};

static void k230_register_gpio_types(void)
{
    type_register_static(&k230_gpio_type_info);
}

type_init(k230_register_gpio_types)
