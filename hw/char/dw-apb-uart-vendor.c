/*
 * Synopsys DesignWare APB UART vendor register window
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The 16550-compatible register block is modeled by serial-mm. This companion
 * window covers the DesignWare APB UART extension registers that sit above the
 * core 16550 area. Firmware commonly polls USR while transmitting early debug
 * output, so report the transmit FIFO as available and not busy.
 */

#include "qemu/osdep.h"
#include "hw/char/dw-apb-uart-vendor.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/module.h"

#define DW_APB_UART_USR        (0x7c - DW_APB_UART_VENDOR_BASE)
#define DW_APB_UART_USR_BUSY   BIT(0)
#define DW_APB_UART_USR_TFNF   BIT(1)
#define DW_APB_UART_USR_TFE    BIT(2)

static uint64_t dw_apb_uart_vendor_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    DWAPBUARTVendorState *s = opaque;

    if (offset + size > DW_APB_UART_VENDOR_SIZE) {
        return 0;
    }

    if (offset == DW_APB_UART_USR && size == 4) {
        uint32_t value = ldl_le_p(&s->regs[offset]);

        value &= ~DW_APB_UART_USR_BUSY;
        value |= DW_APB_UART_USR_TFNF | DW_APB_UART_USR_TFE;
        return value;
    }

    switch (size) {
    case 1:
        return s->regs[offset];
    case 2:
        return lduw_le_p(&s->regs[offset]);
    case 4:
        return ldl_le_p(&s->regs[offset]);
    default:
        return 0;
    }
}

static void dw_apb_uart_vendor_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    DWAPBUARTVendorState *s = opaque;

    if (offset + size > DW_APB_UART_VENDOR_SIZE) {
        return;
    }

    switch (size) {
    case 1:
        s->regs[offset] = value;
        break;
    case 2:
        stw_le_p(&s->regs[offset], value);
        break;
    case 4:
        stl_le_p(&s->regs[offset], value);
        break;
    }
}

static const MemoryRegionOps dw_apb_uart_vendor_ops = {
    .read = dw_apb_uart_vendor_read,
    .write = dw_apb_uart_vendor_write,
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

static void dw_apb_uart_vendor_reset(DeviceState *dev)
{
    DWAPBUARTVendorState *s = DW_APB_UART_VENDOR(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void dw_apb_uart_vendor_realize(DeviceState *dev, Error **errp)
{
    DWAPBUARTVendorState *s = DW_APB_UART_VENDOR(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &dw_apb_uart_vendor_ops, s,
                          "dw-apb-uart-vendor", DW_APB_UART_VENDOR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_dw_apb_uart_vendor = {
    .name = TYPE_DW_APB_UART_VENDOR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, DWAPBUARTVendorState,
                            DW_APB_UART_VENDOR_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void dw_apb_uart_vendor_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dw_apb_uart_vendor_realize;
    device_class_set_legacy_reset(dc, dw_apb_uart_vendor_reset);
    dc->vmsd = &vmstate_dw_apb_uart_vendor;
    dc->user_creatable = false;
}

static const TypeInfo dw_apb_uart_vendor_info = {
    .name = TYPE_DW_APB_UART_VENDOR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWAPBUARTVendorState),
    .class_init = dw_apb_uart_vendor_class_init,
};

static void dw_apb_uart_vendor_register_types(void)
{
    type_register_static(&dw_apb_uart_vendor_info);
}

type_init(dw_apb_uart_vendor_register_types)
