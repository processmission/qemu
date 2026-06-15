/*
 * Synopsys DesignWare APB UART vendor register window
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_DW_APB_UART_VENDOR_H
#define HW_CHAR_DW_APB_UART_VENDOR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_DW_APB_UART_VENDOR "dw-apb-uart-vendor"
OBJECT_DECLARE_SIMPLE_TYPE(DWAPBUARTVendorState, DW_APB_UART_VENDOR)

#define DW_APB_UART_VENDOR_BASE 0x20
#define DW_APB_UART_VENDOR_SIZE 0xe0

struct DWAPBUARTVendorState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t regs[DW_APB_UART_VENDOR_SIZE];
};

#endif /* HW_CHAR_DW_APB_UART_VENDOR_H */
