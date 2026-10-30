/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ST25R100 SPI register interface, oscillator and ISO14443 no-response
 * timer.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/fifo8.h"
#include "hw/ssi/ssi.h"
#include "hw/core/irq.h"

#define TYPE_ST25R100 "microduck-st25r100"
OBJECT_DECLARE_SIMPLE_TYPE(ST25R100State, ST25R100)
struct ST25R100State {
    SSIPeripheral parent_obj;
    uint8_t regs[128];
    uint8_t address;
    bool command_byte, reading, reset_asserted;
    Fifo8 fifo;
    QEMUTimer *timer;
    unsigned operation;
    int64_t oscillator_ready;
    qemu_irq irq;
};

static void nfc_irq(ST25R100State *s)
{
    qemu_set_irq(s->irq, (s->regs[0x3c] | s->regs[0x3d] | s->regs[0x3e]) != 0);
}
static void nfc_complete(void *opaque)
{
    ST25R100State *s = opaque;
    if (s->operation == 0x68) {
        s->regs[0x3e] |= 0x10;
    } else if (s->operation == 0x6a) {
        /*
         * No RF tag is attached: end reception when the programmed NRT
         * expires.
         */
        s->regs[0x3d] |= 0x40;
        fifo8_reset(&s->fifo);
    }
    nfc_irq(s);
}
static void nfc_reset(DeviceState *device)
{
    ST25R100State *s = ST25R100(device);
    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x3f] = 0xa9;
    s->regs[0x09] = 0x91;
    s->regs[0x0d] = 3;
    s->command_byte = true;
    s->oscillator_ready = INT64_MAX;
    fifo8_reset(&s->fifo);
    nfc_irq(s);
}
static void nfc_gpio_reset(void *opaque, int line, int level)
{
    ST25R100State *s = opaque;
    s->reset_asserted = level;
    if (level) {
        nfc_reset(DEVICE(s));
    }
}
static void nfc_command(ST25R100State *s, unsigned command)
{
    if (command == 0x60) {
        nfc_reset(DEVICE(s));
        s->command_byte = false;
    } else if (command == 0x62) {
        timer_del(s->timer);
        fifo8_reset(&s->fifo);
    } else if (command == 0x68 && (s->regs[0] & 0x32) == 0x32) {
        s->operation = command;
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200000);
    } else if (command == 0x6a && (s->regs[0] & 0x32) == 0x32) {
        unsigned ticks = (s->regs[0x1f] << 8) | s->regs[0x20];
        s->operation = command;
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MAX(ticks, 1) * UINT64_C(64000000000) / 13560000);
    }
}
static uint32_t nfc_transfer(SSIPeripheral *device, uint32_t value)
{
    ST25R100State *s = ST25R100(device);
    if (s->reset_asserted) {
        return 0xff;
    }
    if (s->command_byte) {
        s->command_byte = false;
        s->reading = (value & 0x80) != 0;
        s->address = value & 0x7f;
        if (!s->reading && s->address >= 0x60) {
            nfc_command(s, s->address);
        }
        return 0;
    }
    if (s->address == 0x5f) {
        if (s->reading) {
            return fifo8_is_empty(&s->fifo) ? 0 : fifo8_pop(&s->fifo);
        }
        if (!fifo8_is_full(&s->fifo)) {
            fifo8_push(&s->fifo, value);
        }
        return 0;
    }
    unsigned reg = s->address++ & 0x7f;
    if (s->reading) {
        if (reg == 0xf) {
            return s->regs[reg] |
                   (((s->regs[0] & 2) &&
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >=
                     s->oscillator_ready) ? 0x20 : 0);
        }
        if (reg == 0x36) {
            return fifo8_num_used(&s->fifo);
        }
        if (reg == 0x37) {
            return (fifo8_num_used(&s->fifo) >> 8) << 6;
        }
        uint8_t result = s->regs[reg];
        if (reg >= 0x3c && reg <= 0x3e) {
            s->regs[reg] = 0;
            nfc_irq(s);
        }
        return result;
    }
    if (reg >= 0x36 && reg <= 0x3f) {
        return 0;
    }
    if (reg == 0 && (value & 2) && !(s->regs[0] & 2)) {
        s->oscillator_ready = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000;
    }
    s->regs[reg] = value;
    return 0;
}
static int nfc_cs(SSIPeripheral *device, bool level)
{
    ST25R100State *s = ST25R100(device);
    if (!level) {
        s->command_byte = true;
    }
    return 0;
}
static void nfc_realize(SSIPeripheral *device, Error **errp)
{
    ST25R100State *s = ST25R100(device);
    fifo8_create(&s->fifo, 512);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nfc_complete, s);
    qdev_init_gpio_out_named(DEVICE(s), &s->irq, "irq", 1);
    qdev_init_gpio_in_named(DEVICE(s), nfc_gpio_reset, "reset", 1);
}
static void nfc_finalize(Object *obj)
{
    ST25R100State *s = ST25R100(obj);
    timer_free(s->timer);
    fifo8_destroy(&s->fifo);
}
static void nfc_class_init(ObjectClass *oc, const void *data)
{
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);
    sc->realize = nfc_realize;
    sc->transfer = nfc_transfer;
    sc->set_cs = nfc_cs;
    sc->cs_polarity = SSI_CS_LOW;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), nfc_reset);
}
static const TypeInfo nfc_info = {
    .name = TYPE_ST25R100, .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(ST25R100State), .class_init = nfc_class_init,
    .instance_finalize = nfc_finalize,
};
static void nfc_register(void) { type_register_static(&nfc_info); }
type_init(nfc_register)
