/*
 * SiFli SF32LB52X SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SF32LB52X_H
#define HW_ARM_SF32LB52X_H

#include "hw/arm/armv7m.h"
#include "hw/char/sf32lb_usart.h"
#include "hw/display/sf32lb52x_lcdc.h"
#include "hw/dma/sf32lb52x_dmac.h"
#include "hw/i2c/sf32lb52x_i2c.h"
#include "hw/misc/sf32lb52x_mpi.h"
#include "hw/ssi/sf32lb52x_spi.h"
#include "hw/misc/sf32lb52x_crc.h"
#include "hw/misc/sf32lb52x_efuse.h"
#include "hw/misc/sf32lb52x_gpadc.h"
#include "hw/misc/sf32lb52x_rtc.h"
#include "hw/misc/sf32lb52x_trng.h"
#include "hw/misc/sf32lb52x_tsen.h"
#include "hw/misc/sf32lb52x_wdt.h"
#include "hw/core/clock.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_SF32LB52X_SOC "sifli.sf32lb52x.soc"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XSoCState, SF32LB52X_SOC)

#define SF32LB52X_NUM_IRQS      (100)
#define SF32LB52X_NUM_USARTS    (3)
#define SF32LB52X_NUM_I2C       (4)
#define SF32LB52X_NUM_SPI       (2)

/* The HCPU runs from the DLL1 output at 240 MHz. */
#define SF32LB52X_SYSCLK_HZ     (240 * 1000 * 1000)

/* External QSPI NOR on MPI2, accessed through the MPI2 XIP window. */
#define SF32LB52X_FLASH_BASE    0x12000000
#define SF32LB52X_FLASH_SIZE    (16 * 1024 * 1024)
#define SF32LB52X_CODE_BASE     0x12010000

/* HPSYS SRAM and the shared SRAM window used for inter-core data. */
#define SF32LB52X_SRAM_BASE     0x20000000
#define SF32LB52X_SRAM_SIZE     (512 * 1024)
#define SF32LB52X_SRAM1_BASE    0x20400000
#define SF32LB52X_SRAM1_SIZE    (64 * 1024)

/* System controller blocks on the HPSYS AHB. */
#define SF32LB52X_RCC_BASE      0x50000000
#define SF32LB52X_PINMUX_BASE   0x50003000
#define SF32LB52X_CFG_BASE      0x5000b000
#define SF32LB52X_GPIO_BASE     0x500a0000
#define SF32LB52X_CRC_BASE      0x50048000
#define SF32LB52X_EFUSE_BASE    0x5000c000
#define SF32LB52X_GPADC_BASE    0x50087000
#define SF32LB52X_RTC_BASE      0x500cb000
#define SF32LB52X_TRNG_BASE     0x5000f000
#define SF32LB52X_TSEN_BASE     0x50089000
#define SF32LB52X_LCDC_BASE     0x50008000
#define SF32LB52X_DMAC_BASE     0x50081000
#define SF32LB52X_I2C1_BASE     0x5009c000
#define SF32LB52X_SPI1_BASE     0x50095000
#define SF32LB52X_MPI2_BASE     0x50042000
#define SF32LB52X_WDT_BASE      0x50094000
#define SF32LB52X_AON_BASE      0x500c0000
#define SF32LB52X_PMUC_BASE     0x500ca000

#define SF32LB52X_USART1_BASE   0x50084000
#define SF32LB52X_USART_STRIDE  0x1000

#define SF32LB52X_SYSCON_SIZE   0x1000
#define SF32LB52X_SYSCON_WORDS  (SF32LB52X_SYSCON_SIZE / 4)

/* Register banks that only store what the guest writes. */
typedef struct SF32LB52XRegBank {
    MemoryRegion mmio;
    uint32_t regs[SF32LB52X_SYSCON_WORDS];
} SF32LB52XRegBank;

struct SF32LB52XSoCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    ARMv7MState armv7m;
    SF32LBUSARTState usart[SF32LB52X_NUM_USARTS];
    SF32LB52XRtcState rtc;
    SF32LB52XWdtState wdt;
    SF32LB52XCrcState crc;
    SF32LB52XTrngState trng;
    SF32LB52XTsenState tsen;
    SF32LB52XGpadcState gpadc;
    SF32LB52XEfuseState efuse;
    SF32LB52XI2CState i2c[SF32LB52X_NUM_I2C];
    SF32LB52XSPIState spi[SF32LB52X_NUM_SPI];
    SF32LB52XDMACState dmac;
    SF32LB52XMPIState mpi;
    SF32LB52XLCDCState lcdc;

    SF32LB52XRegBank rcc;
    SF32LB52XRegBank aon;
    SF32LB52XRegBank pmuc;
    SF32LB52XRegBank cfg;
    SF32LB52XRegBank pinmux;
    SF32LB52XRegBank gpio;

    MemoryRegion flash;
    MemoryRegion sram;
    MemoryRegion sram1;

    MemoryRegion *memory; /* system memory, linked by the board */
    Clock *sysclk;        /* input clock from the board, forwarded to the CPU */
};

#endif /* HW_ARM_SF32LB52X_H */
