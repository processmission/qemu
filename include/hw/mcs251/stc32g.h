/*
 * STC32G144K246 SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MCS251_STC32G_H
#define HW_MCS251_STC32G_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "target/mcs251/cpu.h"
#include "qom/object.h"

#define TYPE_STC32G_TIMER "stc32g-timer"
OBJECT_DECLARE_SIMPLE_TYPE(Stc32gTimerState, STC32G_TIMER)

#define TYPE_STC32G_UART "stc32g-uart"
OBJECT_DECLARE_SIMPLE_TYPE(Stc32gUARTState, STC32G_UART)

#define TYPE_STC32G_GPIO "stc32g-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Stc32gGPIOState, STC32G_GPIO)

#define TYPE_STC32G_SOC "stc32g-soc"
OBJECT_DECLARE_SIMPLE_TYPE(Stc32gSoCState, STC32G_SOC)

#define STC32G_EDATA_BASE 0x000000u
#define STC32G_EDATA_SIZE (16 * KiB)
#define STC32G_XDATA_BASE 0x010000u
#define STC32G_XDATA_SIZE (128 * KiB)
#define STC32G_EXEC_DATA_BASE 0x030000u
#define STC32G_EXEC_CODE_BASE 0x800000u
#define STC32G_EXEC_RAM_SIZE (4 * KiB)
#define STC32G_FLASH_BASE 0xfc2800u
#define STC32G_FLASH_SIZE (246 * KiB)
#define STC32G_GPIO_PORTS 8
#define STC32G_GPIO_PINS (STC32G_GPIO_PORTS * 8)
#define STC32G_GPIO_REGS (STC32G_GPIO_PORTS * 3)

typedef struct Stc32gTimerChannel {
    Stc32gTimerState *parent;
    unsigned index;
} Stc32gTimerChannel;

struct Stc32gTimerState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    MemoryRegion sfr;
    MemoryRegion xfr;
    QEMUTimer *timer[2];
    Stc32gTimerChannel channel[2];
    qemu_irq irq[4];
    uint32_t clock_frequency;
    uint8_t tmod;
    uint8_t tl[2];
    uint8_t th[2];
    uint8_t reload_tl[2];
    uint8_t reload_th[2];
    uint8_t prescaler[2];
    uint8_t counter_prescale_count[2];
    int64_t last_ns[2];
    bool gate[2];
    bool counter_input[2];
};

struct Stc32gUARTState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    MemoryRegion sfr;
    CharFrontend chr;
    qemu_irq irq;
    uint8_t scon;
    uint8_t rx_buffer;
    uint8_t tx_buffer;
};

typedef struct Stc32gGPIOReg {
    MemoryRegion mr;
    Stc32gGPIOState *parent;
    uint8_t address;
} Stc32gGPIOReg;

struct Stc32gGPIOState {
    SysBusDevice parent_obj;

    MCS251CPU *cpu;
    Stc32gGPIOReg reg[STC32G_GPIO_REGS];
    qemu_irq output[STC32G_GPIO_PINS];
    qemu_irq int_line[2];
    uint8_t latch[STC32G_GPIO_PORTS];
    uint8_t input[STC32G_GPIO_PORTS];
    uint8_t mode1[STC32G_GPIO_PORTS];
    uint8_t mode0[STC32G_GPIO_PORTS];
};

struct Stc32gSoCState {
    SysBusDevice parent_obj;

    MCS251CPU cpu;
    MemoryRegion edata;
    MemoryRegion xdata;
    MemoryRegion exec_ram;
    MemoryRegion exec_data_alias;
    MemoryRegion exec_code_alias;
    MemoryRegion flash;
    Stc32gTimerState timer;
    Stc32gUARTState uart;
    Stc32gGPIOState gpio;
};

#endif
