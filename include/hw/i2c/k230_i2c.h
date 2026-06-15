/*
 * K230 DesignWare I2C controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_K230_I2C_H
#define HW_I2C_K230_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_K230_I2C "riscv.k230.i2c"
OBJECT_DECLARE_SIMPLE_TYPE(K230I2CState, K230_I2C)

#define K230_I2C_SIZE 0x1000
#define K230_I2C_REG_COUNT (K230_I2C_SIZE / 4)
#define K230_I2C_FIFO_DEPTH 16

struct K230I2CState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    I2CBus *bus;

    uint32_t regs[K230_I2C_REG_COUNT];
    uint8_t rx_fifo[K230_I2C_FIFO_DEPTH];
    uint8_t rx_pos;
    uint8_t rx_len;
    uint8_t address;
    bool started;
    bool recv;
};

#endif /* HW_I2C_K230_I2C_H */
