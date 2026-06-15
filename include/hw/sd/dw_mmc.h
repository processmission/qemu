/*
 * DesignWare Mobile Storage Host Controller (dw_mmc) - Synopsys IP.
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SysBusDevice model for the standard dw_mmc register bank, an IDMAC
 * descriptor-DMA engine, a PIO FIFO data port, a card-detect slot, and a
 * single TYPE_SD_CARD attached via an SDBus. RK3588 currently wires this
 * model as its SD-card controller at 0xfe2c0000.
 *
 * The model mirrors the register-extraction contract:
 *   - hand-written MMIO for the core regs, IDMAC block, and FIFO data ports
 *     because command, FIFO, W1C, and DMA side effects dominate this register
 *     bank.
 *   - FIFO data ports at +0x100 (>=2.40a) and +0x200 (legacy U-Boot
 *     register layout) drive an internal byte ring.
 *   - RK vendor regs (0x130/0x134/0x138 TIMING_CON/MISC_CON) RAZ/WI
 *     on RK3588 (internal_phase=false).
 *   - The CMD register START bit self-clears on command completion;
 *     the response is read out of the attached SD card via
 *     sdbus_do_command and copied into RESP0-3 (R2 long-resp maps
 *     RESP0<-resp[3], RESP1<-resp[2], etc., mirroring dw_mmc.c:1782-1787).
 *   - The whole 0x4000 window is covered so AArch64 guest writes to
 *     any address in [0xfe2c0000, 0xfe2c4000) do not abort (D-15).
 */

#ifndef HW_SD_DW_MMC_H
#define HW_SD_DW_MMC_H

#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "qom/object.h"

#define TYPE_DW_MMC "dw-mmc"
OBJECT_DECLARE_SIMPLE_TYPE(DwMmcState, DW_MMC)

#define TYPE_DW_MMC_BUS "dw-mmc-bus"

/* MMIO window size. */
#define DW_MMC_MMIO_SIZE        0x4000

/* FIFO depth in 32-bit words (DTS fifo-depth = 0x100). */
#define DW_MMC_FIFO_DEPTH       0x100

/* IDMAC descriptor walker ring bound (sanity limit). */
#define DW_MMC_IDMAC_MAX_DESCS  4096

struct DwMmcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion vendor;   /* RAZ/WI RK vendor/tail regs after 0x200 */

    SDBus sdbus;
    qemu_irq irq;

    /* Outbound card-present/readonly GPIOs (not wired by board). */
    qemu_irq card_inserted;
    qemu_irq card_readonly;

    /* dw_mmc register file (only the fields the model uses). */
    uint32_t ctrl;          /* 0x000 */
    uint32_t pwren;         /* 0x004 */
    uint32_t clkdiv;        /* 0x008 */
    uint32_t clksrc;        /* 0x00c */
    uint32_t clkena;        /* 0x010 */
    uint32_t tmout;         /* 0x014 */
    uint32_t ctype;         /* 0x018 */
    uint32_t blksiz;        /* 0x01c */
    uint32_t bytcnt;        /* 0x020 */
    uint32_t intmask;       /* 0x024 */
    uint32_t cmdarg;        /* 0x028 */
    uint32_t cmd;           /* 0x02c */
    uint32_t resp[4];       /* 0x030-0x03c */
    uint32_t rintsts;       /* 0x044 (W1C) */
    uint32_t fifoth;        /* 0x04c */
    uint32_t debnce;        /* 0x064 */
    uint32_t usrid;         /* 0x068 */
    uint32_t verid;         /* 0x06c (RO) */
    uint32_t hcon;          /* 0x070 (RO) */
    uint32_t uhs_reg;       /* 0x074 */
    uint32_t rst_n;         /* 0x078 */
    uint32_t bmod;          /* 0x080 (IDMAC bus mode) */
    uint32_t dbaddr;        /* 0x088 (IDMAC desc base) */
    uint32_t idsts;         /* 0x08c (W1C) */
    uint32_t idinten;       /* 0x090 */
    uint32_t cdthrctl;      /* 0x100 */
    uint32_t enable_shift;  /* 0x110 */

    /* FIFO byte ring (used for PIO and IDMAC staging). */
    uint8_t fifo[DW_MMC_FIFO_DEPTH * 4];
    uint32_t fifo_len;      /* bytes currently in FIFO */
    uint32_t fifo_pos;      /* read cursor */

    /* Active transfer tracking. */
    uint32_t transfer_bytes_remaining; /* set on CMD with DAT_EXP */
    bool transfer_is_write;            /* host->card */
    bool transfer_send_stop;
    bool transfer_active;
};

#endif /* HW_SD_DW_MMC_H */
