/*
 * SiFli SF32LB52X I2C controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout follows the SF32LB52x SVD I2C1 block and the vendor HAL
 * cmsis/sf32lb52x/i2c.h definitions; behavior follows the Zephyr driver
 * drivers/i2c/i2c_sf32lb.c.  Manual MemoryRegionOps are used because the
 * register bank is dominated by the master transfer state machine and by
 * side-effect status/FIFO bits.
 */

#ifndef HW_I2C_SF32LB52X_I2C_H
#define HW_I2C_SF32LB52X_I2C_H

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_I2C "sifli.sf32lb52x-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XI2CState, SF32LB52X_I2C)

#define SF32LB52X_I2C_SIZE 0x1000
#define SF32LB52X_I2C_FIFO_DEPTH 16

struct SF32LB52XI2CState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    I2CBus *bus;

    /*
     * Register file (SVD I2C1: CR, TCR, IER, SR, DBR, SAR, LCR, WCR,
     * RCCR, BMR, DNR, RSVD1, FIFO at 0x00..0x30).
     */
    uint32_t cr;
    uint32_t tcr;
    uint32_t ier;
    uint32_t sr_flags;      /* latched W1C/status bits */
    uint32_t sar;
    uint32_t lcr;
    uint32_t wcr;
    uint32_t rccr;
    uint32_t bmr;
    uint32_t dnr;
    uint8_t dbr_tx;
    uint8_t dbr_rx;

    /* Master transfer state (between START and STOP). */
    bool transfer_active;
    bool is_recv;

    /* 16-byte data FIFO behind the FIFO register. */
    uint8_t fifo[SF32LB52X_I2C_FIFO_DEPTH];
    uint32_t fifo_head;
    uint32_t fifo_count;
};

#endif /* HW_I2C_SF32LB52X_I2C_H */
