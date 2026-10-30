/*
 * Sony PlayStation 2 Emotion Engine serial port (SIO)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_PS2_SIO_H
#define HW_CHAR_PS2_SIO_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PS2_SIO "ps2-sio"
OBJECT_DECLARE_SIMPLE_TYPE(PS2SIOState, PS2_SIO)

#define PS2_SIO_LCR         0x00
#define PS2_SIO_LSR         0x10
#define PS2_SIO_IER         0x20
#define PS2_SIO_ISR         0x30
#define PS2_SIO_FCR         0x40
#define PS2_SIO_BGR         0x50
#define PS2_SIO_TXFIFO      0x80
#define PS2_SIO_RXFIFO      0xc0
#define PS2_SIO_SIZE        0x100

#define PS2_SIO_FCR_RFRST   0x02

#define PS2_SIO_FIFO_SIZE   16

struct PS2SIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;

    uint32_t lcr;
    uint32_t ier;
    uint32_t fcr;
    uint32_t bgr;
    uint8_t rx_fifo[PS2_SIO_FIFO_SIZE];
    uint32_t rx_head;
    uint32_t rx_count;
};

#endif
