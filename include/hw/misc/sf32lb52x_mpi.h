/*
 * SiFli SF32LB52X MPI2 QSPI-NOR controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_MPI_H
#define HW_MISC_SF32LB52X_MPI_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_MPI "sifli.sf32lb52x-mpi"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XMPIState, SF32LB52X_MPI)

#define SF32LB52X_MPI_SIZE       0x1000
#define SF32LB52X_MPI_NUM_REGS   (SF32LB52X_MPI_SIZE / 4)
#define SF32LB52X_MPI_FIFO_SIZE  64
#define SF32LB52X_MPI_NOR_SIZE   (16 * 1024 * 1024)

/* Register offsets (HAL mpi.h MPI_TypeDef). */
#define SF32LB52X_MPI_CR      0x00
#define SF32LB52X_MPI_DR      0x04
#define SF32LB52X_MPI_DCR     0x08
#define SF32LB52X_MPI_PSCLR   0x0c
#define SF32LB52X_MPI_SR      0x10
#define SF32LB52X_MPI_SCR     0x14
#define SF32LB52X_MPI_CMDR1   0x18
#define SF32LB52X_MPI_AR1     0x1c
#define SF32LB52X_MPI_ABR1    0x20
#define SF32LB52X_MPI_DLR1    0x24
#define SF32LB52X_MPI_CCR1    0x28
#define SF32LB52X_MPI_CMDR2   0x2c
#define SF32LB52X_MPI_DLR2    0x38
#define SF32LB52X_MPI_CCR2    0x3c
#define SF32LB52X_MPI_HCMDR   0x40
#define SF32LB52X_MPI_HRABR   0x44
#define SF32LB52X_MPI_HRCCR   0x48
#define SF32LB52X_MPI_FIFOCR  0x54
#define SF32LB52X_MPI_MISCR   0x58
#define SF32LB52X_MPI_CIR     0x78
#define SF32LB52X_MPI_SMR     0x7c
#define SF32LB52X_MPI_SMKR    0x80
#define SF32LB52X_MPI_TIMR    0x84

/* CR */
#define SF32LB52X_MPI_CR_EN      (1U << 0)
#define SF32LB52X_MPI_CR_CMD2E   (1U << 16)
#define SF32LB52X_MPI_CR_SME2    (1U << 18)

/* SR */
#define SF32LB52X_MPI_SR_TCF     (1U << 0)
#define SF32LB52X_MPI_SR_FUF     (1U << 1)
#define SF32LB52X_MPI_SR_FOF     (1U << 2)
#define SF32LB52X_MPI_SR_SMF     (1U << 3)

/* SCR */
#define SF32LB52X_MPI_SCR_TCFC   (1U << 0)
#define SF32LB52X_MPI_SCR_FUFC   (1U << 1)
#define SF32LB52X_MPI_SCR_FOFC   (1U << 2)
#define SF32LB52X_MPI_SCR_SMFC   (1U << 3)

/* CCR1 */
#define SF32LB52X_MPI_CCR_DMODE_SHIFT 18
#define SF32LB52X_MPI_CCR_FMODE       (1U << 21)

/* FIFOCR */
#define SF32LB52X_MPI_FIFOCR_RXCLR (1U << 0)
#define SF32LB52X_MPI_FIFOCR_TXCLR (1U << 8)

/* SPI NOR opcodes used by the Zephyr driver. */
#define SF32LB52X_NOR_CMD_WREN   0x06
#define SF32LB52X_NOR_CMD_WRSR   0x01
#define SF32LB52X_NOR_CMD_RDSR   0x05
#define SF32LB52X_NOR_CMD_RDSR2  0x35
#define SF32LB52X_NOR_CMD_RDID   0x9f
#define SF32LB52X_NOR_CMD_READ   0x03
#define SF32LB52X_NOR_CMD_PP     0x02
#define SF32LB52X_NOR_CMD_SE     0x20
#define SF32LB52X_NOR_CMD_BE32   0x52
#define SF32LB52X_NOR_CMD_BE64   0xd8
#define SF32LB52X_NOR_CMD_CE     0x60
#define SF32LB52X_NOR_CMD_CE_ALT 0xc7

#define SF32LB52X_NOR_JEDEC_ID   0x00182085U

struct SF32LB52XMPIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    /* NOR XIP window backing store, linked by the SoC. */
    MemoryRegion *nor;
    uint64_t nor_size;
    /* Guest physical address of the XIP window. */
    hwaddr xip_base;

    uint32_t regs[SF32LB52X_MPI_NUM_REGS];
    uint8_t fifo[SF32LB52X_MPI_FIFO_SIZE];
    unsigned fifo_len;
    bool wel;
    bool qe;
};

#endif /* HW_MISC_SF32LB52X_MPI_H */
