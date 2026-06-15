/*
 * DesignWare Mobile Storage Host Controller (dw_mmc).
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SysBusDevice model for Synopsys dw_mmc. RK3588 wires one instance as
 * sdmmc@fe2c0000. See include/hw/sd/dw_mmc.h for the design contract. The
 * 16 KiB register window is served with hand-written MMIO ops because nearly
 * every register has side effects (CMD kicks the SD card, RINTSTS is W1C,
 * FIFO data port pushes/pops, PLDMND kicks IDMAC, etc.); the registerinfo
 * framework's strengths do not help here.
 *
 * Behavior summary:
 *   - CMD reg write with START set: assemble an SDRequest from
 *     CMDARG + cmd flags, run sdbus_do_command, copy the response
 *     into RESP0-3 (long resp maps R2 the way dw_mmc.c:1782-1787
 *     expects), set CMD_DONE / error bits, raise IRQ.
 *   - CMD with DAT_EXP: set up a transfer; if CTRL[USE_IDMAC]+BMOD
 *     are armed the data phase is served by walking the IDMAC
 *     descriptor ring; otherwise the FIFO port is used (PIO) and
 *     RXDR/TXDR fire as the FIFO crosses a watermark.
 *   - RINTSTS / IDSTS: W1C. MINTSTS = RINTSTS & INTMASK.
 *   - STATUS: RO computed (BUSY=0, FIFO level/watermarks from
 *     the model FIFO).
 *   - CDETECT: RO tied to sdbus_get_inserted.
 *   - RK vendor regs (0x130-0x138): RAZ/WI via a separate MemoryRegion
 *     aliased over the top of the bank so writes there do not abort.
 *
 * The whole 0x4000 window is covered so no guest MMIO access aborts
 * (D-15): the iomem region covers the core registers plus the 0x200 legacy
 * FIFO data port, and the vendor region covers the rest of the 0x4000 window
 * as RAZ/WI.
 */

#include "qemu/osdep.h"
#include "hw/sd/dw_mmc.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "system/blockdev.h"
#include "system/physmem.h"
#include "exec/cpu-common.h"
#include "qemu/bswap.h"

/* CTRL (0x000) bits. */
#define CTRL_CTRL_RESET     BIT(0)
#define CTRL_FIFO_RESET     BIT(1)
#define CTRL_DMA_RESET      BIT(2)
#define CTRL_INT_ENABLE     BIT(4)
#define CTRL_DMA_ENABLE     BIT(5)
#define CTRL_USE_IDMAC      BIT(25)

/* CMD (0x02c) bits. */
#define CMD_START           BIT(31)
#define CMD_USE_HOLD_REG    BIT(29)
#define CMD_VOLT_SWITCH     BIT(28)
#define CMD_CCS_EXP         BIT(23)
#define CMD_CEATA_RD        BIT(22)
#define CMD_UPD_CLK         BIT(21)
#define CMD_INIT            BIT(15)
#define CMD_STOP            BIT(14)
#define CMD_PRV_DAT_WAIT    BIT(13)
#define CMD_SEND_STOP       BIT(12)
#define CMD_STRM_MODE       BIT(11)
#define CMD_DAT_WR          BIT(10)
#define CMD_DAT_EXP         BIT(9)
#define CMD_RESP_CRC        BIT(8)
#define CMD_RESP_LONG       BIT(7)
#define CMD_RESP_EXP        BIT(6)
#define CMD_INDX            0x3f

/* RINTSTS / INTMASK bit layout. */
#define INT_CD              BIT(0)
#define INT_RESP_ERR        BIT(1)
#define INT_CMD_DONE        BIT(2)
#define INT_DATA_OVER       BIT(3)
#define INT_TXDR            BIT(4)
#define INT_RXDR            BIT(5)
#define INT_RCRC            BIT(6)
#define INT_DCRC            BIT(7)
#define INT_RTO             BIT(8)
#define INT_DRTO            BIT(9)
#define INT_HTO             BIT(10)
#define INT_FRUN            BIT(11)
#define INT_HLE             BIT(12)
#define INT_SBE             BIT(13)
#define INT_ACD             BIT(14)
#define INT_EBE             BIT(15)
#define INT_SDIO(n)         BIT(16 + (n))
#define INT_SDIO_DEFAULT    INT_SDIO(8)  /* RK overloads bit 24 */

/* STATUS (0x048) computed. */
#define STATUS_FIFO_RX_WMARK   BIT(0)
#define STATUS_FIFO_TX_WMARK   BIT(1)
#define STATUS_FCNT_SHIFT   17
#define STATUS_FCNT_MASK    (0x1fff << STATUS_FCNT_SHIFT)
#define STATUS_BUSY         BIT(9)
#define STATUS_FIFO_EMPTY   BIT(2)
#define STATUS_FIFO_FULL    BIT(3)

/* FIFOTH (0x04c) fields. */
#define FIFOTH_TX_WMARK_SHIFT   0
#define FIFOTH_TX_WMARK_MASK    0xfff
#define FIFOTH_RX_WMARK_SHIFT   16
#define FIFOTH_RX_WMARK_MASK    (0xfff << FIFOTH_RX_WMARK_SHIFT)
#define FIFOTH_DMA_MSIZE_SHIFT  28

/* BMOD (0x080) bits. */
#define BMOD_SWRESET        BIT(0)
#define BMOD_FB             BIT(1)
#define BMOD_ENABLE         BIT(7)

/* IDSTS / IDINTEN bits. */
#define IDSTS_TI            BIT(0)
#define IDSTS_RI            BIT(1)
#define IDSTS_FBE           BIT(2)
#define IDSTS_DU            BIT(4)
#define IDSTS_CES           BIT(5)
#define IDSTS_NI            BIT(8)
#define IDSTS_AI            BIT(9)
#define IDMAC_INT_CLR       (IDSTS_TI | IDSTS_RI | IDSTS_FBE | IDSTS_DU | \
                             IDSTS_CES | IDSTS_NI | IDSTS_AI)

/* IDMAC descriptor des0 bits (dw_mmc.c:78-84). */
#define IDMAC_DES0_DIC      BIT(1)
#define IDMAC_DES0_LD       BIT(2)
#define IDMAC_DES0_FD       BIT(3)
#define IDMAC_DES0_CH       BIT(4)
#define IDMAC_DES0_ER       BIT(5)
#define IDMAC_DES0_CES      BIT(30)
#define IDMAC_DES0_OWN      BIT(31)

/* IP defaults the driver probes against. */
#define DW_MMC_CORE_MMIO_SIZE 0x204
#define DW_MMC_VERID        0x270a     /* >= 0x240a also exposes data @ 0x100 */
#define DW_MMC_HCON         0x00000000 /* TRANS_MODE=IDMA, ADDR_CONFIG=32-bit,
                                          NUM_SLOTS=0, HDATA_WIDTH=16-bit */
/* RK3588 overloads SDIO slot 8 -> bit 24 for sdio_irq. */
#define DW_MMC_SDIO_IRQ_BIT 24

static void dw_mmc_fifo_reset(DwMmcState *s);

/* ------------------------------------------------------------------------- */
/* IRQ evaluation                                                            */
/* ------------------------------------------------------------------------- */

static void dw_mmc_update_irq(DwMmcState *s)
{
    uint32_t mintsts = s->rintsts & s->intmask;
    bool raise = (s->ctrl & CTRL_INT_ENABLE) && mintsts != 0;
    qemu_set_irq(s->irq, raise);
}

static void dw_mmc_maybe_send_auto_stop(DwMmcState *s)
{
    SDRequest req = {
        .cmd = 12, /* CMD12: STOP_TRANSMISSION */
        .arg = 0,
    };
    uint8_t response[16];

    if (!s->transfer_send_stop) {
        return;
    }

    s->transfer_send_stop = false;
    sdbus_do_command(&s->sdbus, &req, response, sizeof(response));
    s->rintsts |= INT_ACD;
}

static void dw_mmc_abort_pending_data(DwMmcState *s)
{
    SDRequest req = {
        .cmd = 12, /* CMD12: STOP_TRANSMISSION */
        .arg = 0,
    };
    uint8_t response[16];

    if (!sdbus_data_ready(&s->sdbus) && !sdbus_receive_ready(&s->sdbus)) {
        return;
    }

    sdbus_do_command(&s->sdbus, &req, response, sizeof(response));
    s->rintsts |= INT_ACD;
    s->transfer_active = false;
    s->transfer_bytes_remaining = 0;
    s->transfer_send_stop = false;
    dw_mmc_fifo_reset(s);
}

/* ------------------------------------------------------------------------- */
/* FIFO                                                                      */
/* ------------------------------------------------------------------------- */

static uint32_t dw_mmc_fifo_used_words(DwMmcState *s)
{
    /* STATUS.FCNT reports the number of FIFO words the host can read. */
    return s->fifo_len / 4;
}

static uint32_t dw_mmc_fifo_pop_le32(DwMmcState *s)
{
    uint32_t v;
    if (s->fifo_len < 4) {
        return 0;
    }
    v = ldl_le_p(s->fifo + s->fifo_pos);
    s->fifo_pos += 4;
    s->fifo_len -= 4;
    if (s->fifo_pos >= sizeof(s->fifo)) {
        s->fifo_pos = 0;
    }
    if (s->fifo_len == 0) {
        s->fifo_pos = 0;
    }
    return v;
}

static void dw_mmc_fifo_reset(DwMmcState *s)
{
    s->fifo_len = 0;
    s->fifo_pos = 0;
    memset(s->fifo, 0, sizeof(s->fifo));
}

static void dw_mmc_pio_refill_read_fifo(DwMmcState *s)
{
    uint32_t count;

    if (!s->transfer_active || s->transfer_is_write ||
        s->transfer_bytes_remaining == 0 || s->fifo_len != 0) {
        return;
    }

    count = MIN(s->transfer_bytes_remaining,
                (uint32_t)(sizeof(s->fifo) - s->fifo_len));
    if (count == 0) {
        return;
    }

    sdbus_read_data(&s->sdbus, s->fifo + s->fifo_len, count);
    s->fifo_len += count;
    s->transfer_bytes_remaining -= count;
    s->rintsts |= INT_RXDR;

    if (s->transfer_bytes_remaining == 0) {
        dw_mmc_maybe_send_auto_stop(s);
        s->rintsts |= INT_DATA_OVER;
    }
    dw_mmc_update_irq(s);
}

static uint32_t dw_mmc_read_data_port(DwMmcState *s, uint32_t fallback)
{
    uint32_t r = fallback;

    if (s->transfer_active && !s->transfer_is_write && s->fifo_len >= 4) {
        r = dw_mmc_fifo_pop_le32(s);
        if (s->fifo_len == 0) {
            if (s->transfer_bytes_remaining != 0) {
                dw_mmc_pio_refill_read_fifo(s);
            } else {
                dw_mmc_maybe_send_auto_stop(s);
                s->rintsts |= INT_DATA_OVER;
                s->transfer_active = false;
                dw_mmc_update_irq(s);
            }
        }
    }

    return r;
}

static void dw_mmc_write_data_port(DwMmcState *s, uint32_t value,
                                   bool fallback_to_cdthrctl)
{
    if (s->transfer_active && s->transfer_is_write) {
        uint8_t buf[4];

        stl_le_p(buf, value);
        sdbus_write_data(&s->sdbus, buf, sizeof(buf));

        if (s->transfer_bytes_remaining > sizeof(buf)) {
            s->transfer_bytes_remaining -= sizeof(buf);
            s->rintsts |= INT_TXDR;
        } else {
            s->transfer_bytes_remaining = 0;
            s->transfer_active = false;
            dw_mmc_maybe_send_auto_stop(s);
            s->rintsts |= INT_DATA_OVER;
        }
        dw_mmc_update_irq(s);
    } else if (fallback_to_cdthrctl) {
        s->cdthrctl = value;
    }
}

/* ------------------------------------------------------------------------- */
/* IDMAC descriptor-DMA engine                                                */
/* ------------------------------------------------------------------------- */

/*
 * Walk the IDMAC descriptor ring starting at s->dbaddr, transfer
 * buffer1 of each descriptor between the SD card and guest RAM, clear
 * OWN per descriptor, and raise IDSTS RI (read) or TI (write) + NI
 * summary at the end. RXTX direction is determined by the last
 * CMD's CMD_DAT_WR bit.
 *
 * 32-bit mode only (HCON[ADDR_CONFIG]=0 on RK3588).
 */
static void dw_mmc_idmac_kick(DwMmcState *s)
{
    uint32_t desc_addr = s->dbaddr & ~0x3u;
    uint8_t desc[16];
    bool is_write = s->transfer_is_write;
    uint32_t processed = 0;

    while (processed++ < DW_MMC_IDMAC_MAX_DESCS) {
        physical_memory_read(desc_addr, desc, sizeof(desc));
        uint32_t des0 = ldl_le_p(desc + 0);
        uint32_t des1 = ldl_le_p(desc + 4);
        uint32_t des2 = ldl_le_p(desc + 8);
        uint32_t des3 = ldl_le_p(desc + 12);

        if (!(des0 & IDMAC_DES0_OWN)) {
            /* Not owned by DMA - engine stops here. */
            break;
        }
        if (s->transfer_bytes_remaining == 0) {
            break;
        }

        uint32_t buf_size = des1 & 0x1fff;
        if (buf_size == 0) {
            buf_size = 0x2000;
        }
        if (s->transfer_bytes_remaining != 0) {
            buf_size = MIN(buf_size, s->transfer_bytes_remaining);
        }
        if (buf_size > 0 && des2 != 0) {
            if (is_write) {
                /* TX: host RAM -> FIFO -> card. */
                g_autofree uint8_t *buf = g_malloc(buf_size);
                physical_memory_read(des2, buf, buf_size);
                sdbus_write_data(&s->sdbus, buf, buf_size);
            } else {
                /* RX: card -> FIFO -> host RAM. */
                g_autofree uint8_t *buf = g_malloc(buf_size);
                memset(buf, 0, buf_size);
                sdbus_read_data(&s->sdbus, buf, buf_size);
                physical_memory_write(des2, buf, buf_size);
            }
            if (s->transfer_bytes_remaining > buf_size) {
                s->transfer_bytes_remaining -= buf_size;
            } else {
                s->transfer_bytes_remaining = 0;
            }
        }

        /* Hand the descriptor back to the host (clear OWN). */
        des0 &= ~IDMAC_DES0_OWN;
        stl_le_p(desc + 0, des0);
        physical_memory_write(desc_addr, desc, sizeof(desc));

        if (des0 & IDMAC_DES0_LD) {
            /* Last descriptor - raise completion. */
            break;
        }
        if (s->transfer_bytes_remaining == 0) {
            break;
        }
        if (!(des0 & IDMAC_DES0_CH)) {
            /* No chaining - stop. */
            break;
        }

        /* Follow des3 -> next descriptor. */
        desc_addr = des3 & ~0x3u;
        if (desc_addr == 0) {
            break;
        }
    }

    s->idsts |= is_write ? IDSTS_TI : IDSTS_RI;
    s->idsts |= IDSTS_NI;
    dw_mmc_maybe_send_auto_stop(s);
    s->rintsts |= INT_DATA_OVER;
    s->transfer_active = false;

    if (s->idinten & (IDSTS_TI | IDSTS_RI | IDSTS_NI)) {
        /* IDSTS interrupts feed the host IRQ via the IDINTEN mask;
         * dw_mmc.c also gates these on INT_DATA_OVER in MINTSTS, but
         * the controller routes IDMAC irqs via the SDMMC_INT_IDMAC
         * path (bit not in INTMASK) - keep simple: just mirror the
         * DATA_OVER into MINTSTS so the host IRQ fires. */
    }
    dw_mmc_update_irq(s);
}

/* ------------------------------------------------------------------------- */
/* Command sequencing                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Issue the command described by CMDARG + the CMD register's flag
 * bits. Reads the response from the attached SD card and lays it out
 * in RESP0-3. Sets CMD_DONE (or RTO) and either kicks the IDMAC
 * engine (if DAT_EXP && CTRL[USE_IDMAC] && BMOD[ENABLE]) or sets up
 * the FIFO transfer for PIO. The START bit self-clears on completion.
 *
 * R2 long-response mapping (dw_mmc.c:1782-1787): the controller
 * presents the card's 16-byte big-endian R2 with RESP0<-resp[3],
 * RESP1<-resp[2], RESP2<-resp[1], RESP3<-resp[0]&~1. The Linux
 * driver reads RESP0 into cmd->resp[3], so this is the layout it
 * expects.
 */
static void dw_mmc_issue_command(DwMmcState *s)
{
    SDRequest req;
    uint8_t response[16];
    size_t rlen;
    bool expect_resp = s->cmd & CMD_RESP_EXP;
    bool long_resp = s->cmd & CMD_RESP_LONG;
    bool data_expected = s->cmd & CMD_DAT_EXP;
    bool data_write = s->cmd & CMD_DAT_WR;

    if (s->cmd & CMD_UPD_CLK) {
        s->rintsts |= INT_CMD_DONE;
        s->cmd &= ~CMD_START;
        dw_mmc_update_irq(s);
        return;
    }

    req.cmd = s->cmd & CMD_INDX;
    req.arg = s->cmdarg;

    if (req.cmd != 12 && req.cmd != 13) {
        dw_mmc_abort_pending_data(s);
    }

    rlen = sdbus_do_command(&s->sdbus, &req, response, sizeof(response));
    if (rlen == 0 && data_expected && expect_resp &&
        (req.cmd == 18 || req.cmd == 25)) {
        /*
         * Some firmware probes with CMD55 and then immediately issues a normal
         * multi-block data command.  The SD model treats CMD55+CMD18/CMD25 as
         * unimplemented SD security commands; retry once after that prefix has
         * been consumed so the ordinary data path can proceed.
         */
        rlen = sdbus_do_command(&s->sdbus, &req, response, sizeof(response));
    }

    if (expect_resp) {
        if (rlen == 0) {
            s->rintsts |= INT_RTO;
        } else if (long_resp && rlen == 16) {
            /* R2-style 128-bit response; see dw_mmc.c:1782-1787. */
            s->resp[0] = ldl_be_p(response + 12) & ~1u;
            s->resp[1] = ldl_be_p(response + 8);
            s->resp[2] = ldl_be_p(response + 4);
            s->resp[3] = ldl_be_p(response + 0);
            s->rintsts |= INT_CMD_DONE;
        } else if (rlen == 4) {
            s->resp[0] = ldl_be_p(response + 0);
            s->resp[1] = s->resp[2] = s->resp[3] = 0;
            s->rintsts |= INT_CMD_DONE;
        } else if (rlen == 16) {
            /* Card gave a long response we didn't expect as long -
             * treat as short: copy first 4 bytes. */
            s->resp[0] = ldl_be_p(response + 0);
            s->resp[1] = s->resp[2] = s->resp[3] = 0;
            s->rintsts |= INT_CMD_DONE;
        } else {
            s->rintsts |= INT_RESP_ERR;
        }
    } else {
        /* No response expected - CMD_SENT. */
        s->rintsts |= INT_CMD_DONE;
    }
    (void)data_write;

    if (data_expected) {
        /* Set up the data phase. */
        s->transfer_bytes_remaining = s->bytcnt;
        s->transfer_is_write = data_write;
        s->transfer_send_stop = s->cmd & CMD_SEND_STOP;
        s->transfer_active = true;
        if ((s->ctrl & (CTRL_DMA_ENABLE | CTRL_USE_IDMAC)) ==
            (CTRL_DMA_ENABLE | CTRL_USE_IDMAC) &&
            (s->bmod & BMOD_ENABLE)) {
            dw_mmc_idmac_kick(s);
        } else {
            /*
             * PIO: signal RXDR/TXDR immediately so the host can
             * start filling/draining the FIFO via the data port.
             */
            if (data_write) {
                s->rintsts |= INT_TXDR;
            } else {
                dw_mmc_pio_refill_read_fifo(s);
            }
        }
    }

    /* START self-clears on completion. */
    s->cmd &= ~CMD_START;
    dw_mmc_update_irq(s);
}

/* ------------------------------------------------------------------------- */
/* MMIO ops                                                                   */
/* ------------------------------------------------------------------------- */

static uint64_t dw_mmc_read(void *opaque, hwaddr offset, unsigned size)
{
    DwMmcState *s = DW_MMC(opaque);
    uint32_t r = 0;

    switch (offset) {
    case 0x000: /* CTRL */
        r = s->ctrl;
        break;
    case 0x004:
        r = s->pwren;
        break;
    case 0x008:
        r = s->clkdiv;
        break;
    case 0x00c:
        r = s->clksrc;
        break;
    case 0x010:
        r = s->clkena;
        break;
    case 0x014:
        r = s->tmout;
        break;
    case 0x018:
        r = s->ctype;
        break;
    case 0x01c:
        r = s->blksiz;
        break;
    case 0x020:
        r = s->bytcnt;
        break;
    case 0x024:
        r = s->intmask;
        break;
    case 0x028:
        r = s->cmdarg;
        break;
    case 0x02c:
        r = s->cmd;
        break;
    case 0x030:
        r = s->resp[0];
        break;
    case 0x034:
        r = s->resp[1];
        break;
    case 0x038:
        r = s->resp[2];
        break;
    case 0x03c:
        r = s->resp[3];
        break;
    case 0x040: /* MINTSTS = RINTSTS & INTMASK */
        r = s->rintsts & s->intmask;
        break;
    case 0x044:
        r = s->rintsts;
        break;
    case 0x048: { /* STATUS - computed. */
        uint32_t used_words = dw_mmc_fifo_used_words(s);
        uint32_t tx_wmark = (s->fifoth & FIFOTH_TX_WMARK_MASK) >>
                            FIFOTH_TX_WMARK_SHIFT;
        uint32_t rx_wmark = (s->fifoth & FIFOTH_RX_WMARK_MASK) >>
                            FIFOTH_RX_WMARK_SHIFT;

        r = (used_words << STATUS_FCNT_SHIFT) & STATUS_FCNT_MASK;
        if (s->fifo_len == 0) {
            r |= STATUS_FIFO_EMPTY;
        }
        if (s->fifo_len == sizeof(s->fifo)) {
            r |= STATUS_FIFO_FULL;
        }
        if (used_words <= tx_wmark) {
            r |= STATUS_FIFO_TX_WMARK;
        }
        if (used_words > rx_wmark) {
            r |= STATUS_FIFO_RX_WMARK;
        }
        break;
    }
    case 0x04c:
        r = s->fifoth;
        break;
    case 0x050: /* CDETECT - bit0=0 means card present (active low). */
        r = sdbus_get_inserted(&s->sdbus) ? 0 : 1;
        break;
    case 0x054: /* WRTPRT */
        r = sdbus_get_readonly(&s->sdbus) ? 1 : 0;
        break;
    case 0x058: /* GPIO */
        r = 0;
        break;
    case 0x05c: /* TCBCNT */
        r = 0;
        break;
    case 0x060: /* TBBCNT */
        r = 0;
        break;
    case 0x064:
        r = s->debnce;
        break;
    case 0x068:
        r = s->usrid;
        break;
    case 0x06c:
        r = s->verid;
        break;
    case 0x070:
        r = s->hcon;
        break;
    case 0x074:
        r = s->uhs_reg;
        break;
    case 0x078:
        r = s->rst_n;
        break;
    case 0x080:
        r = s->bmod;
        break;
    case 0x084: /* PLDMND - WO */
        r = 0;
        break;
    case 0x088:
        r = s->dbaddr;
        break;
    case 0x08c:
        r = s->idsts;
        break;
    case 0x090:
        r = s->idinten;
        break;
    case 0x094: /* DSCADDR */
        r = 0;
        break;
    case 0x098: /* BUFADDR */
        r = 0;
        break;
    case 0x100: /* CDTHRCTL or FIFO data port for VERID >= 0x240a. */
        r = dw_mmc_read_data_port(s, s->cdthrctl);
        break;
    case 0x200: /* Legacy FIFO data port used by U-Boot dw_mmc. */
        r = dw_mmc_read_data_port(s, 0);
        break;
    case 0x108: /* UHS_REG_EXT */
        r = 0;
        break;
    case 0x10c: /* DDR_REG */
        r = 0;
        break;
    case 0x110:
        r = s->enable_shift;
        break;
    default:
        if (offset >= 0x114 && offset < DW_MMC_CORE_MMIO_SIZE) {
            r = 0;
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dw_mmc: unsupported read offset 0x%03x\n",
                      (unsigned)offset);
        r = 0;
        break;
    }

    return r;
}

static void dw_mmc_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    DwMmcState *s = DW_MMC(opaque);
    uint32_t v = (uint32_t)value;

    switch (offset) {
    case 0x000: /* CTRL */
        s->ctrl = v & ~(CTRL_CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
        if (v & CTRL_FIFO_RESET) {
            dw_mmc_fifo_reset(s);
        }
        if (v & CTRL_DMA_RESET) {
            s->idsts = 0;
        }
        if (v & CTRL_CTRL_RESET) {
            s->transfer_active = false;
            s->transfer_bytes_remaining = 0;
            s->transfer_send_stop = false;
        }
        /* Reset bits self-clear immediately in this model. */
        dw_mmc_update_irq(s);
        break;
    case 0x004:
        s->pwren = v;
        break;
    case 0x008:
        s->clkdiv = v;
        break;
    case 0x00c:
        /* CLKSRC writes are ignored except 0 (single clock source). */
        s->clksrc = 0;
        break;
    case 0x010:
        s->clkena = v;
        break;
    case 0x014:
        s->tmout = v;
        break;
    case 0x018:
        s->ctype = v;
        break;
    case 0x01c:
        s->blksiz = v;
        break;
    case 0x020:
        s->bytcnt = v;
        break;
    case 0x024: /* INTMASK */
        s->intmask = v;
        dw_mmc_update_irq(s);
        break;
    case 0x028:
        s->cmdarg = v;
        break;
    case 0x02c: /* CMD */
        s->cmd = v;
        if (v & CMD_START) {
            dw_mmc_issue_command(s);
        }
        break;
    case 0x030: case 0x034: case 0x038: case 0x03c:
        /* RESP* are RO. */
        break;
    case 0x040: /* MINTSTS - RO. */
        break;
    case 0x044: /* RINTSTS - W1C. */
        s->rintsts &= ~v;
        dw_mmc_update_irq(s);
        break;
    case 0x048: /* STATUS - RO. */
        break;
    case 0x04c: /* FIFOTH */
        s->fifoth = v;
        break;
    case 0x050: /* CDETECT - RO. */
        break;
    case 0x054:
        break;
    case 0x058: /* GPIO - RAZ/WI */
        break;
    case 0x05c:
    case 0x060: /* TCBCNT/TBBCNT RO */
        break;
    case 0x064:
        s->debnce = v;
        break;
    case 0x068:
        s->usrid = v;
        break;
    case 0x06c: /* VERID RO */
        break;
    case 0x070: /* HCON RO */
        break;
    case 0x074:
        s->uhs_reg = v;
        break;
    case 0x078:
        s->rst_n = v;
        break;
    case 0x080: /* BMOD - SWRESET self-clears. */
        s->bmod = v & ~BMOD_SWRESET;
        break;
    case 0x084: /* PLDMND - write 1 kicks IDMAC. */
        if (v && (s->ctrl & (CTRL_DMA_ENABLE | CTRL_USE_IDMAC)) &&
            (s->bmod & BMOD_ENABLE) && s->transfer_active) {
            dw_mmc_idmac_kick(s);
        }
        break;
    case 0x088:
        s->dbaddr = v;
        break;
    case 0x08c: /* IDSTS - W1C. */
        s->idsts &= ~v;
        dw_mmc_update_irq(s);
        break;
    case 0x090:
        s->idinten = v;
        break;
    case 0x094:
    case 0x098: /* DSCADDR/BUFADDR RO */
        break;
    case 0x100: /* CDTHRCTL or FIFO data port for VERID >= 0x240a. */
        dw_mmc_write_data_port(s, v, true);
        break;
    case 0x200: /* Legacy FIFO data port used by U-Boot dw_mmc. */
        dw_mmc_write_data_port(s, v, false);
        break;
    case 0x108:
    case 0x10c: /* UHS_REG_EXT / DDR_REG */
        break;
    case 0x110:
        s->enable_shift = v;
        break;
    default:
        if (offset >= 0x114 && offset < DW_MMC_CORE_MMIO_SIZE) {
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dw_mmc: unsupported write offset 0x%03x val 0x%x\n",
                      (unsigned)offset, v);
        break;
    }
}

static const MemoryRegionOps dw_mmc_ops = {
    .read = dw_mmc_read,
    .write = dw_mmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* RAZ/WI ops for the RK vendor tail after the legacy FIFO data port. */
static uint64_t dw_mmc_vendor_read(void *opaque, hwaddr off, unsigned sz)
{
    return 0;
}

static void dw_mmc_vendor_write(void *opaque, hwaddr off, uint64_t v,
                                unsigned sz)
{
    /* drop */
}

static const MemoryRegionOps dw_mmc_vendor_ops = {
    .read = dw_mmc_vendor_read,
    .write = dw_mmc_vendor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* ------------------------------------------------------------------------- */
/* Device lifecycle                                                          */
/* ------------------------------------------------------------------------- */

static void dw_mmc_set_inserted(DeviceState *dev, bool inserted)
{
    DwMmcState *s = DW_MMC(dev);
    if (inserted) {
        s->rintsts &= ~INT_CD;
    } else {
        s->rintsts |= INT_CD;
    }
    dw_mmc_update_irq(s);
}

static void dw_mmc_set_readonly(DeviceState *dev, bool readonly)
{
    /* nothing modelled */
}

static void dw_mmc_reset(DeviceState *dev)
{
    DwMmcState *s = DW_MMC(dev);

    s->ctrl = 0;
    s->pwren = 0;
    s->clkdiv = 0;
    s->clksrc = 0;
    s->clkena = 0;
    s->tmout = 0xffffffff;
    s->ctype = 0;
    s->blksiz = 0;
    s->bytcnt = 0;
    s->intmask = 0;
    s->cmdarg = 0;
    s->cmd = 0;
    memset(s->resp, 0, sizeof(s->resp));
    s->rintsts = 0;
    s->fifoth = 0x00800000; /* POR: RX watermark = depth-1 */
    s->debnce = 0;
    s->usrid = 0;
    s->verid = DW_MMC_VERID;
    s->hcon = DW_MMC_HCON;
    s->uhs_reg = 0;
    s->rst_n = 1;           /* RST_HWACTIVE active high */
    s->bmod = 0;
    s->dbaddr = 0;
    s->idsts = 0;
    s->idinten = 0;
    s->cdthrctl = 0;
    s->enable_shift = 0;

    s->transfer_bytes_remaining = 0;
    s->transfer_is_write = false;
    s->transfer_send_stop = false;
    s->transfer_active = false;
    dw_mmc_fifo_reset(s);

    dw_mmc_update_irq(s);
}

static void dw_mmc_init(Object *obj)
{
    DwMmcState *s = DW_MMC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dw_mmc_ops, s, "dw-mmc",
                          DW_MMC_CORE_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    /* RK vendor / RAZ tail after the legacy FIFO data port. */
    memory_region_init_io(&s->vendor, obj, &dw_mmc_vendor_ops, s,
                          "dw-mmc-vendor",
                          DW_MMC_MMIO_SIZE - DW_MMC_CORE_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->vendor);

    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(s), &s->card_inserted,
                             "card-inserted", 1);
    qdev_init_gpio_out_named(DEVICE(s), &s->card_readonly,
                             "card-read-only", 1);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_DW_MMC_BUS, DEVICE(s),
              "sd-bus");
}

static void dw_mmc_realize(DeviceState *dev, Error **errp)
{
    DwMmcState *s = DW_MMC(dev);

    /* Initialize the SD card (if any) attached via the bus. */
    /* Card creation is handled by the board (mirrors sdhci). */
    (void)s;
}

static void dw_mmc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = dw_mmc_realize;
    device_class_set_legacy_reset(dc, dw_mmc_reset);
    dc->user_creatable = false;
}

static void dw_mmc_bus_class_init(ObjectClass *oc, const void *data)
{
    SDBusClass *sbc = SD_BUS_CLASS(oc);
    sbc->set_inserted = dw_mmc_set_inserted;
    sbc->set_readonly = dw_mmc_set_readonly;
}

static const TypeInfo dw_mmc_types[] = {
    {
        .name = TYPE_DW_MMC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(DwMmcState),
        .instance_init = dw_mmc_init,
        .class_init = dw_mmc_class_init,
    },
    {
        .name = TYPE_DW_MMC_BUS,
        .parent = TYPE_SD_BUS,
        .instance_size = sizeof(SDBus),
        .class_init = dw_mmc_bus_class_init,
    },
};

DEFINE_TYPES(dw_mmc_types)
