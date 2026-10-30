/*
 * Sony PlayStation 2 Emotion Engine serial port (SIO)
 *
 * The EE SIO has a 16-byte receive FIFO and a transmit FIFO whose levels
 * are reported in SIO_ISR: bits 11:8 count received bytes and bits 15:12
 * give the transmit level, where 0x8 means full.  Transmission is
 * instantaneous here, so the transmit FIFO always reads as empty.  The
 * Linux driver polls, so no interrupt line is modelled.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/ps2_sio.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static int ps2_sio_can_receive(void *opaque)
{
    PS2SIOState *s = opaque;

    return PS2_SIO_FIFO_SIZE - s->rx_count;
}

static void ps2_sio_receive(void *opaque, const uint8_t *buf, int size)
{
    PS2SIOState *s = opaque;

    for (int i = 0; i < size && s->rx_count < PS2_SIO_FIFO_SIZE; i++) {
        s->rx_fifo[(s->rx_head + s->rx_count) % PS2_SIO_FIFO_SIZE] = buf[i];
        s->rx_count++;
    }
}

static uint8_t ps2_sio_pop(PS2SIOState *s)
{
    uint8_t c;

    if (!s->rx_count) {
        return 0;
    }
    c = s->rx_fifo[s->rx_head];
    s->rx_head = (s->rx_head + 1) % PS2_SIO_FIFO_SIZE;
    s->rx_count--;
    qemu_chr_fe_accept_input(&s->chr);
    return c;
}

static uint64_t ps2_sio_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2SIOState *s = opaque;

    switch (addr) {
    case PS2_SIO_LCR:
        return s->lcr;
    case PS2_SIO_LSR:
        return 0;
    case PS2_SIO_IER:
        return s->ier;
    case PS2_SIO_ISR:
        return (MIN(s->rx_count, 0xf) << 8);
    case PS2_SIO_FCR:
        return s->fcr;
    case PS2_SIO_BGR:
        return s->bgr;
    case PS2_SIO_RXFIFO:
        return ps2_sio_pop(s);
    case PS2_SIO_TXFIFO:
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void ps2_sio_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned int size)
{
    PS2SIOState *s = opaque;
    uint8_t ch;

    switch (addr) {
    case PS2_SIO_LCR:
        s->lcr = value;
        break;
    case PS2_SIO_LSR:
        break;
    case PS2_SIO_IER:
        s->ier = value;
        break;
    case PS2_SIO_ISR:
        /* Writing acknowledges the status bits, which are not latched. */
        break;
    case PS2_SIO_FCR:
        s->fcr = value;
        if (value & PS2_SIO_FCR_RFRST) {
            s->rx_head = 0;
            s->rx_count = 0;
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    case PS2_SIO_BGR:
        s->bgr = value;
        break;
    case PS2_SIO_TXFIFO:
        ch = value;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    case PS2_SIO_RXFIFO:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps ps2_sio_ops = {
    .read = ps2_sio_read,
    .write = ps2_sio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ps2_sio_reset_hold(Object *obj, ResetType type)
{
    PS2SIOState *s = PS2_SIO(obj);

    s->lcr = 0;
    s->ier = 0;
    s->fcr = 0;
    s->bgr = 0;
    s->rx_head = 0;
    s->rx_count = 0;
}

static void ps2_sio_realize(DeviceState *dev, Error **errp)
{
    PS2SIOState *s = PS2_SIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &ps2_sio_ops, s,
                          TYPE_PS2_SIO, PS2_SIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    qemu_chr_fe_set_handlers(&s->chr, ps2_sio_can_receive, ps2_sio_receive,
                             NULL, NULL, s, NULL, true);
}

static const Property ps2_sio_properties[] = {
    DEFINE_PROP_CHR("chardev", PS2SIOState, chr),
};

static bool ps2_sio_post_load_valid(void *opaque, int version_id)
{
    PS2SIOState *s = opaque;

    return s->rx_head < PS2_SIO_FIFO_SIZE && s->rx_count <= PS2_SIO_FIFO_SIZE;
}

static const VMStateDescription vmstate_ps2_sio = {
    .name = TYPE_PS2_SIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(lcr, PS2SIOState),
        VMSTATE_UINT32(ier, PS2SIOState),
        VMSTATE_UINT32(fcr, PS2SIOState),
        VMSTATE_UINT32(bgr, PS2SIOState),
        VMSTATE_UINT8_ARRAY(rx_fifo, PS2SIOState, PS2_SIO_FIFO_SIZE),
        VMSTATE_UINT32(rx_head, PS2SIOState),
        VMSTATE_UINT32(rx_count, PS2SIOState),
        VMSTATE_VALIDATE("rx fifo in range", ps2_sio_post_load_valid),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_sio_reset_hold;
    device_class_set_props(dc, ps2_sio_properties);
    dc->realize = ps2_sio_realize;
    dc->vmsd = &vmstate_ps2_sio;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo ps2_sio_types[] = {
    {
        .name = TYPE_PS2_SIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2SIOState),
        .class_init = ps2_sio_class_init,
    },
};

DEFINE_TYPES(ps2_sio_types);
