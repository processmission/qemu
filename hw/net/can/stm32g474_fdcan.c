/*
 * STM32G474 flexible data-rate CAN subsystem
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/net/stm32g474_fdcan.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

REG32(CREL, 0x000)
    FIELD(CREL, DAY, 0, 8)
    FIELD(CREL, MON, 8, 8)
    FIELD(CREL, YEAR, 16, 4)
    FIELD(CREL, SUBSTEP, 20, 4)
    FIELD(CREL, STEP, 24, 4)
    FIELD(CREL, REL, 28, 4)
REG32(ENDN, 0x004)
    FIELD(ENDN, ETV, 0, 32)
REG32(DBTP, 0x00c)
    FIELD(DBTP, DSJW, 0, 4)
    FIELD(DBTP, DTSEG2, 4, 4)
    FIELD(DBTP, DTSEG1, 8, 5)
    FIELD(DBTP, DBRP, 16, 5)
    FIELD(DBTP, TDC, 23, 1)
REG32(TEST, 0x010)
    FIELD(TEST, LBCK, 4, 1)
    FIELD(TEST, TX, 5, 2)
    FIELD(TEST, RX, 7, 1)
REG32(RWD, 0x014)
    FIELD(RWD, WDC, 0, 8)
    FIELD(RWD, WDV, 8, 8)
REG32(CCCR, 0x018)
    FIELD(CCCR, INIT, 0, 1)
    FIELD(CCCR, CCE, 1, 1)
    FIELD(CCCR, ASM, 2, 1)
    FIELD(CCCR, CSA, 3, 1)
    FIELD(CCCR, CSR, 4, 1)
    FIELD(CCCR, MON, 5, 1)
    FIELD(CCCR, DAR, 6, 1)
    FIELD(CCCR, TEST, 7, 1)
    FIELD(CCCR, FDOE, 8, 1)
    FIELD(CCCR, BRSE, 9, 1)
    FIELD(CCCR, PXHD, 12, 1)
    FIELD(CCCR, EFBI, 13, 1)
    FIELD(CCCR, TXP, 14, 1)
    FIELD(CCCR, NISO, 15, 1)
REG32(NBTP, 0x01c)
    FIELD(NBTP, NTSEG2, 0, 7)
    FIELD(NBTP, NTSEG1, 8, 8)
    FIELD(NBTP, NBRP, 16, 9)
    FIELD(NBTP, NSJW, 25, 7)
REG32(TSCC, 0x020)
    FIELD(TSCC, TSS, 0, 2)
    FIELD(TSCC, TCP, 16, 4)
REG32(TSCV, 0x024)
    FIELD(TSCV, TSC, 0, 16)
REG32(TOCC, 0x028)
    FIELD(TOCC, ETOC, 0, 1)
    FIELD(TOCC, TOS, 1, 2)
    FIELD(TOCC, TOP, 16, 16)
REG32(TOCV, 0x02c)
    FIELD(TOCV, TOC, 0, 16)
REG32(ECR, 0x040)
    FIELD(ECR, TEC, 0, 8)
    FIELD(ECR, REC, 8, 7)
    FIELD(ECR, RP, 15, 1)
    FIELD(ECR, CEL, 16, 8)
REG32(PSR, 0x044)
    FIELD(PSR, LEC, 0, 3)
    FIELD(PSR, ACT, 3, 2)
    FIELD(PSR, EP, 5, 1)
    FIELD(PSR, EW, 6, 1)
    FIELD(PSR, BO, 7, 1)
    FIELD(PSR, DLEC, 8, 3)
    FIELD(PSR, RESI, 11, 1)
    FIELD(PSR, RBRS, 12, 1)
    FIELD(PSR, REDL, 13, 1)
    FIELD(PSR, PXE, 14, 1)
    FIELD(PSR, TDCV, 16, 7)
REG32(TDCR, 0x048)
    FIELD(TDCR, TDCF, 0, 7)
    FIELD(TDCR, TDCO, 8, 7)
REG32(IR, 0x050)
    FIELD(IR, RF0N, 0, 1)
    FIELD(IR, RF0F, 1, 1)
    FIELD(IR, RF0L, 2, 1)
    FIELD(IR, RF1N, 3, 1)
    FIELD(IR, RF1F, 4, 1)
    FIELD(IR, RF1L, 5, 1)
    FIELD(IR, HPM, 6, 1)
    FIELD(IR, TC, 7, 1)
    FIELD(IR, TCF, 8, 1)
    FIELD(IR, TFE, 9, 1)
    FIELD(IR, TEFN, 10, 1)
    FIELD(IR, TEFF, 11, 1)
    FIELD(IR, TEFL, 12, 1)
    FIELD(IR, TSW, 13, 1)
    FIELD(IR, MRAF, 14, 1)
    FIELD(IR, TOO, 15, 1)
    FIELD(IR, ELO, 16, 1)
    FIELD(IR, EP, 17, 1)
    FIELD(IR, EW, 18, 1)
    FIELD(IR, BO, 19, 1)
    FIELD(IR, WDI, 20, 1)
    FIELD(IR, PEA, 21, 1)
    FIELD(IR, PED, 22, 1)
    FIELD(IR, ARA, 23, 1)
REG32(IE, 0x054)
REG32(ILS, 0x058)
    FIELD(ILS, RXFIFO0, 0, 1)
    FIELD(ILS, RXFIFO1, 1, 1)
    FIELD(ILS, SMSG, 2, 1)
    FIELD(ILS, TFERR, 3, 1)
    FIELD(ILS, MISC, 4, 1)
    FIELD(ILS, BERR, 5, 1)
    FIELD(ILS, PERR, 6, 1)
REG32(ILE, 0x05c)
    FIELD(ILE, EINT0, 0, 1)
    FIELD(ILE, EINT1, 1, 1)
REG32(RXGFC, 0x080)
    FIELD(RXGFC, RRFE, 0, 1)
    FIELD(RXGFC, RRFS, 1, 1)
    FIELD(RXGFC, ANFE, 2, 2)
    FIELD(RXGFC, ANFS, 4, 2)
    FIELD(RXGFC, F1OM, 8, 1)
    FIELD(RXGFC, F0OM, 9, 1)
    FIELD(RXGFC, LSS, 16, 5)
    FIELD(RXGFC, LSE, 24, 4)
REG32(XIDAM, 0x084)
    FIELD(XIDAM, EIDM, 0, 29)
REG32(HPMS, 0x088)
    FIELD(HPMS, BIDX, 0, 3)
    FIELD(HPMS, MSI, 6, 2)
    FIELD(HPMS, FIDX, 8, 5)
    FIELD(HPMS, FLST, 15, 1)
REG32(RXF0S, 0x090)
    FIELD(RXF0S, F0FL, 0, 4)
    FIELD(RXF0S, F0GI, 8, 2)
    FIELD(RXF0S, F0PI, 16, 2)
    FIELD(RXF0S, F0F, 24, 1)
    FIELD(RXF0S, RF0L, 25, 1)
REG32(RXF0A, 0x094)
    FIELD(RXF0A, F0AI, 0, 3)
REG32(RXF1S, 0x098)
    FIELD(RXF1S, F1FL, 0, 4)
    FIELD(RXF1S, F1GI, 8, 2)
    FIELD(RXF1S, F1PI, 16, 2)
    FIELD(RXF1S, F1F, 24, 1)
    FIELD(RXF1S, RF1L, 25, 1)
REG32(RXF1A, 0x09c)
    FIELD(RXF1A, F1AI, 0, 3)
REG32(TXBC, 0x0c0)
    FIELD(TXBC, TFQM, 24, 1)
REG32(TXFQS, 0x0c4)
    FIELD(TXFQS, TFFL, 0, 3)
    FIELD(TXFQS, TFGI, 8, 2)
    FIELD(TXFQS, TFQPI, 16, 2)
    FIELD(TXFQS, TFQF, 21, 1)
REG32(TXBRP, 0x0c8)
    FIELD(TXBRP, TRP, 0, 3)
REG32(TXBAR, 0x0cc)
    FIELD(TXBAR, AR, 0, 3)
REG32(TXBCR, 0x0d0)
    FIELD(TXBCR, CR, 0, 3)
REG32(TXBTO, 0x0d4)
    FIELD(TXBTO, TO, 0, 3)
REG32(TXBCF, 0x0d8)
    FIELD(TXBCF, CF, 0, 3)
REG32(TXBTIE, 0x0dc)
    FIELD(TXBTIE, TIE, 0, 3)
REG32(TXBCIE, 0x0e0)
    FIELD(TXBCIE, CFIE, 0, 3)
REG32(TXEFS, 0x0e4)
    FIELD(TXEFS, EFFL, 0, 3)
    FIELD(TXEFS, EFGI, 8, 2)
    FIELD(TXEFS, EFPI, 16, 2)
    FIELD(TXEFS, EFF, 24, 1)
    FIELD(TXEFS, TEFL, 25, 1)
REG32(TXEFA, 0x0e8)
    FIELD(TXEFA, EFAI, 0, 2)
REG32(CKDIV, 0x100)
    FIELD(CKDIV, PDIV, 0, 4)

#define STM32G474_FDCAN_COMMON_NUM_DESCRIPTORS 36
#define STM32G474_FDCAN_NUM_IRQ_GROUPS 7
#define STM32G474_FDCAN_TX_BUFFER_MASK 0x00000007U
#define STM32G474_FDCAN_IR_MASK 0x00ffffffU
#define STM32G474_FDCAN_ILS_MASK 0x0000007fU

#define STM32G474_FDCAN_CCCR_MODE_MASK \
    (R_CCCR_ASM_MASK | R_CCCR_MON_MASK | R_CCCR_DAR_MASK | \
     R_CCCR_TEST_MASK | R_CCCR_FDOE_MASK | R_CCCR_BRSE_MASK | \
     R_CCCR_PXHD_MASK | R_CCCR_EFBI_MASK | R_CCCR_TXP_MASK | \
     R_CCCR_NISO_MASK)

#define STM32G474_FDCAN_RSVD(mask) (UINT32_MAX & ~(uint32_t)(mask))

static const MCanMsgRamLayout stm32g474_fdcan_mram_layout = {
    .std_filter = 0x000,
    .ext_filter = 0x070,
    .rx_fifo = { 0x0b0, 0x188 },
    .tx_event = 0x260,
    .tx_buffer = 0x278,
    .std_filters = 28,
    .ext_filters = 8,
    .rx_elements = { 3, 3 },
    .tx_events = 3,
    .tx_buffers = 3,
};

static const uint32_t stm32g474_fdcan_irq_group_masks[] = {
    0x000007,
    0x000038,
    0x0001c0,
    0x001e00,
    0x00e000,
    0x030000,
    0xfc0000,
};

static const uint32_t stm32g474_fdcan_engine_event_ir[] = {
    R_IR_RF0N_MASK,
    R_IR_RF0F_MASK,
    R_IR_RF0L_MASK,
    R_IR_RF1N_MASK,
    R_IR_RF1F_MASK,
    R_IR_RF1L_MASK,
    R_IR_HPM_MASK,
    R_IR_TEFN_MASK,
    R_IR_TEFF_MASK,
    R_IR_TEFL_MASK,
};

static bool stm32g474_fdcan_in_reset(const Stm32g474FdcanState *s)
{
    return s->resetting || s->peripheral_reset_asserted;
}

static void
stm32g474_fdcan_update_irq(Stm32g474FdcanChannel *channel)
{
    uint32_t pending =
        channel->regs[R_IR] & channel->regs[R_IE] &
        STM32G474_FDCAN_IR_MASK;
    uint32_t routed[STM32G474_FDCAN_NUM_IRQS] = { 0 };
    uint32_t ils = channel->regs[R_ILS];
    uint32_t ile = channel->regs[R_ILE];

    for (unsigned int group = 0;
         group < ARRAY_SIZE(stm32g474_fdcan_irq_group_masks); group++) {
        unsigned int line = (ils & BIT(group)) != 0;

        routed[line] |=
            pending & stm32g474_fdcan_irq_group_masks[group];
    }

    qemu_set_irq(channel->irq[0],
                 (ile & R_ILE_EINT0_MASK) && routed[0]);
    qemu_set_irq(channel->irq[1],
                 (ile & R_ILE_EINT1_MASK) && routed[1]);
}

static void
stm32g474_fdcan_sync_rx_fifo(Stm32g474FdcanChannel *channel,
                              unsigned int fifo)
{
    MCanFifoStatus status =
        m_can_rx_fifo_status(&channel->engine, fifo);
    uint32_t value = 0;

    if (fifo == 0) {
        value = FIELD_DP32(value, RXF0S, F0FL, status.fill);
        value = FIELD_DP32(value, RXF0S, F0GI, status.get_index);
        value = FIELD_DP32(value, RXF0S, F0PI, status.put_index);
        value = FIELD_DP32(value, RXF0S, F0F, status.full);
        value = FIELD_DP32(value, RXF0S, RF0L, status.lost);
        channel->regs[R_RXF0S] = value;
    } else {
        value = FIELD_DP32(value, RXF1S, F1FL, status.fill);
        value = FIELD_DP32(value, RXF1S, F1GI, status.get_index);
        value = FIELD_DP32(value, RXF1S, F1PI, status.put_index);
        value = FIELD_DP32(value, RXF1S, F1F, status.full);
        value = FIELD_DP32(value, RXF1S, RF1L, status.lost);
        channel->regs[R_RXF1S] = value;
    }
}

static void
stm32g474_fdcan_sync_tx_event_fifo(Stm32g474FdcanChannel *channel)
{
    MCanFifoStatus status =
        m_can_tx_event_status(&channel->engine);
    uint32_t value = 0;

    value = FIELD_DP32(value, TXEFS, EFFL, status.fill);
    value = FIELD_DP32(value, TXEFS, EFGI, status.get_index);
    value = FIELD_DP32(value, TXEFS, EFPI, status.put_index);
    value = FIELD_DP32(value, TXEFS, EFF, status.full);
    value = FIELD_DP32(value, TXEFS, TEFL, status.lost);
    channel->regs[R_TXEFS] = value;
}

static void
stm32g474_fdcan_sync_high_priority(Stm32g474FdcanChannel *channel)
{
    MCanHighPriorityState status =
        m_can_high_priority_status(&channel->engine);
    uint32_t value = 0;

    if (status.valid) {
        value = FIELD_DP32(value, HPMS, BIDX, status.buffer_index);
        value = FIELD_DP32(value, HPMS, MSI, status.storage);
        value = FIELD_DP32(value, HPMS, FIDX, status.filter_index);
        value = FIELD_DP32(value, HPMS, FLST, status.extended);
    }
    channel->regs[R_HPMS] = value;
}

static void
stm32g474_fdcan_sync_tx_queue(Stm32g474FdcanChannel *channel)
{
    uint32_t pending =
        channel->regs[R_TXBRP] & STM32G474_FDCAN_TX_BUFFER_MASK;
    uint32_t free_mask = ~pending & STM32G474_FDCAN_TX_BUFFER_MASK;
    unsigned int fill = ctpop32(pending);
    unsigned int get_index = pending ? ctz32(pending) : 0;
    unsigned int put_index = free_mask ? ctz32(free_mask) : 0;
    uint32_t value = 0;

    value = FIELD_DP32(value, TXFQS, TFFL,
                       stm32g474_fdcan_mram_layout.tx_buffers - fill);
    value = FIELD_DP32(value, TXFQS, TFGI, get_index);
    value = FIELD_DP32(value, TXFQS, TFQPI, put_index);
    value = FIELD_DP32(value, TXFQS, TFQF,
                       fill == stm32g474_fdcan_mram_layout.tx_buffers);
    channel->regs[R_TXFQS] = value;
}

static void
stm32g474_fdcan_sync_dynamic_status(Stm32g474FdcanChannel *channel)
{
    stm32g474_fdcan_sync_rx_fifo(channel, 0);
    stm32g474_fdcan_sync_rx_fifo(channel, 1);
    stm32g474_fdcan_sync_tx_event_fifo(channel);
    stm32g474_fdcan_sync_high_priority(channel);
    stm32g474_fdcan_sync_tx_queue(channel);
}

static MCanNonmatching stm32g474_fdcan_nonmatching(unsigned int field)
{
    switch (field) {
    case 0:
        return M_CAN_NONMATCHING_FIFO0;
    case 1:
        return M_CAN_NONMATCHING_FIFO1;
    default:
        return M_CAN_NONMATCHING_REJECT;
    }
}

static uint64_t
stm32g474_fdcan_effective_clock_hz(const Stm32g474FdcanState *s)
{
    uint64_t kernel_hz = clock_get_hz(s->kernel_clk);
    unsigned int ckdiv =
        FIELD_EX32(s->channel[0].regs[R_CKDIV], CKDIV, PDIV);
    unsigned int divisor = ckdiv ? 2 * ckdiv : 1;

    return kernel_hz / divisor;
}

static void stm32g474_fdcan_get_engine_config(void *opaque,
                                               MCanEngineConfig *config)
{
    Stm32g474FdcanChannel *channel = opaque;
    Stm32g474FdcanState *s = channel->parent;
    uint32_t cccr = channel->regs[R_CCCR];
    uint32_t rxgfc = channel->regs[R_RXGFC];
    uint64_t effective_hz = stm32g474_fdcan_effective_clock_hz(s);

    *config = (MCanEngineConfig) {
        .enabled = !stm32g474_fdcan_in_reset(s) &&
                   effective_hz != 0 &&
                   effective_hz <= clock_get_hz(s->pclk) &&
                   !(cccr & (R_CCCR_INIT_MASK | R_CCCR_CSR_MASK)),
        .fd_enabled = (cccr & R_CCCR_FDOE_MASK) != 0,
        .brs_enabled = (cccr & R_CCCR_BRSE_MASK) != 0,
        .reject_remote_standard =
            (rxgfc & R_RXGFC_RRFS_MASK) != 0,
        .reject_remote_extended =
            (rxgfc & R_RXGFC_RRFE_MASK) != 0,
        .rx_fifo_overwrite = {
            (rxgfc & R_RXGFC_F0OM_MASK) != 0,
            (rxgfc & R_RXGFC_F1OM_MASK) != 0,
        },
        .std_filters = FIELD_EX32(rxgfc, RXGFC, LSS),
        .ext_filters = FIELD_EX32(rxgfc, RXGFC, LSE),
        .nonmatching_standard = stm32g474_fdcan_nonmatching(
            FIELD_EX32(rxgfc, RXGFC, ANFS)),
        .nonmatching_extended = stm32g474_fdcan_nonmatching(
            FIELD_EX32(rxgfc, RXGFC, ANFE)),
        .extended_id_mask =
            FIELD_EX32(channel->regs[R_XIDAM], XIDAM, EIDM),
    };
}

static void stm32g474_fdcan_engine_event(void *opaque, uint32_t events)
{
    Stm32g474FdcanChannel *channel = opaque;
    Stm32g474FdcanState *s = channel->parent;
    uint32_t ir = 0;

    if (stm32g474_fdcan_in_reset(s)) {
        return;
    }

    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_fdcan_engine_event_ir); i++) {
        if (events & BIT(i)) {
            ir |= stm32g474_fdcan_engine_event_ir[i];
        }
    }

    stm32g474_fdcan_sync_dynamic_status(channel);
    channel->regs[R_IR] |= ir;
    stm32g474_fdcan_update_irq(channel);
}

static const MCanEngineOps stm32g474_fdcan_engine_ops = {
    .get_config = stm32g474_fdcan_get_engine_config,
    .event = stm32g474_fdcan_engine_event,
};

static uint32_t stm32g474_fdcan_register_value(RegisterInfo *reg)
{
    return *(uint32_t *)reg->data;
}

static uint64_t
stm32g474_fdcan_protected_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    uint32_t cccr = channel->regs[R_CCCR];

    if ((cccr & (R_CCCR_INIT_MASK | R_CCCR_CCE_MASK)) ==
        (R_CCCR_INIT_MASK | R_CCCR_CCE_MASK)) {
        return val;
    }
    return stm32g474_fdcan_register_value(reg);
}

static uint64_t
stm32g474_fdcan_test_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (channel->regs[R_CCCR] & R_CCCR_TEST_MASK) {
        return val;
    }
    return stm32g474_fdcan_register_value(reg);
}

static uint64_t
stm32g474_fdcan_cccr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    uint32_t old = channel->regs[R_CCCR];
    uint32_t value = val;
    uint32_t rising_modes;

    if (!(old & R_CCCR_INIT_MASK)) {
        value = (value & ~R_CCCR_CCE_MASK) |
                (old & R_CCCR_CCE_MASK);
    }
    if (!(value & R_CCCR_INIT_MASK)) {
        value &= ~R_CCCR_CCE_MASK;
    }

    rising_modes = value & ~old & STM32G474_FDCAN_CCCR_MODE_MASK;
    if ((old & (R_CCCR_INIT_MASK | R_CCCR_CCE_MASK)) !=
        (R_CCCR_INIT_MASK | R_CCCR_CCE_MASK)) {
        value &= ~rising_modes;
    }

    channel->cccr_old = old;
    channel->cccr_write_pending = true;
    return value;
}

static void
stm32g474_fdcan_sync_cccr_status(Stm32g474FdcanChannel *channel)
{
    if (channel->regs[R_CCCR] & R_CCCR_CSR_MASK) {
        channel->regs[R_CCCR] |= R_CCCR_CSA_MASK;
    } else {
        channel->regs[R_CCCR] &= ~R_CCCR_CSA_MASK;
    }
}

static void
stm32g474_fdcan_cccr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    Stm32g474FdcanState *s = channel->parent;
    uint32_t old;

    if (s->resetting || !channel->cccr_write_pending) {
        channel->cccr_write_pending = false;
        return;
    }

    old = channel->cccr_old;
    channel->cccr_write_pending = false;

    stm32g474_fdcan_sync_cccr_status(channel);

    if ((old & R_CCCR_TEST_MASK) &&
        !(channel->regs[R_CCCR] & R_CCCR_TEST_MASK)) {
        register_reset(&channel->regs_info[R_TEST]);
    }
}

static uint64_t
stm32g474_fdcan_tscv_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (FIELD_EX32(channel->regs[R_TSCC], TSCC, TSS) == 1) {
        return 0;
    }
    return stm32g474_fdcan_register_value(reg);
}

static uint64_t
stm32g474_fdcan_tocv_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (FIELD_EX32(channel->regs[R_TOCC], TOCC, TOS) == 0) {
        return FIELD_EX32(channel->regs[R_TOCC], TOCC, TOP);
    }
    return stm32g474_fdcan_register_value(reg);
}

static uint64_t
stm32g474_fdcan_psr_post_read(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    uint32_t value = channel->regs[R_PSR];

    value = FIELD_DP32(value, PSR, LEC, 7);
    value = FIELD_DP32(value, PSR, DLEC, 7);
    value &= ~(R_PSR_PXE_MASK | R_PSR_REDL_MASK |
               R_PSR_RBRS_MASK | R_PSR_RESI_MASK);
    channel->regs[R_PSR] = value;
    return val;
}

static void
stm32g474_fdcan_irq_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (!stm32g474_fdcan_in_reset(channel->parent)) {
        stm32g474_fdcan_update_irq(channel);
    }
}

static void
stm32g474_fdcan_ir_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    Stm32g474FdcanState *s = channel->parent;

    if (s->resetting || !s->engines_initialized) {
        return;
    }

    if (!(channel->regs[R_IR] & R_IR_RF0L_MASK)) {
        m_can_rx_fifo_clear_lost(&channel->engine, 0);
    }
    if (!(channel->regs[R_IR] & R_IR_RF1L_MASK)) {
        m_can_rx_fifo_clear_lost(&channel->engine, 1);
    }
    if (!(channel->regs[R_IR] & R_IR_TEFL_MASK)) {
        m_can_tx_event_clear_lost(&channel->engine);
    }
    stm32g474_fdcan_sync_dynamic_status(channel);
    stm32g474_fdcan_update_irq(channel);
}

static void
stm32g474_fdcan_rxf0a_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (!stm32g474_fdcan_in_reset(channel->parent) &&
        channel->parent->engines_initialized) {
        m_can_rx_fifo_ack(&channel->engine, 0,
                          FIELD_EX32(val, RXF0A, F0AI));
        stm32g474_fdcan_sync_rx_fifo(channel, 0);
    }
}

static void
stm32g474_fdcan_rxf1a_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (!stm32g474_fdcan_in_reset(channel->parent) &&
        channel->parent->engines_initialized) {
        m_can_rx_fifo_ack(&channel->engine, 1,
                          FIELD_EX32(val, RXF1A, F1AI));
        stm32g474_fdcan_sync_rx_fifo(channel, 1);
    }
}

static void
stm32g474_fdcan_txefa_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (!stm32g474_fdcan_in_reset(channel->parent) &&
        channel->parent->engines_initialized) {
        m_can_tx_event_ack(&channel->engine,
                           FIELD_EX32(val, TXEFA, EFAI));
        stm32g474_fdcan_sync_tx_event_fifo(channel);
    }
}

static uint64_t
stm32g474_fdcan_txbar_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (channel->regs[R_CCCR] & R_CCCR_CCE_MASK) {
        return stm32g474_fdcan_register_value(reg);
    }
    return stm32g474_fdcan_register_value(reg) | val;
}

static void
stm32g474_fdcan_txbar_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    uint32_t request = val & STM32G474_FDCAN_TX_BUFFER_MASK;

    channel->regs[R_TXBAR] = 0;
    if (stm32g474_fdcan_in_reset(channel->parent)) {
        return;
    }

    channel->regs[R_TXBRP] |= request;
    channel->regs[R_TXBTO] &= ~request;
    channel->regs[R_TXBCF] &= ~request;
    stm32g474_fdcan_sync_tx_queue(channel);
}

static uint64_t
stm32g474_fdcan_txbcr_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    uint32_t pending = channel->regs[R_TXBRP];

    if (channel->regs[R_CCCR] & R_CCCR_CCE_MASK) {
        return stm32g474_fdcan_register_value(reg);
    }
    return stm32g474_fdcan_register_value(reg) | (val & pending);
}

static void
stm32g474_fdcan_txbcr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;
    uint32_t cancelled =
        val & channel->regs[R_TXBRP] &
        STM32G474_FDCAN_TX_BUFFER_MASK;

    if (stm32g474_fdcan_in_reset(channel->parent)) {
        channel->regs[R_TXBCR] = 0;
        return;
    }

    channel->regs[R_TXBRP] &= ~cancelled;
    channel->regs[R_TXBCF] |= cancelled;
    channel->regs[R_TXBCR] &= channel->regs[R_TXBRP];
    if (cancelled & channel->regs[R_TXBCIE]) {
        channel->regs[R_IR] |= R_IR_TCF_MASK;
    }
    stm32g474_fdcan_sync_tx_queue(channel);
    stm32g474_fdcan_update_irq(channel);
}

static uint64_t
stm32g474_fdcan_ckdiv_pre_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474FdcanChannel *channel = reg->opaque;

    if (channel->regs[R_CCCR] & R_CCCR_CCE_MASK) {
        return val;
    }
    return stm32g474_fdcan_register_value(reg);
}

static const RegisterAccessInfo stm32g474_fdcan_regs_info[] = {
    {
        .name = "CREL",
        .addr = A_CREL,
        .reset = 0x32141218,
        .ro = UINT32_MAX,
    }, {
        .name = "ENDN",
        .addr = A_ENDN,
        .reset = 0x87654321,
        .ro = UINT32_MAX,
    }, {
        .name = "DBTP",
        .addr = A_DBTP,
        .reset = 0x00000a33,
        .rsvd = STM32G474_FDCAN_RSVD(0x009f1fff),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "TEST",
        .addr = A_TEST,
        .ro = R_TEST_RX_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(0x000000f0),
        .pre_write = stm32g474_fdcan_test_pre_write,
    }, {
        .name = "RWD",
        .addr = A_RWD,
        .ro = R_RWD_WDV_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(0x0000ffff),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "CCCR",
        .addr = A_CCCR,
        .reset = 0x00000001,
        .ro = R_CCCR_CSA_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(0x0000f3ff),
        .pre_write = stm32g474_fdcan_cccr_pre_write,
        .post_write = stm32g474_fdcan_cccr_post_write,
    }, {
        .name = "NBTP",
        .addr = A_NBTP,
        .reset = 0x06000a03,
        .rsvd = STM32G474_FDCAN_RSVD(0xffffff7f),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "TSCC",
        .addr = A_TSCC,
        .rsvd = STM32G474_FDCAN_RSVD(0x000f0003),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "TSCV",
        .addr = A_TSCV,
        .rsvd = STM32G474_FDCAN_RSVD(0x0000ffff),
        .pre_write = stm32g474_fdcan_tscv_pre_write,
    }, {
        .name = "TOCC",
        .addr = A_TOCC,
        .reset = 0xffff0000,
        .rsvd = STM32G474_FDCAN_RSVD(0xffff0007),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "TOCV",
        .addr = A_TOCV,
        .reset = 0x0000ffff,
        .rsvd = STM32G474_FDCAN_RSVD(0x0000ffff),
        .pre_write = stm32g474_fdcan_tocv_pre_write,
    }, {
        .name = "ECR",
        .addr = A_ECR,
        .ro = 0x00ffffff,
        .cor = R_ECR_CEL_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(0x00ffffff),
    }, {
        .name = "PSR",
        .addr = A_PSR,
        .reset = 0x00000707,
        .ro = 0x007f7fff,
        .rsvd = STM32G474_FDCAN_RSVD(0x007f7fff),
        .post_read = stm32g474_fdcan_psr_post_read,
    }, {
        .name = "TDCR",
        .addr = A_TDCR,
        .rsvd = STM32G474_FDCAN_RSVD(0x00007f7f),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "IR",
        .addr = A_IR,
        .w1c = STM32G474_FDCAN_IR_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(STM32G474_FDCAN_IR_MASK),
        .post_write = stm32g474_fdcan_ir_post_write,
    }, {
        .name = "IE",
        .addr = A_IE,
        .rsvd = STM32G474_FDCAN_RSVD(STM32G474_FDCAN_IR_MASK),
        .post_write = stm32g474_fdcan_irq_post_write,
    }, {
        .name = "ILS",
        .addr = A_ILS,
        .rsvd = STM32G474_FDCAN_RSVD(STM32G474_FDCAN_ILS_MASK),
        .post_write = stm32g474_fdcan_irq_post_write,
    }, {
        .name = "ILE",
        .addr = A_ILE,
        .rsvd = STM32G474_FDCAN_RSVD(0x00000003),
        .post_write = stm32g474_fdcan_irq_post_write,
    }, {
        .name = "RXGFC",
        .addr = A_RXGFC,
        .rsvd = STM32G474_FDCAN_RSVD(0x0f1f033f),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "XIDAM",
        .addr = A_XIDAM,
        .reset = 0x1fffffff,
        .rsvd = STM32G474_FDCAN_RSVD(0x1fffffff),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "HPMS",
        .addr = A_HPMS,
        .ro = 0x00009fc7,
        .rsvd = STM32G474_FDCAN_RSVD(0x00009fc7),
    }, {
        .name = "RXF0S",
        .addr = A_RXF0S,
        .ro = 0x0303030f,
        .rsvd = STM32G474_FDCAN_RSVD(0x0303030f),
    }, {
        .name = "RXF0A",
        .addr = A_RXF0A,
        .rsvd = STM32G474_FDCAN_RSVD(0x00000007),
        .post_write = stm32g474_fdcan_rxf0a_post_write,
    }, {
        .name = "RXF1S",
        .addr = A_RXF1S,
        .ro = 0x0303030f,
        .rsvd = STM32G474_FDCAN_RSVD(0x0303030f),
    }, {
        .name = "RXF1A",
        .addr = A_RXF1A,
        .rsvd = STM32G474_FDCAN_RSVD(0x00000007),
        .post_write = stm32g474_fdcan_rxf1a_post_write,
    }, {
        .name = "TXBC",
        .addr = A_TXBC,
        .rsvd = STM32G474_FDCAN_RSVD(0x01000000),
        .pre_write = stm32g474_fdcan_protected_pre_write,
    }, {
        .name = "TXFQS",
        .addr = A_TXFQS,
        .reset = 0x00000003,
        .ro = 0x00230307,
        .rsvd = STM32G474_FDCAN_RSVD(0x00230307),
    }, {
        .name = "TXBRP",
        .addr = A_TXBRP,
        .ro = STM32G474_FDCAN_TX_BUFFER_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(
            STM32G474_FDCAN_TX_BUFFER_MASK),
    }, {
        .name = "TXBAR",
        .addr = A_TXBAR,
        .rsvd = STM32G474_FDCAN_RSVD(
            STM32G474_FDCAN_TX_BUFFER_MASK),
        .pre_write = stm32g474_fdcan_txbar_pre_write,
        .post_write = stm32g474_fdcan_txbar_post_write,
    }, {
        .name = "TXBCR",
        .addr = A_TXBCR,
        .rsvd = STM32G474_FDCAN_RSVD(
            STM32G474_FDCAN_TX_BUFFER_MASK),
        .pre_write = stm32g474_fdcan_txbcr_pre_write,
        .post_write = stm32g474_fdcan_txbcr_post_write,
    }, {
        .name = "TXBTO",
        .addr = A_TXBTO,
        .ro = STM32G474_FDCAN_TX_BUFFER_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(
            STM32G474_FDCAN_TX_BUFFER_MASK),
    }, {
        .name = "TXBCF",
        .addr = A_TXBCF,
        .ro = STM32G474_FDCAN_TX_BUFFER_MASK,
        .rsvd = STM32G474_FDCAN_RSVD(
            STM32G474_FDCAN_TX_BUFFER_MASK),
    }, {
        .name = "TXBTIE",
        .addr = A_TXBTIE,
        .rsvd = STM32G474_FDCAN_RSVD(
            STM32G474_FDCAN_TX_BUFFER_MASK),
    }, {
        .name = "TXBCIE",
        .addr = A_TXBCIE,
        .rsvd = STM32G474_FDCAN_RSVD(
            STM32G474_FDCAN_TX_BUFFER_MASK),
    }, {
        .name = "TXEFS",
        .addr = A_TXEFS,
        .ro = 0x03030307,
        .rsvd = STM32G474_FDCAN_RSVD(0x03030307),
    }, {
        .name = "TXEFA",
        .addr = A_TXEFA,
        .rsvd = STM32G474_FDCAN_RSVD(0x00000003),
        .post_write = stm32g474_fdcan_txefa_post_write,
    }, {
        .name = "CKDIV",
        .addr = A_CKDIV,
        .rsvd = STM32G474_FDCAN_RSVD(0x0000000f),
        .pre_write = stm32g474_fdcan_ckdiv_pre_write,
    },
};

G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_fdcan_regs_info) ==
                STM32G474_FDCAN_COMMON_NUM_DESCRIPTORS + 1);
G_STATIC_ASSERT(STM32G474_FDCAN_NUM_REGS == R_CKDIV + 1);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_fdcan_irq_group_masks) ==
                STM32G474_FDCAN_NUM_IRQ_GROUPS);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_fdcan_engine_event_ir) == 10);

static void stm32g474_fdcan_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    Stm32g474FdcanState *s = STM32G474_FDCAN(
        register_array_get_owner(reg_array));

    if (!stm32g474_fdcan_in_reset(s)) {
        register_write_memory(opaque, addr, value, size);
    }
}

static const MemoryRegionOps stm32g474_fdcan_ops = {
    .read = register_read_memory,
    .write = stm32g474_fdcan_write,
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

static void
stm32g474_fdcan_reset_registers(Stm32g474FdcanState *s)
{
    for (unsigned int channel_index = 0;
         channel_index < STM32G474_FDCAN_NUM_CHANNELS;
         channel_index++) {
        Stm32g474FdcanChannel *channel =
            &s->channel[channel_index];

        for (int i = 0; i < channel->reg_array->num_elements; i++) {
            register_reset(channel->reg_array->r[i]);
        }
        if (s->engines_initialized) {
            m_can_engine_reset(&channel->engine);
        }
        channel->cccr_old = 0;
        channel->cccr_write_pending = false;
        qemu_set_irq(channel->irq[0], 0);
        qemu_set_irq(channel->irq[1], 0);
    }
}

static void
stm32g474_fdcan_reset_input(void *opaque, int n, int level)
{
    Stm32g474FdcanState *s = STM32G474_FDCAN(opaque);
    bool asserted = level != 0;
    bool was_resetting;

    if (asserted == s->peripheral_reset_asserted) {
        return;
    }

    s->peripheral_reset_asserted = asserted;
    if (!asserted) {
        return;
    }

    was_resetting = s->resetting;
    s->resetting = true;
    stm32g474_fdcan_reset_registers(s);
    s->resetting = was_resetting;
}

static void stm32g474_fdcan_reset_enter(Object *obj, ResetType type)
{
    Stm32g474FdcanState *s = STM32G474_FDCAN(obj);

    s->resetting = true;
}

static void stm32g474_fdcan_reset_hold(Object *obj, ResetType type)
{
    Stm32g474FdcanState *s = STM32G474_FDCAN(obj);

    stm32g474_fdcan_reset_registers(s);
    s->peripheral_reset_asserted = false;
}

static void stm32g474_fdcan_reset_exit(Object *obj, ResetType type)
{
    Stm32g474FdcanState *s = STM32G474_FDCAN(obj);

    s->resetting = false;
    if (!s->engines_initialized) {
        return;
    }

    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        stm32g474_fdcan_sync_dynamic_status(&s->channel[i]);
        stm32g474_fdcan_update_irq(&s->channel[i]);
    }
}

static bool
stm32g474_fdcan_fifo_state_valid(const MCanFifoState *state,
                                  unsigned int capacity)
{
    return state->fill <= capacity &&
           state->get_index < capacity &&
           state->put_index < capacity;
}

static bool
stm32g474_fdcan_channel_state_valid(Stm32g474FdcanChannel *channel)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(channel->regs); i++) {
        RegisterInfo *reg = &channel->regs_info[i];

        if (!reg->access && channel->regs[i]) {
            return false;
        }
        if (reg->access && (channel->regs[i] & reg->access->rsvd)) {
            return false;
        }
    }

    if (!(channel->regs[R_CCCR] & R_CCCR_INIT_MASK) &&
        (channel->regs[R_CCCR] & R_CCCR_CCE_MASK)) {
        return false;
    }
    if (!stm32g474_fdcan_fifo_state_valid(
            &channel->engine.rx_fifo[0],
            stm32g474_fdcan_mram_layout.rx_elements[0]) ||
        !stm32g474_fdcan_fifo_state_valid(
            &channel->engine.rx_fifo[1],
            stm32g474_fdcan_mram_layout.rx_elements[1]) ||
        !stm32g474_fdcan_fifo_state_valid(
            &channel->engine.tx_event_fifo,
            stm32g474_fdcan_mram_layout.tx_events)) {
        return false;
    }
    if (channel->engine.high_priority.storage >
        M_CAN_HIGH_PRIORITY_FIFO1) {
        return false;
    }
    if ((channel->engine.high_priority.storage ==
         M_CAN_HIGH_PRIORITY_FIFO0 ||
         channel->engine.high_priority.storage ==
         M_CAN_HIGH_PRIORITY_FIFO1) &&
        channel->engine.high_priority.buffer_index >=
        stm32g474_fdcan_mram_layout.rx_elements[
            channel->engine.high_priority.storage -
            M_CAN_HIGH_PRIORITY_FIFO0]) {
        return false;
    }
    if ((channel->engine.high_priority.storage ==
         M_CAN_HIGH_PRIORITY_NO_FIFO ||
         channel->engine.high_priority.storage ==
         M_CAN_HIGH_PRIORITY_FIFO_LOST) &&
        channel->engine.high_priority.buffer_index != 0) {
        return false;
    }
    if (channel->engine.high_priority.valid &&
        channel->engine.high_priority.filter_index >=
        (channel->engine.high_priority.extended ?
         stm32g474_fdcan_mram_layout.ext_filters :
         stm32g474_fdcan_mram_layout.std_filters)) {
        return false;
    }
    return true;
}

static int stm32g474_fdcan_post_load(void *opaque, int version_id)
{
    Stm32g474FdcanState *s = STM32G474_FDCAN(opaque);

    s->resetting = true;
    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        Stm32g474FdcanChannel *channel = &s->channel[i];

        channel->cccr_old = 0;
        channel->cccr_write_pending = false;
        if (!stm32g474_fdcan_channel_state_valid(channel)) {
            s->resetting = false;
            return -EINVAL;
        }
    }

    if (s->peripheral_reset_asserted) {
        stm32g474_fdcan_reset_registers(s);
        s->resetting = false;
        return 0;
    }

    s->resetting = false;
    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        stm32g474_fdcan_sync_cccr_status(&s->channel[i]);
        stm32g474_fdcan_sync_dynamic_status(&s->channel[i]);
        stm32g474_fdcan_update_irq(&s->channel[i]);
    }
    return 0;
}

static const VMStateDescription vmstate_stm32g474_fdcan_channel = {
    .name = "stm32g474-fdcan-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474FdcanChannel,
                             STM32G474_FDCAN_NUM_REGS),
        VMSTATE_STRUCT(engine, Stm32g474FdcanChannel, 0,
                       vmstate_m_can_engine, MCanEngine),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_stm32g474_fdcan = {
    .name = TYPE_STM32G474_FDCAN,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stm32g474_fdcan_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channel, Stm32g474FdcanState,
                             STM32G474_FDCAN_NUM_CHANNELS, 0,
                             vmstate_stm32g474_fdcan_channel,
                             Stm32g474FdcanChannel),
        VMSTATE_BOOL(peripheral_reset_asserted,
                     Stm32g474FdcanState),
        VMSTATE_CLOCK(kernel_clk, Stm32g474FdcanState),
        VMSTATE_CLOCK(pclk, Stm32g474FdcanState),
        VMSTATE_END_OF_LIST()
    },
};

static void
stm32g474_fdcan_realize(DeviceState *dev, Error **errp)
{
    Stm32g474FdcanState *s = STM32G474_FDCAN(dev);

    if (!clock_has_source(s->kernel_clk)) {
        error_setg(errp, TYPE_STM32G474_FDCAN
                   ": kernel-clk clock must be connected");
        return;
    }
    if (!clock_has_source(s->pclk)) {
        error_setg(errp, TYPE_STM32G474_FDCAN
                   ": pclk clock must be connected");
        return;
    }
    if (!memory_region_init_ram(&s->message_ram, OBJECT(dev),
                                TYPE_STM32G474_FDCAN ".message-ram",
                                STM32G474_FDCAN_MRAM_SIZE, errp)) {
        return;
    }

    s->message_ram_ptr = memory_region_get_ram_ptr(&s->message_ram);
    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        uint8_t *slice =
            s->message_ram_ptr +
            i * STM32G474_FDCAN_CHANNEL_MRAM_SIZE;

        if (!m_can_engine_init(
                &s->channel[i].engine, slice,
                STM32G474_FDCAN_CHANNEL_MRAM_SIZE,
                &stm32g474_fdcan_mram_layout,
                &stm32g474_fdcan_engine_ops, &s->channel[i])) {
            error_setg(errp, TYPE_STM32G474_FDCAN
                       ": invalid message RAM layout for channel %u",
                       i + 1);
            return;
        }
    }
    s->engines_initialized = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->message_ram);
}

static const Property stm32g474_fdcan_properties[] = {
    DEFINE_PROP_LINK("canbus0", Stm32g474FdcanState, canbus[0],
                     TYPE_CAN_BUS, CanBusState *),
    DEFINE_PROP_LINK("canbus1", Stm32g474FdcanState, canbus[1],
                     TYPE_CAN_BUS, CanBusState *),
    DEFINE_PROP_LINK("canbus2", Stm32g474FdcanState, canbus[2],
                     TYPE_CAN_BUS, CanBusState *),
};

static void stm32g474_fdcan_init(Object *obj)
{
    Stm32g474FdcanState *s = STM32G474_FDCAN(obj);
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        Stm32g474FdcanChannel *channel = &s->channel[i];
        unsigned int num_descriptors =
            STM32G474_FDCAN_COMMON_NUM_DESCRIPTORS + (i == 0);

        channel->parent = s;
        channel->index = i;
        channel->reg_array = register_init_block32(
            dev, stm32g474_fdcan_regs_info, num_descriptors,
            channel->regs_info, channel->regs,
            &stm32g474_fdcan_ops, false,
            STM32G474_FDCAN_MMIO_SIZE);
        for (int reg = 0;
             reg < channel->reg_array->num_elements; reg++) {
            channel->reg_array->r[reg]->opaque = channel;
        }
        sysbus_init_mmio(sbd, &channel->reg_array->mem);
        for (unsigned int irq = 0;
             irq < STM32G474_FDCAN_NUM_IRQS; irq++) {
            sysbus_init_irq(sbd, &channel->irq[irq]);
        }
    }

    s->kernel_clk = qdev_init_clock_in(dev, "kernel-clk",
                                       NULL, NULL, 0);
    s->pclk = qdev_init_clock_in(dev, "pclk", NULL, NULL, 0);
    qdev_init_gpio_in_named(dev, stm32g474_fdcan_reset_input,
                            "reset", 1);
}

static void
stm32g474_fdcan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_fdcan_realize;
    dc->vmsd = &vmstate_stm32g474_fdcan;
    dc->user_creatable = false;
    device_class_set_props(dc, stm32g474_fdcan_properties);
    rc->phases.enter = stm32g474_fdcan_reset_enter;
    rc->phases.hold = stm32g474_fdcan_reset_hold;
    rc->phases.exit = stm32g474_fdcan_reset_exit;
}

static const TypeInfo stm32g474_fdcan_info = {
    .name = TYPE_STM32G474_FDCAN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32g474FdcanState),
    .instance_init = stm32g474_fdcan_init,
    .class_init = stm32g474_fdcan_class_init,
};

static void stm32g474_fdcan_register_types(void)
{
    type_register_static(&stm32g474_fdcan_info);
}

type_init(stm32g474_fdcan_register_types)
