/*
 * STM32G474 flexible data-rate CAN subsystem
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_STM32G474_FDCAN_H
#define HW_NET_STM32G474_FDCAN_H

#include "hw/core/clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/net/m_can.h"
#include "net/can_emu.h"
#include "qom/object.h"

#define TYPE_STM32G474_FDCAN "stm32g474-fdcan"
OBJECT_DECLARE_SIMPLE_TYPE(Stm32g474FdcanState, STM32G474_FDCAN)

#define STM32G474_FDCAN_NUM_CHANNELS 3
#define STM32G474_FDCAN_NUM_IRQS 2
#define STM32G474_FDCAN_MMIO_SIZE 0x400
#define STM32G474_FDCAN_MRAM_SIZE 0x9f0
#define STM32G474_FDCAN_CHANNEL_MRAM_SIZE 0x350
#define STM32G474_FDCAN_NUM_REGS (0x100 / sizeof(uint32_t) + 1)

typedef struct Stm32g474FdcanChannel {
    Stm32g474FdcanState *parent;
    unsigned int index;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_FDCAN_NUM_REGS];
    uint32_t regs[STM32G474_FDCAN_NUM_REGS];

    MCanEngine engine;
    qemu_irq irq[STM32G474_FDCAN_NUM_IRQS];

    uint32_t cccr_old;
    bool cccr_write_pending;
} Stm32g474FdcanChannel;

struct Stm32g474FdcanState {
    SysBusDevice parent_obj;

    Stm32g474FdcanChannel channel[STM32G474_FDCAN_NUM_CHANNELS];
    MemoryRegion message_ram;
    uint8_t *message_ram_ptr;

    Clock *kernel_clk;
    Clock *pclk;
    CanBusState *canbus[STM32G474_FDCAN_NUM_CHANNELS];

    bool resetting;
    bool peripheral_reset_asserted;
    bool engines_initialized;
};

#endif
