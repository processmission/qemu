/*
 * STC8G UART1
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/char/stc8g_uart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu-qom.h"

REG8(SCON, 0x00)
FIELD(SCON, RI, 0, 1)
FIELD(SCON, TI, 1, 1)
FIELD(SCON, REN, 4, 1)
REG8(SBUF, 0x01)
REG8(SADDR, 0x11)
REG8(SADEN, 0x21)

#define STC8G_UART_SFR_SIZE (A_SADEN + 1)

enum Stc8gUARTRegister {
    STC8G_UART_SCON,
    STC8G_UART_SBUF,
    STC8G_UART_SADDR,
    STC8G_UART_SADEN,
    STC8G_UART_REGS,
};

struct Stc8gUARTState {
    SysBusDevice parent_obj;

    MCS51CPU *cpu;
    CharFrontend chr;
    qemu_irq irq;
    MemoryRegion sfr;
    RegisterInfoArray *reg_array[STC8G_UART_REGS];
    RegisterInfo regs_info[STC8G_UART_REGS];
    uint8_t regs[STC8G_UART_REGS];
    uint8_t rx_buffer;
};

static void stc8g_uart_update_irq(Stc8gUARTState *s)
{
    qemu_set_irq(s->irq, FIELD_EX8(s->regs[STC8G_UART_SCON], SCON, TI) ||
                         FIELD_EX8(s->regs[STC8G_UART_SCON], SCON, RI));
}

static int stc8g_uart_can_receive(void *opaque)
{
    Stc8gUARTState *s = opaque;

    return FIELD_EX8(s->regs[STC8G_UART_SCON], SCON, REN) &&
           !FIELD_EX8(s->regs[STC8G_UART_SCON], SCON, RI);
}

static void stc8g_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Stc8gUARTState *s = opaque;

    if (size && stc8g_uart_can_receive(s)) {
        s->rx_buffer = buf[0];
        s->regs[STC8G_UART_SCON] =
            FIELD_DP8(s->regs[STC8G_UART_SCON], SCON, RI, 1);
        stc8g_uart_update_irq(s);
    }
}

static void stc8g_uart_scon_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gUARTState *s = STC8G_UART(reg->opaque);

    stc8g_uart_update_irq(s);
    if (!device_is_in_reset(DEVICE(s))) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static uint64_t stc8g_uart_sbuf_post_read(RegisterInfo *reg,
                                          uint64_t value)
{
    Stc8gUARTState *s = STC8G_UART(reg->opaque);

    return s->rx_buffer;
}

static void stc8g_uart_sbuf_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gUARTState *s = STC8G_UART(reg->opaque);
    uint8_t byte = value;

    if (device_is_in_reset(DEVICE(s))) {
        return;
    }

    qemu_chr_fe_write_all(&s->chr, &byte, 1);
    s->regs[STC8G_UART_SCON] =
        FIELD_DP8(s->regs[STC8G_UART_SCON], SCON, TI, 1);
    stc8g_uart_update_irq(s);
}

static const RegisterAccessInfo stc8g_uart_regs_info[] = {
    {   .name = "SCON", .addr = 0,
        .post_write = stc8g_uart_scon_post_write,
    },{ .name = "SBUF", .addr = 0,
        .post_read = stc8g_uart_sbuf_post_read,
        .post_write = stc8g_uart_sbuf_post_write,
    },{ .name = "SADDR", .addr = 0,
    },{ .name = "SADEN", .addr = 0,
    },
};

static const hwaddr stc8g_uart_reg_offsets[] = {
    A_SCON, A_SBUF, A_SADDR, A_SADEN,
};

static const MemoryRegionOps stc8g_uart_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_uart_reset(DeviceState *dev)
{
    Stc8gUARTState *s = STC8G_UART(dev);
    unsigned index;

    s->rx_buffer = 0;
    for (index = 0; index < ARRAY_SIZE(stc8g_uart_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    stc8g_uart_update_irq(s);
}

static int stc8g_uart_post_load(void *opaque, int version_id)
{
    Stc8gUARTState *s = opaque;

    stc8g_uart_update_irq(s);
    if (stc8g_uart_can_receive(s)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
    return 0;
}

static const VMStateDescription stc8g_uart_vmstate = {
    .name = "stc8g.uart1",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_uart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gUARTState, STC8G_UART_REGS),
        VMSTATE_UINT8(rx_buffer, Stc8gUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_uart_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc8gUARTState, cpu, TYPE_MCS51_CPU,
                     MCS51CPU *),
    DEFINE_PROP_CHR("chardev", Stc8gUARTState, chr),
};

static void stc8g_uart_realize(DeviceState *dev, Error **errp)
{
    Stc8gUARTState *s = STC8G_UART(dev);

    if (!s->cpu) {
        error_setg(errp, "stc8g-uart requires a CPU link");
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, stc8g_uart_can_receive,
                             stc8g_uart_receive, NULL, NULL, s, NULL,
                             true);
}

static void stc8g_uart_init(Object *obj)
{
    Stc8gUARTState *s = STC8G_UART(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_uart_regs_info) !=
                      STC8G_UART_REGS);
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_uart_reg_offsets) !=
                      STC8G_UART_REGS);
    memory_region_init(&s->sfr, obj, "stc8g.uart1-sfr",
                       STC8G_UART_SFR_SIZE);
    for (index = 0; index < ARRAY_SIZE(stc8g_uart_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_uart_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index],
            &stc8g_uart_ops, false, 1);
        memory_region_add_subregion(&s->sfr,
                                    stc8g_uart_reg_offsets[index],
                                    &s->reg_array[index]->mem);
    }
    sysbus_init_mmio(sbd, &s->sfr);
    sysbus_init_irq(sbd, &s->irq);
}

static void stc8g_uart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_uart_realize;
    device_class_set_legacy_reset(dc, stc8g_uart_reset);
    device_class_set_props(dc, stc8g_uart_properties);
    dc->vmsd = &stc8g_uart_vmstate;
}

static const TypeInfo stc8g_uart_type = {
    .name = TYPE_STC8G_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gUARTState),
    .instance_init = stc8g_uart_init,
    .class_init = stc8g_uart_class_init,
};

static void stc8g_uart_register_types(void)
{
    type_register_static(&stc8g_uart_type);
}

type_init(stc8g_uart_register_types)
