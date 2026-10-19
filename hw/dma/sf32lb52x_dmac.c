/*
 * SiFli SF32LB52X DMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout follows the SF32LB52x SVD DMAC1 block (ISR/IFCR, eight
 * channel banks at stride 0x14, CSELR1/CSELR2 with 6-bit request selectors)
 * and the vendor HAL cmsis/sf32lb52x/dmac.h definitions; behavior follows the
 * Zephyr drivers/dma/dma_sf32lb.c config/reload/start/stop/get_status and ISR
 * flow.
 *
 * Model behaviour:
 *  - 8 channels; each channel bank is CCR/CNDTR/CPAR/CM0AR/CBSR;
 *  - a channel start (CCR.EN 0->1) with MEM2MEM=1 executes a memory-to-memory
 *    copy of CNDTR items between CPAR and CM0AR through the system address
 *    space, from a bottom half (so the EN write returns before completion);
 *  - completion (or bus error) sets the per-channel flags in ISR, raises the
 *    per-channel level IRQ when the matching CCR enable bit is set, clears
 *    EN and decrements CNDTR;
 *  - IFCR is write-1-to-clear; CCR/CNDTR/CPAR/CM0AR/CBSR/CSELR/DBGSEL are
 *    plain RW with documented reserved bits;
 *  - CIRC reloads CNDTR/CPAR/CM0AR and keeps EN after a pass (one pass is
 *    executed; continuous request-driven looping is not modelled).
 *
 * Explicit simplifications (logged with LOG_UNIMP):
 *  - request-gated transfers (MEM2MEM=0): the 64 hardware DREQ lines are not
 *    modelled, so the channel runs as soon as EN is set with CPAR and CM0AR
 *    as its endpoints. The Zephyr MPI flash driver relies on this completion
 *    model;
 *  - DMA master accesses bypass the M33 MPU (TCG has no bus-master MPU), and
 *    there is no DMA latency other than the bottom-half hop.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/dma/sf32lb52x_dmac.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "system/address-spaces.h"
#include "system/dma.h"

REG32(ISR, 0x00)
REG32(IFCR, 0x04)
REG32(CCR, 0x08)
    FIELD(CCR, EN, 0, 1)
    FIELD(CCR, TCIE, 1, 1)
    FIELD(CCR, HTIE, 2, 1)
    FIELD(CCR, TEIE, 3, 1)
    FIELD(CCR, DIR, 4, 1)
    FIELD(CCR, CIRC, 5, 1)
    FIELD(CCR, PINC, 6, 1)
    FIELD(CCR, MINC, 7, 1)
    FIELD(CCR, PSIZE, 8, 2)
    FIELD(CCR, MSIZE, 10, 2)
    FIELD(CCR, PL, 12, 2)
    FIELD(CCR, MEM2MEM, 14, 1)
REG32(CNDTR, 0x0c)
    FIELD(CNDTR, NDT, 0, 16)
REG32(CPAR, 0x10)
REG32(CM0AR, 0x14)
REG32(CBSR, 0x18)
    FIELD(CBSR, BS, 0, 8)
REG32(CSELR1, 0xa8)
    FIELD(CSELR1, C1S, 0, 6)
    FIELD(CSELR1, C2S, 8, 6)
    FIELD(CSELR1, C3S, 16, 6)
    FIELD(CSELR1, C4S, 24, 6)
REG32(CSELR2, 0xac)
    FIELD(CSELR2, C5S, 0, 6)
    FIELD(CSELR2, C6S, 8, 6)
    FIELD(CSELR2, C7S, 16, 6)
    FIELD(CSELR2, C8S, 24, 6)
REG32(DBGSEL, 0xb0)

#define SF32LB52X_DMAC_CHANNEL_STRIDE 0x14

#define A_DMAC_CCR(ch)   (A_CCR + SF32LB52X_DMAC_CHANNEL_STRIDE * (ch))
#define A_DMAC_CNDTR(ch) (A_DMAC_CCR(ch) + 0x04)
#define A_DMAC_CPAR(ch)  (A_DMAC_CCR(ch) + 0x08)
#define A_DMAC_CM0AR(ch) (A_DMAC_CCR(ch) + 0x0c)
#define A_DMAC_CBSR(ch)  (A_DMAC_CCR(ch) + 0x10)

#define R_DMAC_CCR(ch)   (A_DMAC_CCR(ch) / 4)
#define R_DMAC_CNDTR(ch) (A_DMAC_CNDTR(ch) / 4)
#define R_DMAC_CPAR(ch)  (A_DMAC_CPAR(ch) / 4)
#define R_DMAC_CM0AR(ch) (A_DMAC_CM0AR(ch) / 4)
#define R_DMAC_CBSR(ch)  (A_DMAC_CBSR(ch) / 4)

/* ISR/IFCR bit assignment: 4 bits per channel, GIF/TCIF/HTIF/TEIF ascending. */
#define DMAC_ISR_GIF(ch)   (1U << (4 * (ch) + 0))
#define DMAC_ISR_TCIF(ch)  (1U << (4 * (ch) + 1))
#define DMAC_ISR_HTIF(ch)  (1U << (4 * (ch) + 2))
#define DMAC_ISR_TEIF(ch)  (1U << (4 * (ch) + 3))
#define DMAC_ISR_CH_MASK(ch) (0xfU << (4 * (ch)))

#define SF32LB52X_DMAC_CCR_RSVD   (~(uint64_t)0x7fff)
#define SF32LB52X_DMAC_CNDTR_RSVD (~(uint64_t)0xffff)
#define SF32LB52X_DMAC_CBSR_RSVD  (~(uint64_t)0xff)
#define SF32LB52X_DMAC_CSELR_RSVD (~(uint64_t)0x3f3f3f3f)

static int sf32lb52x_dmac_channel_from_reg(RegisterInfo *reg)
{
    return (reg->access->addr - A_DMAC_CCR(0)) / SF32LB52X_DMAC_CHANNEL_STRIDE;
}

static void sf32lb52x_dmac_update_irq(SF32LB52XDMACState *s, int ch)
{
    uint32_t ccr = s->regs[R_DMAC_CCR(ch)];
    uint32_t isr = s->regs[R_ISR];
    bool level;

    level = ((isr & DMAC_ISR_TCIF(ch)) && (ccr & R_CCR_TCIE_MASK)) ||
            ((isr & DMAC_ISR_HTIF(ch)) && (ccr & R_CCR_HTIE_MASK)) ||
            ((isr & DMAC_ISR_TEIF(ch)) && (ccr & R_CCR_TEIE_MASK));
    qemu_set_irq(s->irq[ch], level);
}

static void sf32lb52x_dmac_update_all_irqs(SF32LB52XDMACState *s)
{
    int ch;

    for (ch = 0; ch < SF32LB52X_DMAC_NUM_CHANNELS; ch++) {
        sf32lb52x_dmac_update_irq(s, ch);
    }
}

static bool sf32lb52x_dmac_copy_item(int ch, uint32_t src, uint32_t dst,
                                     unsigned int src_size,
                                     unsigned int dst_size)
{
    uint8_t src_buf[4] = { 0 };
    uint8_t dst_buf[4] = { 0 };
    unsigned int i;

    if (dma_memory_read(&address_space_memory, src, src_buf, src_size,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: channel %d read error at 0x%08" PRIx32 "\n",
                      __func__, ch, src);
        return false;
    }

    /* Mismatched PSIZE/MSIZE are truncated/zero-extended (driver forbids). */
    for (i = 0; i < MIN(src_size, dst_size); i++) {
        dst_buf[i] = src_buf[i];
    }

    if (dma_memory_write(&address_space_memory, dst, dst_buf, dst_size,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: channel %d write error at 0x%08" PRIx32 "\n",
                      __func__, ch, dst);
        return false;
    }

    return true;
}

static void sf32lb52x_dmac_complete_channel(SF32LB52XDMACState *s, int ch)
{
    uint32_t ccr = s->regs[R_DMAC_CCR(ch)];

    if (ccr & R_CCR_CIRC_MASK) {
        s->regs[R_DMAC_CNDTR(ch)] = s->circ_cndtr[ch];
        s->regs[R_DMAC_CPAR(ch)] = s->circ_cpar[ch];
        s->regs[R_DMAC_CM0AR(ch)] = s->circ_cm0ar[ch];
    } else {
        s->regs[R_DMAC_CCR(ch)] &= ~(uint32_t)R_CCR_EN_MASK;
        s->en_prev[ch] = 0;
    }

    s->regs[R_ISR] |= DMAC_ISR_TCIF(ch) | DMAC_ISR_GIF(ch);
    sf32lb52x_dmac_update_irq(s, ch);
}

static void sf32lb52x_dmac_error_channel(SF32LB52XDMACState *s, int ch)
{
    s->regs[R_ISR] |= DMAC_ISR_TEIF(ch) | DMAC_ISR_GIF(ch);
    s->regs[R_DMAC_CCR(ch)] &= ~(uint32_t)R_CCR_EN_MASK;
    s->en_prev[ch] = 0;
    sf32lb52x_dmac_update_irq(s, ch);
}

static void sf32lb52x_dmac_run_channel(SF32LB52XDMACState *s, int ch)
{
    uint32_t ccr = s->regs[R_DMAC_CCR(ch)];
    uint32_t ndt = FIELD_EX32(s->regs[R_DMAC_CNDTR(ch)], CNDTR, NDT);
    uint32_t psize, msize;
    uint32_t src, dst;
    unsigned int src_size, dst_size;
    bool pinc, minc, dir_m2p;
    uint32_t i;

    if (s->resetting || !(ccr & R_CCR_EN_MASK)) {
        return;
    }

    if (!(ccr & R_CCR_MEM2MEM_MASK)) {
        /*
         * Request lines are not modeled: a request-gated (peripheral or
         * memory-to-peripheral) channel runs as soon as EN is set, with
         * CPAR and CM0AR as its two endpoints. The MPI
         * QSPI flash driver depends on this completion model.
         */
        qemu_log_mask(LOG_UNIMP,
                      "%s: channel %d: request lines not modelled; "
                      "transfer runs on enable\n", __func__, ch);
    }

    psize = FIELD_EX32(ccr, CCR, PSIZE);
    msize = FIELD_EX32(ccr, CCR, MSIZE);
    if (psize > 2 || msize > 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: channel %d: invalid PSIZE=%" PRIu32
                      " MSIZE=%" PRIu32 "\n", __func__, ch, psize, msize);
        sf32lb52x_dmac_error_channel(s, ch);
        return;
    }

    dir_m2p = FIELD_EX32(ccr, CCR, DIR);
    pinc = FIELD_EX32(ccr, CCR, PINC);
    minc = FIELD_EX32(ccr, CCR, MINC);

    /*
     * DIR=0: CPAR is the peripheral (source), CM0AR the memory (destination);
     * DIR=1: CPAR is the peripheral (destination), CM0AR the memory (source).
     * In memory-to-memory mode CPAR is the source (driver configures it so).
     */
    if (dir_m2p) {
        src = s->regs[R_DMAC_CM0AR(ch)];
        src_size = 1U << msize;
        dst = s->regs[R_DMAC_CPAR(ch)];
        dst_size = 1U << psize;
    } else {
        src = s->regs[R_DMAC_CPAR(ch)];
        src_size = 1U << psize;
        dst = s->regs[R_DMAC_CM0AR(ch)];
        dst_size = 1U << msize;
    }

    for (i = 0; i < ndt; i++) {
        if (!sf32lb52x_dmac_copy_item(ch, src, dst, src_size, dst_size)) {
            s->regs[R_DMAC_CNDTR(ch)] = (ndt - i) & R_CNDTR_NDT_MASK;
            sf32lb52x_dmac_error_channel(s, ch);
            return;
        }

        if (dir_m2p) {
            /* PMEM: MINC advances the memory source, PINC the peripheral. */
            if (minc) {
                src += src_size;
            }
            if (pinc) {
                dst += dst_size;
            }
        } else {
            /* P2MEM: PINC advances the peripheral source, MINC the memory. */
            if (pinc) {
                src += src_size;
            }
            if (minc) {
                dst += dst_size;
            }
        }

        s->regs[R_DMAC_CNDTR(ch)] = (ndt - i - 1) & R_CNDTR_NDT_MASK;

        if (ndt > 1 && (i + 1) == ndt / 2 &&
            !(s->regs[R_ISR] & DMAC_ISR_HTIF(ch))) {
            s->regs[R_ISR] |= DMAC_ISR_HTIF(ch) | DMAC_ISR_GIF(ch);
            sf32lb52x_dmac_update_irq(s, ch);
        }
    }

    sf32lb52x_dmac_complete_channel(s, ch);
}

static void sf32lb52x_dmac_transfer_bh(void *opaque)
{
    SF32LB52XDMACState *s = opaque;
    uint8_t pending = s->pending;
    int ch;

    s->pending = 0;

    RCU_READ_LOCK_GUARD();

    for (ch = 0; ch < SF32LB52X_DMAC_NUM_CHANNELS; ch++) {
        if (pending & (1U << ch)) {
            sf32lb52x_dmac_run_channel(s, ch);
        }
    }
}

static void sf32lb52x_dmac_start_channel(SF32LB52XDMACState *s, int ch)
{
    s->circ_cndtr[ch] = s->regs[R_DMAC_CNDTR(ch)];
    s->circ_cpar[ch] = s->regs[R_DMAC_CPAR(ch)];
    s->circ_cm0ar[ch] = s->regs[R_DMAC_CM0AR(ch)];
    s->pending |= 1U << ch;
    qemu_bh_schedule(s->bh);
}

static void sf32lb52x_dmac_ccr_post_write(RegisterInfo *reg, uint64_t val)
{
    SF32LB52XDMACState *s = SF32LB52X_DMAC(reg->opaque);
    int ch = sf32lb52x_dmac_channel_from_reg(reg);
    bool en = FIELD_EX32((uint32_t)val, CCR, EN);

    if (s->resetting) {
        s->en_prev[ch] = en;
        return;
    }

    if (en && !s->en_prev[ch]) {
        sf32lb52x_dmac_start_channel(s, ch);
    } else if (!en && s->en_prev[ch]) {
        s->pending &= ~(1U << ch);
    }

    s->en_prev[ch] = en;
    sf32lb52x_dmac_update_irq(s, ch);
}

static void sf32lb52x_dmac_ifcr_post_write(RegisterInfo *reg, uint64_t val)
{
    SF32LB52XDMACState *s = SF32LB52X_DMAC(reg->opaque);
    uint32_t v = (uint32_t)val;
    int ch;

    if (s->resetting) {
        return;
    }

    for (ch = 0; ch < SF32LB52X_DMAC_NUM_CHANNELS; ch++) {
        if (!(v & DMAC_ISR_CH_MASK(ch))) {
            continue;
        }

        if (v & DMAC_ISR_GIF(ch)) {
            s->regs[R_ISR] &= ~DMAC_ISR_CH_MASK(ch);
        } else {
            if (v & DMAC_ISR_TCIF(ch)) {
                s->regs[R_ISR] &= ~DMAC_ISR_TCIF(ch);
            }
            if (v & DMAC_ISR_HTIF(ch)) {
                s->regs[R_ISR] &= ~DMAC_ISR_HTIF(ch);
            }
            if (v & DMAC_ISR_TEIF(ch)) {
                s->regs[R_ISR] &= ~DMAC_ISR_TEIF(ch);
            }
        }
    }

    /* IFCR is a write-only clear register. */
    s->regs[R_IFCR] = 0;

    sf32lb52x_dmac_update_all_irqs(s);
}

static const RegisterAccessInfo sf32lb52x_dmac_regs_info[] = {
    {
        .name = "ISR",
        .addr = A_ISR,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "IFCR",
        .addr = A_IFCR,
        .post_write = sf32lb52x_dmac_ifcr_post_write,
    },
    { .name = "CCR1", .addr = A_DMAC_CCR(0), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR1", .addr = A_DMAC_CNDTR(0),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR1", .addr = A_DMAC_CPAR(0) },
    { .name = "CM0AR1", .addr = A_DMAC_CM0AR(0) },
    { .name = "CBSR1", .addr = A_DMAC_CBSR(0),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    { .name = "CCR2", .addr = A_DMAC_CCR(1), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR2", .addr = A_DMAC_CNDTR(1),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR2", .addr = A_DMAC_CPAR(1) },
    { .name = "CM0AR2", .addr = A_DMAC_CM0AR(1) },
    { .name = "CBSR2", .addr = A_DMAC_CBSR(1),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    { .name = "CCR3", .addr = A_DMAC_CCR(2), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR3", .addr = A_DMAC_CNDTR(2),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR3", .addr = A_DMAC_CPAR(2) },
    { .name = "CM0AR3", .addr = A_DMAC_CM0AR(2) },
    { .name = "CBSR3", .addr = A_DMAC_CBSR(2),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    { .name = "CCR4", .addr = A_DMAC_CCR(3), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR4", .addr = A_DMAC_CNDTR(3),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR4", .addr = A_DMAC_CPAR(3) },
    { .name = "CM0AR4", .addr = A_DMAC_CM0AR(3) },
    { .name = "CBSR4", .addr = A_DMAC_CBSR(3),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    { .name = "CCR5", .addr = A_DMAC_CCR(4), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR5", .addr = A_DMAC_CNDTR(4),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR5", .addr = A_DMAC_CPAR(4) },
    { .name = "CM0AR5", .addr = A_DMAC_CM0AR(4) },
    { .name = "CBSR5", .addr = A_DMAC_CBSR(4),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    { .name = "CCR6", .addr = A_DMAC_CCR(5), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR6", .addr = A_DMAC_CNDTR(5),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR6", .addr = A_DMAC_CPAR(5) },
    { .name = "CM0AR6", .addr = A_DMAC_CM0AR(5) },
    { .name = "CBSR6", .addr = A_DMAC_CBSR(5),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    { .name = "CCR7", .addr = A_DMAC_CCR(6), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR7", .addr = A_DMAC_CNDTR(6),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR7", .addr = A_DMAC_CPAR(6) },
    { .name = "CM0AR7", .addr = A_DMAC_CM0AR(6) },
    { .name = "CBSR7", .addr = A_DMAC_CBSR(6),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    { .name = "CCR8", .addr = A_DMAC_CCR(7), .rsvd = SF32LB52X_DMAC_CCR_RSVD,
      .post_write = sf32lb52x_dmac_ccr_post_write },
    { .name = "CNDTR8", .addr = A_DMAC_CNDTR(7),
      .rsvd = SF32LB52X_DMAC_CNDTR_RSVD },
    { .name = "CPAR8", .addr = A_DMAC_CPAR(7) },
    { .name = "CM0AR8", .addr = A_DMAC_CM0AR(7) },
    { .name = "CBSR8", .addr = A_DMAC_CBSR(7),
      .rsvd = SF32LB52X_DMAC_CBSR_RSVD },
    {
        .name = "CSELR1",
        .addr = A_CSELR1,
        .rsvd = SF32LB52X_DMAC_CSELR_RSVD,
    }, {
        .name = "CSELR2",
        .addr = A_CSELR2,
        .rsvd = SF32LB52X_DMAC_CSELR_RSVD,
    }, {
        .name = "DBGSEL",
        .addr = A_DBGSEL,
    },
};

static const MemoryRegionOps sf32lb52x_dmac_regs_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void sf32lb52x_dmac_reset_enter(Object *obj, ResetType type)
{
    SF32LB52XDMACState *s = SF32LB52X_DMAC(obj);

    s->resetting = true;
}

static void sf32lb52x_dmac_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XDMACState *s = SF32LB52X_DMAC(obj);
    int i;

    qemu_bh_cancel(s->bh);
    s->pending = 0;

    for (i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }

    memset(s->en_prev, 0, sizeof(s->en_prev));
    memset(s->circ_cndtr, 0, sizeof(s->circ_cndtr));
    memset(s->circ_cpar, 0, sizeof(s->circ_cpar));
    memset(s->circ_cm0ar, 0, sizeof(s->circ_cm0ar));

    for (i = 0; i < SF32LB52X_DMAC_NUM_CHANNELS; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static void sf32lb52x_dmac_reset_exit(Object *obj, ResetType type)
{
    SF32LB52XDMACState *s = SF32LB52X_DMAC(obj);

    s->resetting = false;
}

static void sf32lb52x_dmac_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XDMACState *s = SF32LB52X_DMAC(dev);
    int i;

    s->reg_array = register_init_block32(dev, sf32lb52x_dmac_regs_info,
                                         ARRAY_SIZE(sf32lb52x_dmac_regs_info),
                                         s->regs_info, s->regs,
                                         &sf32lb52x_dmac_regs_ops, false,
                                         SF32LB52X_DMAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->reg_array->mem);

    for (i = 0; i < SF32LB52X_DMAC_NUM_CHANNELS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }

    s->bh = qemu_bh_new(sf32lb52x_dmac_transfer_bh, s);
}

static int sf32lb52x_dmac_post_load(void *opaque, int version_id)
{
    SF32LB52XDMACState *s = opaque;

    sf32lb52x_dmac_update_all_irqs(s);

    return 0;
}

static const VMStateDescription vmstate_sf32lb52x_dmac = {
    .name = TYPE_SF32LB52X_DMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sf32lb52x_dmac_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SF32LB52XDMACState, SF32LB52X_DMAC_R_MAX),
        VMSTATE_UINT8(pending, SF32LB52XDMACState),
        VMSTATE_UINT8_ARRAY(en_prev, SF32LB52XDMACState,
                            SF32LB52X_DMAC_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(circ_cndtr, SF32LB52XDMACState,
                             SF32LB52X_DMAC_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(circ_cpar, SF32LB52XDMACState,
                             SF32LB52X_DMAC_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(circ_cm0ar, SF32LB52XDMACState,
                             SF32LB52X_DMAC_NUM_CHANNELS),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_dmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = sf32lb52x_dmac_reset_enter;
    rc->phases.hold = sf32lb52x_dmac_reset_hold;
    rc->phases.exit = sf32lb52x_dmac_reset_exit;
    dc->realize = sf32lb52x_dmac_realize;
    dc->desc = "SiFli SF32LB52X DMA controller";
    dc->vmsd = &vmstate_sf32lb52x_dmac;
    dc->user_creatable = false;
}

static void sf32lb52x_dmac_finalize(Object *obj)
{
    SF32LB52XDMACState *s = SF32LB52X_DMAC(obj);

    if (s->bh) {
        qemu_bh_delete(s->bh);
        s->bh = NULL;
    }
}

static const TypeInfo sf32lb52x_dmac_types[] = {
    {
        .name = TYPE_SF32LB52X_DMAC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XDMACState),
        .instance_finalize = sf32lb52x_dmac_finalize,
        .class_init = sf32lb52x_dmac_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_dmac_types)
