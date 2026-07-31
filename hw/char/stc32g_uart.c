/*
 * STC32G UART1
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/char/stc32g_uart.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "target/mcs51/cpu-qom.h"

REG8(SCON, 0)
FIELD(SCON, RI, 0, 1)
FIELD(SCON, TI, 1, 1)
FIELD(SCON, REN, 4, 1)
REG8(SBUF, 1)

#define STC32G_UART_REGS (R_SBUF + 1)

struct Stc32gUARTState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    CharFrontend chr;
    qemu_irq irq;
    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STC32G_UART_REGS];
    uint8_t regs[STC32G_UART_REGS];
    uint8_t rx_buffer;
};

static void stc32g_uart_update_irq(Stc32gUARTState *s)
{
    qemu_set_irq(s->irq, FIELD_EX8(s->regs[R_SCON], SCON, TI) ||
                         FIELD_EX8(s->regs[R_SCON], SCON, RI));
}

static int stc32g_uart_can_receive(void *opaque)
{
    Stc32gUARTState *s = opaque;

    return FIELD_EX8(s->regs[R_SCON], SCON, REN) &&
           !FIELD_EX8(s->regs[R_SCON], SCON, RI);
}

static void stc32g_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Stc32gUARTState *s = opaque;

    if (size && stc32g_uart_can_receive(s)) {
        s->rx_buffer = buf[0];
        s->regs[R_SCON] =
            FIELD_DP8(s->regs[R_SCON], SCON, RI, 1);
        stc32g_uart_update_irq(s);
    }
}

static void stc32g_uart_scon_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc32gUARTState *s = STC32G_UART(reg->opaque);

    stc32g_uart_update_irq(s);
    if (!device_is_in_reset(DEVICE(s))) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static uint64_t stc32g_uart_sbuf_post_read(RegisterInfo *reg,
                                           uint64_t value)
{
    Stc32gUARTState *s = STC32G_UART(reg->opaque);

    return s->rx_buffer;
}

static void stc32g_uart_sbuf_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc32gUARTState *s = STC32G_UART(reg->opaque);
    uint8_t byte = value;

    if (device_is_in_reset(DEVICE(s))) {
        return;
    }

    qemu_chr_fe_write_all(&s->chr, &byte, 1);
    s->regs[R_SCON] = FIELD_DP8(s->regs[R_SCON], SCON, TI, 1);
    stc32g_uart_update_irq(s);
}

static const RegisterAccessInfo stc32g_uart_regs_info[] = {
    {   .name = "SCON", .addr = A_SCON,
        .post_write = stc32g_uart_scon_post_write,
    },{ .name = "SBUF", .addr = A_SBUF,
        .post_read = stc32g_uart_sbuf_post_read,
        .post_write = stc32g_uart_sbuf_post_write,
    },
};

static const MemoryRegionOps stc32g_uart_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc32g_uart_reset(DeviceState *dev)
{
    Stc32gUARTState *s = STC32G_UART(dev);
    unsigned i;

    s->rx_buffer = 0;
    for (i = 0; i < ARRAY_SIZE(stc32g_uart_regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    stc32g_uart_update_irq(s);
}

static int stc32g_uart_post_load(void *opaque, int version_id)
{
    Stc32gUARTState *s = opaque;

    stc32g_uart_update_irq(s);
    if (stc32g_uart_can_receive(s)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
    return 0;
}

static const VMStateDescription stc32g_uart_vmstate = {
    .name = "stc32g.uart1",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc32g_uart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc32gUARTState, STC32G_UART_REGS),
        VMSTATE_UINT8(rx_buffer, Stc32gUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_uart_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gUARTState, cpu, TYPE_MCS51_CPU,
                     MCS251CPU *),
    DEFINE_PROP_CHR("chardev", Stc32gUARTState, chr),
};

static void stc32g_uart_realize(DeviceState *dev, Error **errp)
{
    Stc32gUARTState *s = STC32G_UART(dev);

    if (!s->cpu) {
        error_setg(errp, "stc32g-uart requires a CPU link");
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, stc32g_uart_can_receive,
                             stc32g_uart_receive, NULL, NULL, s, NULL,
                             true);
}

static void stc32g_uart_init(Object *obj)
{
    Stc32gUARTState *s = STC32G_UART(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc32g_uart_regs_info) !=
                      STC32G_UART_REGS);
    s->reg_array = register_init_block8(
        DEVICE(obj), stc32g_uart_regs_info,
        ARRAY_SIZE(stc32g_uart_regs_info), s->regs_info, s->regs,
        &stc32g_uart_ops, false, STC32G_UART_REGS);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
}

static void stc32g_uart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc32g_uart_realize;
    device_class_set_legacy_reset(dc, stc32g_uart_reset);
    device_class_set_props(dc, stc32g_uart_properties);
    dc->vmsd = &stc32g_uart_vmstate;
}

static const TypeInfo stc32g_uart_type = {
    .name = TYPE_STC32G_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc32gUARTState),
    .instance_init = stc32g_uart_init,
    .class_init = stc32g_uart_class_init,
};

static void stc32g_uart_register_types(void)
{
    type_register_static(&stc32g_uart_type);
}

type_init(stc32g_uart_register_types)
