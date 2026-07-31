/*
 * STC32G UART1
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/mcs251/stc32g.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"

#define SCON_REN 0x10
#define SCON_TI 0x02
#define SCON_RI 0x01

static void stc32g_uart_update_irq(Stc32gUARTState *s)
{
    qemu_set_irq(s->irq, s->scon & (SCON_TI | SCON_RI));
}

static int stc32g_uart_can_receive(void *opaque)
{
    Stc32gUARTState *s = opaque;

    return (s->scon & SCON_REN) && !(s->scon & SCON_RI);
}

static void stc32g_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Stc32gUARTState *s = opaque;

    if (size && stc32g_uart_can_receive(s)) {
        s->rx_buffer = buf[0];
        s->scon |= SCON_RI;
        stc32g_uart_update_irq(s);
    }
}

static uint64_t stc32g_uart_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    Stc32gUARTState *s = opaque;

    switch (offset) {
    case 0:
        return s->scon;
    case 1:
        return s->rx_buffer;
    default:
        return 0;
    }
}

static void stc32g_uart_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    Stc32gUARTState *s = opaque;
    uint8_t byte = value;

    switch (offset) {
    case 0: {
        bool receive_reenabled = !(s->scon & SCON_REN) &&
                                 (byte & SCON_REN);
        bool receive_consumed = (s->scon & SCON_RI) &&
                                !(byte & SCON_RI);

        s->scon = byte;
        stc32g_uart_update_irq(s);
        if (receive_reenabled || receive_consumed) {
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    }
    case 1:
        s->tx_buffer = byte;
        qemu_chr_fe_write_all(&s->chr, &s->tx_buffer, 1);
        s->scon |= SCON_TI;
        stc32g_uart_update_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps stc32g_uart_ops = {
    .read = stc32g_uart_read,
    .write = stc32g_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc32g_uart_reset(DeviceState *dev)
{
    Stc32gUARTState *s = STC32G_UART(dev);

    s->scon = 0;
    s->rx_buffer = 0;
    s->tx_buffer = 0;
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
        VMSTATE_UINT8(scon, Stc32gUARTState),
        VMSTATE_UINT8(rx_buffer, Stc32gUARTState),
        VMSTATE_UINT8(tx_buffer, Stc32gUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc32g_uart_properties[] = {
    DEFINE_PROP_LINK("cpu", Stc32gUARTState, cpu, TYPE_MCS251_CPU,
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

    memory_region_init_io(&s->sfr, obj, &stc32g_uart_ops, s,
                          "stc32g.uart1-sfr", 2);
    sysbus_init_mmio(sbd, &s->sfr);
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
