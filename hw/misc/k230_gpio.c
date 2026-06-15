/*
 * K230 GPIO register block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/misc/k230_gpio.h"

#define K230_GPIO_SWPORTA_DR       0x00
#define K230_GPIO_SWPORTA_DDR      0x04
#define K230_GPIO_SWPORTB_DR       0x0c
#define K230_GPIO_SWPORTB_DDR      0x10
#define K230_GPIO_SWPORTC_DR       0x18
#define K230_GPIO_SWPORTC_DDR      0x1c
#define K230_GPIO_SWPORTD_DR       0x24
#define K230_GPIO_SWPORTD_DDR      0x28
#define K230_GPIO_INTEN            0x30
#define K230_GPIO_INTMASK          0x34
#define K230_GPIO_INTTYPE_LEVEL    0x38
#define K230_GPIO_INT_POLARITY     0x3c
#define K230_GPIO_INTSTATUS        0x40
#define K230_GPIO_PORTA_DEBOUNCE   0x48
#define K230_GPIO_PORTA_EOI        0x4c
#define K230_GPIO_EXT_PORTA        0x50
#define K230_GPIO_EXT_PORTB        0x54
#define K230_GPIO_EXT_PORTC        0x58
#define K230_GPIO_EXT_PORTD        0x5c
#define K230_GPIO_INTTYPE_BOTHEDGE 0x68
#define K230_GPIO_INTSTATUS_V2     0x3c
#define K230_GPIO_PORTA_EOI_V2     0x40

#define K230_GPIO_INTMASK_V2       0x44
#define K230_GPIO_INTTYPE_LEVEL_V2 0x34
#define K230_GPIO_INT_POLARITY_V2  0x38

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

static uint32_t k230_gpio_port_value(K230GpioState *s, hwaddr dr,
                                     hwaddr ddr, uint32_t input)
{
    uint32_t data = k230_gpio_reg_read32(s, dr);
    uint32_t dir = k230_gpio_reg_read32(s, ddr);

    return (data & dir) | (input & ~dir);
}

static uint32_t k230_gpio_pending(K230GpioState *s)
{
    uint32_t status = k230_gpio_reg_read32(s, K230_GPIO_INTSTATUS);
    uint32_t enable = k230_gpio_reg_read32(s, K230_GPIO_INTEN);
    uint32_t mask = k230_gpio_reg_read32(s, K230_GPIO_INTMASK);

    return status & enable & ~mask;
}

static void k230_gpio_update_irq(K230GpioState *s)
{
    uint32_t pending = k230_gpio_pending(s);

    for (int i = 0; i < K230_GPIO_IRQ_COUNT; i++) {
        qemu_set_irq(s->irq[i], extract32(pending, i, 1));
    }
}

static void k230_gpio_update_level_status(K230GpioState *s)
{
    uint32_t status = k230_gpio_reg_read32(s, K230_GPIO_INTSTATUS);
    uint32_t type = k230_gpio_reg_read32(s, K230_GPIO_INTTYPE_LEVEL);
    uint32_t polarity = k230_gpio_reg_read32(s, K230_GPIO_INT_POLARITY);
    uint32_t bothedge = k230_gpio_reg_read32(s, K230_GPIO_INTTYPE_BOTHEDGE);
    uint32_t level_mask = ~(type | bothedge);
    uint32_t active = (s->input & polarity) | (~s->input & ~polarity);

    status &= ~level_mask;
    status |= active & level_mask;
    k230_gpio_reg_write32(s, K230_GPIO_INTSTATUS, status);
}

static void k230_gpio_set_input(void *opaque, int line, int level)
{
    K230GpioState *s = K230_GPIO(opaque);
    uint32_t old = s->input;
    uint32_t mask = BIT(line);
    uint32_t changed;
    uint32_t rising;
    uint32_t falling;
    uint32_t type;
    uint32_t polarity;
    uint32_t bothedge;
    uint32_t status;

    if (level) {
        s->input |= mask;
    } else {
        s->input &= ~mask;
    }

    changed = old ^ s->input;
    if (!changed) {
        return;
    }

    type = k230_gpio_reg_read32(s, K230_GPIO_INTTYPE_LEVEL);
    polarity = k230_gpio_reg_read32(s, K230_GPIO_INT_POLARITY);
    bothedge = k230_gpio_reg_read32(s, K230_GPIO_INTTYPE_BOTHEDGE);
    status = k230_gpio_reg_read32(s, K230_GPIO_INTSTATUS);
    rising = changed & s->input;
    falling = changed & ~s->input;

    status |= changed & bothedge;
    status |= rising & type & polarity;
    status |= falling & type & ~polarity;
    k230_gpio_reg_write32(s, K230_GPIO_INTSTATUS, status);
    k230_gpio_update_level_status(s);
    s->last_input = s->input;
    k230_gpio_update_irq(s);
}

static uint64_t k230_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230GpioState *s = K230_GPIO(opaque);

    if (size == 4) {
        switch (addr) {
        case K230_GPIO_EXT_PORTA:
            return k230_gpio_port_value(s, K230_GPIO_SWPORTA_DR,
                                        K230_GPIO_SWPORTA_DDR, s->input);
        case K230_GPIO_EXT_PORTB:
            return k230_gpio_port_value(s, K230_GPIO_SWPORTB_DR,
                                        K230_GPIO_SWPORTB_DDR, 0);
        case K230_GPIO_EXT_PORTC:
            return k230_gpio_port_value(s, K230_GPIO_SWPORTC_DR,
                                        K230_GPIO_SWPORTC_DDR, 0);
        case K230_GPIO_EXT_PORTD:
            return k230_gpio_port_value(s, K230_GPIO_SWPORTD_DR,
                                        K230_GPIO_SWPORTD_DDR, 0);
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
        k230_gpio_update_level_status(s);
    } else if (size == 4 && addr == K230_GPIO_PORTA_EOI_V2) {
        k230_gpio_reg_write32(s, K230_GPIO_INTSTATUS_V2,
                              k230_gpio_reg_read32(s, K230_GPIO_INTSTATUS_V2) &
                              ~(uint32_t)val);
    } else if (size == 4 &&
               (addr == K230_GPIO_INTEN ||
                addr == K230_GPIO_INTMASK ||
                addr == K230_GPIO_INTTYPE_LEVEL ||
                addr == K230_GPIO_INT_POLARITY ||
                addr == K230_GPIO_INTTYPE_BOTHEDGE ||
                addr == K230_GPIO_INTMASK_V2 ||
                addr == K230_GPIO_INTTYPE_LEVEL_V2 ||
                addr == K230_GPIO_INT_POLARITY_V2)) {
        k230_gpio_update_level_status(s);
    }

    k230_gpio_update_irq(s);
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
    s->input = 0;
    s->last_input = 0;
    k230_gpio_update_irq(s);
}

static const VMStateDescription vmstate_k230_gpio = {
    .name = TYPE_K230_GPIO,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230GpioState, K230_GPIO_SIZE),
        VMSTATE_UINT32(input, K230GpioState),
        VMSTATE_UINT32(last_input, K230GpioState),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_gpio_realize(DeviceState *dev, Error **errp)
{
    K230GpioState *s = K230_GPIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    for (int i = 0; i < K230_GPIO_IRQ_COUNT; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_gpio_ops, s,
                          TYPE_K230_GPIO, K230_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    qdev_init_gpio_in_named(dev, k230_gpio_set_input, "input",
                            K230_GPIO_IRQ_COUNT);
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
