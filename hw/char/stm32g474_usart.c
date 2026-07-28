/*
 * STM32G474 USART and UART
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "hw/char/stm32g474_usart.h"
#include "hw/core/clock.h"
#include "hw/core/registerfields.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/runstate.h"

#define STM32G474_USART_NUM_REGS 12

typedef struct Stm32g474UsartVariant Stm32g474UsartVariant;

struct Stm32g474UsartState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_USART_NUM_REGS];
    uint32_t regs[STM32G474_USART_NUM_REGS];

    Clock *clk;
    CharFrontend chr;
    qemu_irq irq;
    guint watch_tag;
    VMChangeStateEntry *resume_entry;

    bool tx_pending;
    bool tdr_write_accepted;
    bool resetting;
    bool peripheral_reset_asserted;
    bool handlers_installed;
    bool host_io_blocked;
};

struct Stm32g474UsartClass {
    SysBusDeviceClass parent_class;

    const Stm32g474UsartVariant *variant;
};

REG32(CR1, 0x00)
    FIELD(CR1, RXFFIE, 31, 1)
    FIELD(CR1, TXFEIE, 30, 1)
    FIELD(CR1, FIFOEN, 29, 1)
    FIELD(CR1, M1, 28, 1)
    FIELD(CR1, EOBIE, 27, 1)
    FIELD(CR1, RTOIE, 26, 1)
    FIELD(CR1, DEAT, 21, 5)
    FIELD(CR1, DEDT, 16, 5)
    FIELD(CR1, OVER8, 15, 1)
    FIELD(CR1, CMIE, 14, 1)
    FIELD(CR1, MME, 13, 1)
    FIELD(CR1, M0, 12, 1)
    FIELD(CR1, WAKE, 11, 1)
    FIELD(CR1, PCE, 10, 1)
    FIELD(CR1, PS, 9, 1)
    FIELD(CR1, PEIE, 8, 1)
    FIELD(CR1, TXEIE, 7, 1)
    FIELD(CR1, TCIE, 6, 1)
    FIELD(CR1, RXNEIE, 5, 1)
    FIELD(CR1, IDLEIE, 4, 1)
    FIELD(CR1, TE, 3, 1)
    FIELD(CR1, RE, 2, 1)
    FIELD(CR1, UESM, 1, 1)
    FIELD(CR1, UE, 0, 1)
REG32(CR2, 0x04)
    FIELD(CR2, ADD, 24, 8)
    FIELD(CR2, RTOEN, 23, 1)
    FIELD(CR2, ABRMOD, 21, 2)
    FIELD(CR2, ABREN, 20, 1)
    FIELD(CR2, MSBFIRST, 19, 1)
    FIELD(CR2, DATAINV, 18, 1)
    FIELD(CR2, TXINV, 17, 1)
    FIELD(CR2, RXINV, 16, 1)
    FIELD(CR2, SWAP, 15, 1)
    FIELD(CR2, LINEN, 14, 1)
    FIELD(CR2, STOP, 12, 2)
    FIELD(CR2, CLKEN, 11, 1)
    FIELD(CR2, CPOL, 10, 1)
    FIELD(CR2, CPHA, 9, 1)
    FIELD(CR2, LBCL, 8, 1)
    FIELD(CR2, LBDIE, 6, 1)
    FIELD(CR2, LBDL, 5, 1)
    FIELD(CR2, ADDM7, 4, 1)
    FIELD(CR2, DIS_NSS, 3, 1)
    FIELD(CR2, SLVEN, 0, 1)
REG32(CR3, 0x08)
    FIELD(CR3, TXFTCFG, 29, 3)
    FIELD(CR3, RXFTIE, 28, 1)
    FIELD(CR3, RXFTCFG, 25, 3)
    FIELD(CR3, TCBGTIE, 24, 1)
    FIELD(CR3, TXFTIE, 23, 1)
    FIELD(CR3, WUFIE, 22, 1)
    FIELD(CR3, WUS, 20, 2)
    FIELD(CR3, SCARCNT, 17, 3)
    FIELD(CR3, DEP, 15, 1)
    FIELD(CR3, DEM, 14, 1)
    FIELD(CR3, DDRE, 13, 1)
    FIELD(CR3, OVRDIS, 12, 1)
    FIELD(CR3, ONEBIT, 11, 1)
    FIELD(CR3, CTSIE, 10, 1)
    FIELD(CR3, CTSE, 9, 1)
    FIELD(CR3, RTSE, 8, 1)
    FIELD(CR3, DMAT, 7, 1)
    FIELD(CR3, DMAR, 6, 1)
    FIELD(CR3, SCEN, 5, 1)
    FIELD(CR3, NACK, 4, 1)
    FIELD(CR3, HDSEL, 3, 1)
    FIELD(CR3, IRLP, 2, 1)
    FIELD(CR3, IREN, 1, 1)
    FIELD(CR3, EIE, 0, 1)
REG32(BRR, 0x0c)
    FIELD(BRR, BRR, 0, 16)
REG32(GTPR, 0x10)
    FIELD(GTPR, GT, 8, 8)
    FIELD(GTPR, PSC, 0, 8)
REG32(RTOR, 0x14)
    FIELD(RTOR, BLEN, 24, 8)
    FIELD(RTOR, RTO, 0, 24)
REG32(RQR, 0x18)
    FIELD(RQR, TXFRQ, 4, 1)
    FIELD(RQR, RXFRQ, 3, 1)
    FIELD(RQR, MMRQ, 2, 1)
    FIELD(RQR, SBKRQ, 1, 1)
    FIELD(RQR, ABRRQ, 0, 1)
REG32(ISR, 0x1c)
    FIELD(ISR, TXFT, 27, 1)
    FIELD(ISR, RXFT, 26, 1)
    FIELD(ISR, TCBGT, 25, 1)
    FIELD(ISR, RXFF, 24, 1)
    FIELD(ISR, TXFE, 23, 1)
    FIELD(ISR, REACK, 22, 1)
    FIELD(ISR, TEACK, 21, 1)
    FIELD(ISR, WUF, 20, 1)
    FIELD(ISR, RWU, 19, 1)
    FIELD(ISR, SBKF, 18, 1)
    FIELD(ISR, CMF, 17, 1)
    FIELD(ISR, BUSY, 16, 1)
    FIELD(ISR, ABRF, 15, 1)
    FIELD(ISR, ABRE, 14, 1)
    FIELD(ISR, UDR, 13, 1)
    FIELD(ISR, EOBF, 12, 1)
    FIELD(ISR, RTOF, 11, 1)
    FIELD(ISR, CTS, 10, 1)
    FIELD(ISR, CTSIF, 9, 1)
    FIELD(ISR, LBDF, 8, 1)
    FIELD(ISR, TXE, 7, 1)
    FIELD(ISR, TC, 6, 1)
    FIELD(ISR, RXNE, 5, 1)
    FIELD(ISR, IDLE, 4, 1)
    FIELD(ISR, ORE, 3, 1)
    FIELD(ISR, NE, 2, 1)
    FIELD(ISR, FE, 1, 1)
    FIELD(ISR, PE, 0, 1)
REG32(ICR, 0x20)
    FIELD(ICR, WUCF, 20, 1)
    FIELD(ICR, CMCF, 17, 1)
    FIELD(ICR, UDRCF, 13, 1)
    FIELD(ICR, EOBCF, 12, 1)
    FIELD(ICR, RTOCF, 11, 1)
    FIELD(ICR, CTSCF, 9, 1)
    FIELD(ICR, LBDCF, 8, 1)
    FIELD(ICR, TCBGTCF, 7, 1)
    FIELD(ICR, TCCF, 6, 1)
    FIELD(ICR, TXFECF, 5, 1)
    FIELD(ICR, IDLECF, 4, 1)
    FIELD(ICR, ORECF, 3, 1)
    FIELD(ICR, NECF, 2, 1)
    FIELD(ICR, FECF, 1, 1)
    FIELD(ICR, PECF, 0, 1)
REG32(RDR, 0x24)
    FIELD(RDR, RDR, 0, 9)
REG32(TDR, 0x28)
    FIELD(TDR, TDR, 0, 9)
REG32(PRESC, 0x2c)
    FIELD(PRESC, PRESCALER, 0, 4)

#define STM32G474_USART_MMIO_SIZE 0x400
#define STM32G474_USART_ISR_RESET (R_ISR_TXE_MASK | R_ISR_TC_MASK)
#define STM32G474_USART_U_ISR_VALID_MASK 0x027fffff
#define STM32G474_USART_A_ISR_VALID_MASK 0x007fcfff
#define STM32G474_USART_CR1_FIFO_MASK \
    (R_CR1_RXFFIE_MASK | R_CR1_TXFEIE_MASK | R_CR1_FIFOEN_MASK)
#define STM32G474_USART_CR3_FIFO_MASK \
    (R_CR3_TXFTCFG_MASK | R_CR3_RXFTIE_MASK | R_CR3_RXFTCFG_MASK | \
     R_CR3_TXFTIE_MASK)

struct Stm32g474UsartVariant {
    const char *name;
    const RegisterAccessInfo *regs_info;
    size_t num_regs;
    uint32_t isr_valid_mask;
    bool is_usart;
};

#define STM32G474_USART_CR1_LOCKED_MASK \
    (R_CR1_FIFOEN_MASK | R_CR1_M1_MASK | R_CR1_DEAT_MASK | \
     R_CR1_DEDT_MASK | R_CR1_OVER8_MASK | R_CR1_M0_MASK | \
     R_CR1_WAKE_MASK | R_CR1_PCE_MASK | R_CR1_PS_MASK)
#define STM32G474_USART_CR2_UE_LOCKED_MASK \
    (R_CR2_MSBFIRST_MASK | R_CR2_DATAINV_MASK | R_CR2_TXINV_MASK | \
     R_CR2_RXINV_MASK | R_CR2_SWAP_MASK | R_CR2_LINEN_MASK | \
     R_CR2_STOP_MASK | R_CR2_CLKEN_MASK | R_CR2_CPOL_MASK | \
     R_CR2_CPHA_MASK | R_CR2_LBCL_MASK | R_CR2_LBDL_MASK | \
     R_CR2_ADDM7_MASK)
#define STM32G474_USART_CR2_TE_LOCKED_MASK \
    (R_CR2_CPOL_MASK | R_CR2_CPHA_MASK | R_CR2_LBCL_MASK)
#define STM32G474_USART_CR3_UE_LOCKED_MASK \
    (R_CR3_WUS_MASK | R_CR3_DEP_MASK | R_CR3_DEM_MASK | \
     R_CR3_DDRE_MASK | R_CR3_OVRDIS_MASK | R_CR3_ONEBIT_MASK | \
     R_CR3_CTSE_MASK | R_CR3_RTSE_MASK | R_CR3_SCEN_MASK | \
     R_CR3_NACK_MASK | R_CR3_HDSEL_MASK | R_CR3_IRLP_MASK | \
     R_CR3_IREN_MASK)
#define STM32G474_USART_ISR_FIFO_MASK \
    (R_ISR_TXFT_MASK | R_ISR_RXFT_MASK | R_ISR_RXFF_MASK | \
     R_ISR_TXFE_MASK)
#define STM32G474_USART_COMMON_CR1_UNIMP_MASK \
    (STM32G474_USART_CR1_FIFO_MASK | R_CR1_DEAT_MASK | \
     R_CR1_DEDT_MASK | R_CR1_RTOIE_MASK | R_CR1_CMIE_MASK | \
     R_CR1_MME_MASK | R_CR1_WAKE_MASK | R_CR1_PEIE_MASK | \
     R_CR1_IDLEIE_MASK | R_CR1_UESM_MASK)
#define STM32G474_USART_COMMON_ISR_MODELED_MASK \
    (R_ISR_REACK_MASK | R_ISR_TEACK_MASK | R_ISR_WUF_MASK | \
     R_ISR_CMF_MASK | R_ISR_RTOF_MASK | R_ISR_CTSIF_MASK | \
     R_ISR_LBDF_MASK | R_ISR_TXE_MASK | R_ISR_TC_MASK | \
     R_ISR_RXNE_MASK | R_ISR_IDLE_MASK | R_ISR_ORE_MASK | \
     R_ISR_NE_MASK | R_ISR_FE_MASK | R_ISR_PE_MASK)
#define STM32G474_USART_U_ISR_MODELED_MASK \
    (STM32G474_USART_COMMON_ISR_MODELED_MASK | R_ISR_TCBGT_MASK | \
     R_ISR_UDR_MASK | R_ISR_EOBF_MASK)

static const uint16_t stm32g474_usart_prescalers[] = {
    1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256,
};

static const Stm32g474UsartVariant *
stm32g474_usart_get_variant(Stm32g474UsartState *s)
{
    return STM32G474_USART_BASE_GET_CLASS(s)->variant;
}

static bool stm32g474_usart_in_reset(Stm32g474UsartState *s)
{
    return s->resetting || s->peripheral_reset_asserted;
}

static void stm32g474_usart_cancel_tx_watch(Stm32g474UsartState *s)
{
    g_clear_handle_id(&s->watch_tag, g_source_remove);
}

static void stm32g474_usart_cancel_resume(Stm32g474UsartState *s)
{
    if (s->resume_entry) {
        qemu_del_vm_change_state_handler(s->resume_entry);
        s->resume_entry = NULL;
    }
}

static void stm32g474_usart_update_irq(Stm32g474UsartState *s)
{
    const Stm32g474UsartVariant *variant =
        stm32g474_usart_get_variant(s);
    uint32_t cr1 = s->regs[R_CR1];
    uint32_t cr2 = s->regs[R_CR2];
    uint32_t cr3 = s->regs[R_CR3];
    uint32_t isr = s->regs[R_ISR];
    bool level;

    if (s->host_io_blocked) {
        return;
    }

    if (stm32g474_usart_in_reset(s)) {
        qemu_set_irq(s->irq, 0);
        return;
    }

    level = ((isr & R_ISR_PE_MASK) && (cr1 & R_CR1_PEIE_MASK)) ||
            ((isr & R_ISR_IDLE_MASK) && (cr1 & R_CR1_IDLEIE_MASK)) ||
            ((isr & R_ISR_RXNE_MASK) && (cr1 & R_CR1_RXNEIE_MASK)) ||
            ((isr & R_ISR_TC_MASK) && (cr1 & R_CR1_TCIE_MASK)) ||
            ((isr & R_ISR_TXE_MASK) && (cr1 & R_CR1_TXEIE_MASK)) ||
            ((isr & R_ISR_ORE_MASK) &&
             ((cr1 & R_CR1_RXNEIE_MASK) || (cr3 & R_CR3_EIE_MASK))) ||
            (((isr & R_ISR_FE_MASK) || (isr & R_ISR_NE_MASK)) &&
             (cr3 & R_CR3_EIE_MASK)) ||
            ((isr & R_ISR_LBDF_MASK) && (cr2 & R_CR2_LBDIE_MASK)) ||
            ((isr & R_ISR_CTSIF_MASK) && (cr3 & R_CR3_CTSIE_MASK)) ||
            ((isr & R_ISR_RTOF_MASK) && (cr1 & R_CR1_RTOIE_MASK)) ||
            ((isr & R_ISR_CMF_MASK) && (cr1 & R_CR1_CMIE_MASK)) ||
            ((isr & R_ISR_WUF_MASK) && (cr3 & R_CR3_WUFIE_MASK));

    if (variant->is_usart) {
        level = level ||
                ((isr & R_ISR_EOBF_MASK) &&
                 (cr1 & R_CR1_EOBIE_MASK)) ||
                ((isr & R_ISR_UDR_MASK) &&
                 (cr3 & R_CR3_EIE_MASK)) ||
                ((isr & R_ISR_TCBGT_MASK) &&
                 (cr3 & R_CR3_TCBGTIE_MASK));
    }

    qemu_set_irq(s->irq, level);
}

static int stm32g474_usart_can_receive(void *opaque)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);
    uint32_t cr1 = s->regs[R_CR1];

    return s->handlers_installed &&
           !s->host_io_blocked &&
           !stm32g474_usart_in_reset(s) &&
           clock_get_hz(s->clk) != 0 &&
           (cr1 & R_CR1_UE_MASK) &&
           (cr1 & R_CR1_RE_MASK) &&
           !(s->regs[R_ISR] & R_ISR_RXNE_MASK);
}

static void stm32g474_usart_accept_input(Stm32g474UsartState *s)
{
    if (qemu_chr_fe_backend_connected(&s->chr) &&
        stm32g474_usart_can_receive(s)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static void stm32g474_usart_receive(void *opaque, const uint8_t *buf,
                                    int size)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);

    if (size <= 0 || !s->handlers_installed || s->host_io_blocked ||
        stm32g474_usart_in_reset(s) ||
        clock_get_hz(s->clk) == 0 ||
        !(s->regs[R_CR1] & R_CR1_UE_MASK) ||
        !(s->regs[R_CR1] & R_CR1_RE_MASK)) {
        return;
    }

    if (s->regs[R_ISR] & R_ISR_RXNE_MASK) {
        if (s->regs[R_CR3] & R_CR3_OVRDIS_MASK) {
            s->regs[R_RDR] = buf[0];
        } else {
            s->regs[R_ISR] |= R_ISR_ORE_MASK;
        }
    } else {
        s->regs[R_RDR] = buf[0];
        s->regs[R_ISR] |= R_ISR_RXNE_MASK;
    }

    stm32g474_usart_update_irq(s);
}

static void stm32g474_usart_complete_tx(Stm32g474UsartState *s)
{
    s->tx_pending = false;
    s->regs[R_ISR] |= STM32G474_USART_ISR_RESET;
    stm32g474_usart_update_irq(s);
}

static gboolean stm32g474_usart_transmit_watch(void *do_not_use,
                                               GIOCondition cond,
                                               void *opaque);

static void stm32g474_usart_try_transmit(Stm32g474UsartState *s)
{
    uint8_t byte;
    int ret;

    if (!s->tx_pending || s->watch_tag || s->host_io_blocked ||
        stm32g474_usart_in_reset(s) ||
        clock_get_hz(s->clk) == 0) {
        return;
    }

    if (!(s->regs[R_CR1] & R_CR1_UE_MASK) ||
        !(s->regs[R_CR1] & R_CR1_TE_MASK)) {
        stm32g474_usart_complete_tx(s);
        return;
    }

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        stm32g474_usart_complete_tx(s);
        return;
    }

    byte = s->regs[R_TDR] & UINT8_MAX;
    ret = qemu_chr_fe_write(&s->chr, &byte, 1);
    if (ret == 1) {
        stm32g474_usart_complete_tx(s);
        return;
    }

    if (ret == 0 || (ret == -1 && errno == EAGAIN)) {
        s->watch_tag = qemu_chr_fe_add_watch(
            &s->chr, G_IO_OUT | G_IO_HUP,
            stm32g474_usart_transmit_watch, s);
        if (s->watch_tag) {
            return;
        }
    }

    stm32g474_usart_complete_tx(s);
}

static gboolean stm32g474_usart_transmit_watch(void *do_not_use,
                                               GIOCondition cond,
                                               void *opaque)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);

    s->watch_tag = 0;
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        if (s->tx_pending && !s->host_io_blocked &&
            !stm32g474_usart_in_reset(s)) {
            stm32g474_usart_complete_tx(s);
        }
        return G_SOURCE_REMOVE;
    }

    stm32g474_usart_try_transmit(s);
    return G_SOURCE_REMOVE;
}

static void stm32g474_usart_update_params(Stm32g474UsartState *s)
{
    const Stm32g474UsartVariant *variant =
        stm32g474_usart_get_variant(s);
    uint32_t cr1 = s->regs[R_CR1];
    uint32_t brr = FIELD_EX32(s->regs[R_BRR], BRR, BRR);
    uint32_t presc = FIELD_EX32(s->regs[R_PRESC], PRESC, PRESCALER);
    uint32_t word_length;
    uint32_t stop;
    uint64_t clock_hz;
    uint64_t denominator;
    uint64_t speed;
    QEMUSerialSetParams params;

    if (!s->handlers_installed || s->host_io_blocked ||
        stm32g474_usart_in_reset(s) ||
        !qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }

    clock_hz = clock_get_hz(s->clk);
    if (clock_hz == 0 || brr < 0x10 ||
        presc >= ARRAY_SIZE(stm32g474_usart_prescalers)) {
        return;
    }

    word_length = (FIELD_EX32(cr1, CR1, M1) << 1) |
                  FIELD_EX32(cr1, CR1, M0);
    if (word_length == 0) {
        params.data_bits = (cr1 & R_CR1_PCE_MASK) ? 7 : 8;
    } else if (word_length == 1) {
        if (!(cr1 & R_CR1_PCE_MASK)) {
            qemu_log_mask(LOG_UNIMP,
                          TYPE_STM32G474_USART_BASE "(%s)"
                          ": 9-bit byte-stream mode is unsupported\n",
                          variant->name);
            return;
        }
        params.data_bits = 8;
    } else if (word_length == 2) {
        params.data_bits = (cr1 & R_CR1_PCE_MASK) ? 6 : 7;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_STM32G474_USART_BASE "(%s)"
                      ": invalid CR1.M word length\n",
                      variant->name);
        return;
    }

    stop = FIELD_EX32(s->regs[R_CR2], CR2, STOP);
    if (stop == 0) {
        params.stop_bits = 1;
    } else if (stop == 2) {
        params.stop_bits = 2;
    } else {
        qemu_log_mask(LOG_UNIMP,
                      TYPE_STM32G474_USART_BASE "(%s)"
                      ": fractional stop bits are unsupported\n",
                      variant->name);
        return;
    }

    if (cr1 & R_CR1_PCE_MASK) {
        params.parity = (cr1 & R_CR1_PS_MASK) ? 'O' : 'E';
    } else {
        params.parity = 'N';
    }

    if (cr1 & R_CR1_OVER8_MASK) {
        uint32_t decoded_brr;

        if (brr & BIT(3)) {
            return;
        }
        decoded_brr = (brr & 0xfff0) | ((brr & 0x7) << 1);
        denominator =
            (uint64_t)stm32g474_usart_prescalers[presc] * decoded_brr;
        speed = (clock_hz * 2) / denominator;
    } else {
        denominator =
            (uint64_t)stm32g474_usart_prescalers[presc] * brr;
        speed = clock_hz / denominator;
    }

    if (speed == 0 || speed > INT_MAX) {
        return;
    }

    params.speed = speed;
    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_PARAMS, &params);
}

static void stm32g474_usart_apply_fifo_policy(Stm32g474UsartState *s)
{
    const Stm32g474UsartVariant *variant =
        stm32g474_usart_get_variant(s);

    s->regs[R_CR1] &= ~STM32G474_USART_CR1_FIFO_MASK;
    s->regs[R_CR3] &= ~STM32G474_USART_CR3_FIFO_MASK;
    s->regs[R_ISR] &= ~STM32G474_USART_ISR_FIFO_MASK;
    s->regs[R_ISR] &= variant->isr_valid_mask;
}

static void stm32g474_usart_update_ack(Stm32g474UsartState *s)
{
    uint32_t cr1 = s->regs[R_CR1];

    s->regs[R_ISR] &= ~(R_ISR_TEACK_MASK | R_ISR_REACK_MASK);
    if ((cr1 & R_CR1_UE_MASK) && (cr1 & R_CR1_TE_MASK)) {
        s->regs[R_ISR] |= R_ISR_TEACK_MASK;
    }
    if ((cr1 & R_CR1_UE_MASK) && (cr1 & R_CR1_RE_MASK)) {
        s->regs[R_ISR] |= R_ISR_REACK_MASK;
    }
}

static uint64_t stm32g474_usart_cr1_pre_write(RegisterInfo *reg,
                                               uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);
    uint32_t old = s->regs[R_CR1];

    if (stm32g474_usart_in_reset(s)) {
        return old;
    }

    if (old & R_CR1_UE_MASK) {
        value = (value & ~STM32G474_USART_CR1_LOCKED_MASK) |
                (old & STM32G474_USART_CR1_LOCKED_MASK);
    }

    return value & ~STM32G474_USART_CR1_FIFO_MASK;
}

static void stm32g474_usart_cr1_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);
    uint32_t cr1;

    if (stm32g474_usart_in_reset(s)) {
        return;
    }

    cr1 = s->regs[R_CR1];
    if (!(cr1 & R_CR1_UE_MASK)) {
        stm32g474_usart_cancel_tx_watch(s);
        s->tx_pending = false;
        s->regs[R_ISR] = STM32G474_USART_ISR_RESET;
    } else if (!(cr1 & R_CR1_TE_MASK) && s->tx_pending) {
        stm32g474_usart_cancel_tx_watch(s);
        s->tx_pending = false;
        s->regs[R_ISR] |= STM32G474_USART_ISR_RESET;
    }

    stm32g474_usart_update_ack(s);
    stm32g474_usart_update_params(s);
    stm32g474_usart_update_irq(s);
    stm32g474_usart_accept_input(s);
}

static uint64_t stm32g474_usart_cr2_pre_write(RegisterInfo *reg,
                                               uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);
    uint32_t old_cr1 = s->regs[R_CR1];
    uint32_t old = s->regs[R_CR2];
    uint32_t locked = 0;

    if (stm32g474_usart_in_reset(s)) {
        return old;
    }

    if (old_cr1 & R_CR1_UE_MASK) {
        locked |= STM32G474_USART_CR2_UE_LOCKED_MASK;
        if (old_cr1 & R_CR1_RE_MASK) {
            locked |= R_CR2_ADD_MASK;
        }
        if (old & R_CR2_ABREN_MASK) {
            locked |= R_CR2_ABRMOD_MASK;
        }
    }
    if (old_cr1 & R_CR1_TE_MASK) {
        locked |= STM32G474_USART_CR2_TE_LOCKED_MASK;
    }

    return (value & ~locked) | (old & locked);
}

static void stm32g474_usart_cr2_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    if (stm32g474_usart_in_reset(s)) {
        return;
    }

    stm32g474_usart_update_params(s);
    stm32g474_usart_update_irq(s);
}

static uint64_t stm32g474_usart_cr3_pre_write(RegisterInfo *reg,
                                               uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);
    const Stm32g474UsartVariant *variant =
        stm32g474_usart_get_variant(s);
    uint32_t old = s->regs[R_CR3];

    if (stm32g474_usart_in_reset(s)) {
        return old;
    }

    if (s->regs[R_CR1] & R_CR1_UE_MASK) {
        value = (value & ~STM32G474_USART_CR3_UE_LOCKED_MASK) |
                (old & STM32G474_USART_CR3_UE_LOCKED_MASK);
        if (variant->is_usart && (value & R_CR3_SCARCNT_MASK)) {
            value = (value & ~R_CR3_SCARCNT_MASK) |
                    (old & R_CR3_SCARCNT_MASK);
        }
    }

    return value & ~STM32G474_USART_CR3_FIFO_MASK;
}

static void stm32g474_usart_cr3_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    if (stm32g474_usart_in_reset(s)) {
        return;
    }

    stm32g474_usart_update_irq(s);
    stm32g474_usart_accept_input(s);
}

static uint64_t stm32g474_usart_brr_pre_write(RegisterInfo *reg,
                                               uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    if (stm32g474_usart_in_reset(s) ||
        (s->regs[R_CR1] & R_CR1_UE_MASK)) {
        return s->regs[R_BRR];
    }

    return value;
}

static void stm32g474_usart_params_post_write(RegisterInfo *reg,
                                               uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    if (!stm32g474_usart_in_reset(s)) {
        stm32g474_usart_update_params(s);
    }
}

static uint64_t stm32g474_usart_gtpr_pre_write(RegisterInfo *reg,
                                                uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    if (stm32g474_usart_in_reset(s) ||
        (s->regs[R_CR1] & R_CR1_UE_MASK)) {
        return s->regs[R_GTPR];
    }

    return value;
}

static uint64_t stm32g474_usart_rtor_pre_write(RegisterInfo *reg,
                                                uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    if (stm32g474_usart_in_reset(s)) {
        return s->regs[R_RTOR];
    }

    return value;
}

static void stm32g474_usart_rqr_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);
    uint32_t commands = s->regs[R_RQR];

    s->regs[R_RQR] = 0;
    if (stm32g474_usart_in_reset(s)) {
        return;
    }

    if (commands & R_RQR_RXFRQ_MASK) {
        s->regs[R_ISR] &= ~R_ISR_RXNE_MASK;
    }

    stm32g474_usart_update_irq(s);
    stm32g474_usart_accept_input(s);
}

static void stm32g474_usart_icr_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);
    const Stm32g474UsartVariant *variant =
        stm32g474_usart_get_variant(s);
    uint32_t commands = s->regs[R_ICR];
    uint32_t clear = 0;

    s->regs[R_ICR] = 0;
    if (stm32g474_usart_in_reset(s)) {
        return;
    }

    if (commands & R_ICR_WUCF_MASK) {
        clear |= R_ISR_WUF_MASK;
    }
    if (commands & R_ICR_CMCF_MASK) {
        clear |= R_ISR_CMF_MASK;
    }
    if (variant->is_usart && (commands & R_ICR_UDRCF_MASK)) {
        clear |= R_ISR_UDR_MASK;
    }
    if (variant->is_usart && (commands & R_ICR_EOBCF_MASK)) {
        clear |= R_ISR_EOBF_MASK;
    }
    if (commands & R_ICR_RTOCF_MASK) {
        clear |= R_ISR_RTOF_MASK;
    }
    if (commands & R_ICR_CTSCF_MASK) {
        clear |= R_ISR_CTSIF_MASK;
    }
    if (commands & R_ICR_LBDCF_MASK) {
        clear |= R_ISR_LBDF_MASK;
    }
    if (variant->is_usart && (commands & R_ICR_TCBGTCF_MASK)) {
        clear |= R_ISR_TCBGT_MASK;
    }
    if (commands & R_ICR_TCCF_MASK) {
        clear |= R_ISR_TC_MASK;
    }
    if (commands & R_ICR_TXFECF_MASK) {
        clear |= R_ISR_TXFE_MASK;
    }
    if (commands & R_ICR_IDLECF_MASK) {
        clear |= R_ISR_IDLE_MASK;
    }
    if (commands & R_ICR_ORECF_MASK) {
        clear |= R_ISR_ORE_MASK;
    }
    if (commands & R_ICR_NECF_MASK) {
        clear |= R_ISR_NE_MASK;
    }
    if (commands & R_ICR_FECF_MASK) {
        clear |= R_ISR_FE_MASK;
    }
    if (commands & R_ICR_PECF_MASK) {
        clear |= R_ISR_PE_MASK;
    }

    s->regs[R_ISR] &= ~clear;
    stm32g474_usart_update_irq(s);
}

static uint64_t stm32g474_usart_rdr_post_read(RegisterInfo *reg,
                                               uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    value &= R_RDR_RDR_MASK;
    if (stm32g474_usart_in_reset(s)) {
        return value;
    }

    s->regs[R_ISR] &= ~R_ISR_RXNE_MASK;
    stm32g474_usart_update_irq(s);
    stm32g474_usart_accept_input(s);
    return value;
}

static uint64_t stm32g474_usart_tdr_pre_write(RegisterInfo *reg,
                                               uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    s->tdr_write_accepted = false;
    if (stm32g474_usart_in_reset(s) ||
        !(s->regs[R_CR1] & R_CR1_UE_MASK) ||
        !(s->regs[R_CR1] & R_CR1_TE_MASK) ||
        !(s->regs[R_ISR] & R_ISR_TXE_MASK)) {
        return s->regs[R_TDR];
    }

    s->tdr_write_accepted = true;
    return value;
}

static void stm32g474_usart_tdr_post_write(RegisterInfo *reg,
                                            uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);
    bool accepted = s->tdr_write_accepted;

    s->tdr_write_accepted = false;
    if (!accepted || stm32g474_usart_in_reset(s)) {
        return;
    }

    s->regs[R_ISR] &= ~STM32G474_USART_ISR_RESET;
    s->tx_pending = true;
    stm32g474_usart_update_irq(s);
    if (clock_get_hz(s->clk) != 0) {
        stm32g474_usart_try_transmit(s);
    }
}

static uint64_t stm32g474_usart_presc_pre_write(RegisterInfo *reg,
                                                 uint64_t value)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(reg->opaque);

    if (stm32g474_usart_in_reset(s) ||
        (s->regs[R_CR1] & R_CR1_UE_MASK)) {
        return s->regs[R_PRESC];
    }

    if (value > 0xb) {
        value = 0xb;
    }
    return value;
}

static const RegisterAccessInfo stm32g474_usart_u_regs_info[] = {
    {
        .name = "CR1",
        .addr = A_CR1,
        .unimp = STM32G474_USART_COMMON_CR1_UNIMP_MASK |
                  R_CR1_EOBIE_MASK,
        .pre_write = stm32g474_usart_cr1_pre_write,
        .post_write = stm32g474_usart_cr1_post_write,
    }, {
        .name = "CR2",
        .addr = A_CR2,
        .rsvd = 0x00000086,
        .unimp = 0xffffff79 & ~R_CR2_STOP_MASK,
        .pre_write = stm32g474_usart_cr2_pre_write,
        .post_write = stm32g474_usart_cr2_post_write,
    }, {
        .name = "CR3",
        .addr = A_CR3,
        .rsvd = 0x00010000,
        .unimp = 0xfffeffff &
                  ~(R_CR3_OVRDIS_MASK | R_CR3_EIE_MASK),
        .pre_write = stm32g474_usart_cr3_pre_write,
        .post_write = stm32g474_usart_cr3_post_write,
    }, {
        .name = "BRR",
        .addr = A_BRR,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_usart_brr_pre_write,
        .post_write = stm32g474_usart_params_post_write,
    }, {
        .name = "GTPR",
        .addr = A_GTPR,
        .rsvd = 0xffff0000,
        .unimp = 0x0000ffff,
        .pre_write = stm32g474_usart_gtpr_pre_write,
    }, {
        .name = "RTOR",
        .addr = A_RTOR,
        .unimp = UINT32_MAX,
        .pre_write = stm32g474_usart_rtor_pre_write,
    }, {
        .name = "RQR",
        .addr = A_RQR,
        .rsvd = 0xffffffe0,
        .unimp = R_RQR_TXFRQ_MASK | R_RQR_MMRQ_MASK |
                  R_RQR_SBKRQ_MASK | R_RQR_ABRRQ_MASK,
        .post_write = stm32g474_usart_rqr_post_write,
    }, {
        .name = "ISR",
        .addr = A_ISR,
        .reset = STM32G474_USART_ISR_RESET,
        .ro = STM32G474_USART_U_ISR_VALID_MASK,
        .rsvd = 0xfd800000,
        .unimp = STM32G474_USART_U_ISR_VALID_MASK &
                  ~STM32G474_USART_U_ISR_MODELED_MASK,
    }, {
        .name = "ICR",
        .addr = A_ICR,
        .rsvd = 0xffedc400,
        .post_write = stm32g474_usart_icr_post_write,
    }, {
        .name = "RDR",
        .addr = A_RDR,
        .ro = R_RDR_RDR_MASK,
        .rsvd = 0xfffffe00,
        .post_read = stm32g474_usart_rdr_post_read,
    }, {
        .name = "TDR",
        .addr = A_TDR,
        .rsvd = 0xfffffe00,
        .pre_write = stm32g474_usart_tdr_pre_write,
        .post_write = stm32g474_usart_tdr_post_write,
    }, {
        .name = "PRESC",
        .addr = A_PRESC,
        .rsvd = 0xfffffff0,
        .pre_write = stm32g474_usart_presc_pre_write,
        .post_write = stm32g474_usart_params_post_write,
    },
};

static const RegisterAccessInfo stm32g474_usart_a_regs_info[] = {
    {
        .name = "CR1",
        .addr = A_CR1,
        .rsvd = 0x08000000,
        .unimp = STM32G474_USART_COMMON_CR1_UNIMP_MASK,
        .pre_write = stm32g474_usart_cr1_pre_write,
        .post_write = stm32g474_usart_cr1_post_write,
    }, {
        .name = "CR2",
        .addr = A_CR2,
        .rsvd = 0x00000f8f,
        .unimp = 0xfffff070 & ~R_CR2_STOP_MASK,
        .pre_write = stm32g474_usart_cr2_pre_write,
        .post_write = stm32g474_usart_cr2_post_write,
    }, {
        .name = "CR3",
        .addr = A_CR3,
        .rsvd = 0x010f0030,
        .unimp = 0xfef0ffcf &
                  ~(R_CR3_OVRDIS_MASK | R_CR3_EIE_MASK),
        .pre_write = stm32g474_usart_cr3_pre_write,
        .post_write = stm32g474_usart_cr3_post_write,
    }, {
        .name = "BRR",
        .addr = A_BRR,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_usart_brr_pre_write,
        .post_write = stm32g474_usart_params_post_write,
    }, {
        .name = "GTPR",
        .addr = A_GTPR,
        .rsvd = 0xffffff00,
        .unimp = 0x000000ff,
        .pre_write = stm32g474_usart_gtpr_pre_write,
    }, {
        .name = "RTOR",
        .addr = A_RTOR,
        .unimp = UINT32_MAX,
        .pre_write = stm32g474_usart_rtor_pre_write,
    }, {
        .name = "RQR",
        .addr = A_RQR,
        .rsvd = 0xffffffe0,
        .unimp = R_RQR_TXFRQ_MASK | R_RQR_MMRQ_MASK |
                  R_RQR_SBKRQ_MASK | R_RQR_ABRRQ_MASK,
        .post_write = stm32g474_usart_rqr_post_write,
    }, {
        .name = "ISR",
        .addr = A_ISR,
        .reset = STM32G474_USART_ISR_RESET,
        .ro = STM32G474_USART_A_ISR_VALID_MASK,
        .rsvd = 0xff803000,
        .unimp = STM32G474_USART_A_ISR_VALID_MASK &
                  ~STM32G474_USART_COMMON_ISR_MODELED_MASK,
    }, {
        .name = "ICR",
        .addr = A_ICR,
        .rsvd = 0xffedf480,
        .post_write = stm32g474_usart_icr_post_write,
    }, {
        .name = "RDR",
        .addr = A_RDR,
        .ro = R_RDR_RDR_MASK,
        .rsvd = 0xfffffe00,
        .post_read = stm32g474_usart_rdr_post_read,
    }, {
        .name = "TDR",
        .addr = A_TDR,
        .rsvd = 0xfffffe00,
        .pre_write = stm32g474_usart_tdr_pre_write,
        .post_write = stm32g474_usart_tdr_post_write,
    }, {
        .name = "PRESC",
        .addr = A_PRESC,
        .rsvd = 0xfffffff0,
        .pre_write = stm32g474_usart_presc_pre_write,
        .post_write = stm32g474_usart_params_post_write,
    },
};

static const Stm32g474UsartVariant stm32g474_usart_u_variant = {
    .name = "USART",
    .regs_info = stm32g474_usart_u_regs_info,
    .num_regs = ARRAY_SIZE(stm32g474_usart_u_regs_info),
    .isr_valid_mask = STM32G474_USART_U_ISR_VALID_MASK,
    .is_usart = true,
};

static const Stm32g474UsartVariant stm32g474_usart_a_variant = {
    .name = "UART",
    .regs_info = stm32g474_usart_a_regs_info,
    .num_regs = ARRAY_SIZE(stm32g474_usart_a_regs_info),
    .isr_valid_mask = STM32G474_USART_A_ISR_VALID_MASK,
    .is_usart = false,
};

G_STATIC_ASSERT(STM32G474_USART_NUM_REGS ==
                A_PRESC / sizeof(uint32_t) + 1);

static const MemoryRegionOps stm32g474_usart_ops = {
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

static void stm32g474_usart_reset_registers(Stm32g474UsartState *s)
{
    for (int i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }
}

static void stm32g474_usart_reset_input(void *opaque, int n, int level)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);
    bool asserted = level != 0;
    bool was_resetting;

    if (asserted == s->peripheral_reset_asserted) {
        return;
    }

    s->peripheral_reset_asserted = asserted;
    if (!asserted) {
        return;
    }

    stm32g474_usart_cancel_tx_watch(s);
    was_resetting = s->resetting;
    s->resetting = true;
    s->tdr_write_accepted = false;
    stm32g474_usart_reset_registers(s);
    s->tx_pending = false;
    s->watch_tag = 0;
    s->resetting = was_resetting;
    if (!s->host_io_blocked) {
        qemu_set_irq(s->irq, 0);
    }
}

static void stm32g474_usart_reset_enter(Object *obj, ResetType type)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(obj);

    s->resetting = true;
    s->tdr_write_accepted = false;
    stm32g474_usart_cancel_tx_watch(s);
}

static void stm32g474_usart_reset_hold(Object *obj, ResetType type)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(obj);

    stm32g474_usart_reset_registers(s);
    s->tx_pending = false;
    s->tdr_write_accepted = false;
    s->peripheral_reset_asserted = false;
    s->watch_tag = 0;
    if (!s->host_io_blocked) {
        qemu_set_irq(s->irq, 0);
    }
}

static void stm32g474_usart_reset_exit(Object *obj, ResetType type)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(obj);

    s->resetting = false;
    stm32g474_usart_apply_fifo_policy(s);
    stm32g474_usart_update_ack(s);
    stm32g474_usart_update_params(s);
    stm32g474_usart_update_irq(s);
    stm32g474_usart_accept_input(s);
}

static void stm32g474_usart_clock_update(void *opaque, ClockEvent event)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);

    if (!s->handlers_installed || s->host_io_blocked ||
        stm32g474_usart_in_reset(s)) {
        return;
    }

    if (clock_get_hz(s->clk) == 0) {
        stm32g474_usart_cancel_tx_watch(s);
        stm32g474_usart_update_irq(s);
        return;
    }

    stm32g474_usart_update_params(s);
    if (s->tx_pending) {
        stm32g474_usart_try_transmit(s);
    }
    stm32g474_usart_update_irq(s);
    stm32g474_usart_accept_input(s);
}

static int stm32g474_usart_pre_load(void *opaque)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);

    s->host_io_blocked = true;
    stm32g474_usart_cancel_resume(s);
    stm32g474_usart_cancel_tx_watch(s);
    s->tdr_write_accepted = false;
    s->resetting = true;
    return 0;
}

static void stm32g474_usart_resume(void *opaque, bool running,
                                   RunState state)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);
    VMChangeStateEntry *entry;

    if (!running) {
        return;
    }

    entry = s->resume_entry;
    s->resume_entry = NULL;
    if (entry) {
        qemu_del_vm_change_state_handler(entry);
    }

    s->host_io_blocked = false;
    stm32g474_usart_update_irq(s);
    stm32g474_usart_update_params(s);
    if (s->tx_pending) {
        stm32g474_usart_try_transmit(s);
    }
    stm32g474_usart_accept_input(s);
}

static int stm32g474_usart_post_load(void *opaque, int version_id)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(opaque);
    const Stm32g474UsartVariant *variant =
        stm32g474_usart_get_variant(s);

    s->host_io_blocked = true;
    stm32g474_usart_cancel_resume(s);
    stm32g474_usart_cancel_tx_watch(s);
    s->watch_tag = 0;
    s->tdr_write_accepted = false;
    for (size_t i = 0; i < variant->num_regs; i++) {
        const RegisterAccessInfo *access = &variant->regs_info[i];
        unsigned int index = access->addr / sizeof(uint32_t);

        s->regs[index] = (s->regs[index] & ~access->rsvd) |
                         (access->reset & access->rsvd);
    }
    stm32g474_usart_apply_fifo_policy(s);
    s->regs[R_RQR] = 0;
    s->regs[R_ICR] = 0;
    if (s->regs[R_PRESC] > 0xb) {
        s->regs[R_PRESC] = 0xb;
    }

    if (s->peripheral_reset_asserted) {
        stm32g474_usart_reset_registers(s);
        s->tx_pending = false;
    } else if (s->tx_pending &&
               (!(s->regs[R_CR1] & R_CR1_UE_MASK) ||
                !(s->regs[R_CR1] & R_CR1_TE_MASK))) {
        s->tx_pending = false;
        s->regs[R_ISR] |= STM32G474_USART_ISR_RESET;
    } else if (s->tx_pending) {
        s->regs[R_ISR] &= ~STM32G474_USART_ISR_RESET;
    }

    stm32g474_usart_update_ack(s);
    s->resetting = false;
    s->resume_entry = qdev_add_vm_change_state_handler(
        DEVICE(s), stm32g474_usart_resume, NULL, s);
    return 0;
}

static const VMStateDescription vmstate_stm32g474_usart = {
    .name = TYPE_STM32G474_USART_BASE,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = stm32g474_usart_pre_load,
    .post_load = stm32g474_usart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474UsartState,
                             STM32G474_USART_NUM_REGS),
        VMSTATE_BOOL(tx_pending, Stm32g474UsartState),
        VMSTATE_BOOL(peripheral_reset_asserted, Stm32g474UsartState),
        VMSTATE_CLOCK(clk, Stm32g474UsartState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stm32g474_usart_properties[] = {
    DEFINE_PROP_CHR("chardev", Stm32g474UsartState, chr),
};

static void stm32g474_usart_realize(DeviceState *dev, Error **errp)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(dev);
    const Stm32g474UsartVariant *variant =
        stm32g474_usart_get_variant(s);

    if (!variant || !s->reg_array) {
        error_setg(errp, TYPE_STM32G474_USART_BASE
                   ": concrete variant is missing");
        return;
    }
    if (!clock_has_source(s->clk)) {
        error_setg(errp, TYPE_STM32G474_USART_BASE
                   ": clk clock must be connected");
        return;
    }

    qemu_chr_fe_set_handlers(&s->chr, stm32g474_usart_can_receive,
                             stm32g474_usart_receive, NULL, NULL,
                             s, NULL, true);
    s->handlers_installed = true;
}

static void stm32g474_usart_unrealize(DeviceState *dev)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(dev);

    s->handlers_installed = false;
    stm32g474_usart_cancel_resume(s);
    stm32g474_usart_cancel_tx_watch(s);
    qemu_chr_fe_deinit(&s->chr, false);
}

static void stm32g474_usart_init(Object *obj)
{
    Stm32g474UsartState *s = STM32G474_USART_BASE(obj);
    Stm32g474UsartClass *uc = STM32G474_USART_BASE_GET_CLASS(obj);
    DeviceState *dev = DEVICE(obj);

    g_assert(uc->variant);
    g_assert(uc->variant->num_regs <= STM32G474_USART_NUM_REGS);

    s->reg_array = register_init_block32(
        dev, uc->variant->regs_info, uc->variant->num_regs,
        s->regs_info, s->regs, &stm32g474_usart_ops, false,
        STM32G474_USART_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->clk = qdev_init_clock_in(dev, "clk", stm32g474_usart_clock_update,
                                s, ClockUpdate);
    qdev_init_gpio_in_named(dev, stm32g474_usart_reset_input, "reset", 1);
}

static void stm32g474_usart_base_class_init(ObjectClass *klass,
                                             const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_usart_realize;
    dc->unrealize = stm32g474_usart_unrealize;
    dc->vmsd = &vmstate_stm32g474_usart;
    dc->user_creatable = false;
    device_class_set_props(dc, stm32g474_usart_properties);
    rc->phases.enter = stm32g474_usart_reset_enter;
    rc->phases.hold = stm32g474_usart_reset_hold;
    rc->phases.exit = stm32g474_usart_reset_exit;
}

static void stm32g474_usart_class_init(ObjectClass *klass,
                                        const void *data)
{
    Stm32g474UsartClass *uc = STM32G474_USART_BASE_CLASS(klass);

    uc->variant = &stm32g474_usart_u_variant;
}

static void stm32g474_uart_class_init(ObjectClass *klass,
                                       const void *data)
{
    Stm32g474UsartClass *uc = STM32G474_USART_BASE_CLASS(klass);

    uc->variant = &stm32g474_usart_a_variant;
}

static const TypeInfo stm32g474_usart_types[] = {
    {
        .name = TYPE_STM32G474_USART_BASE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Stm32g474UsartState),
        .class_size = sizeof(Stm32g474UsartClass),
        .instance_init = stm32g474_usart_init,
        .class_init = stm32g474_usart_base_class_init,
        .abstract = true,
    }, {
        .name = TYPE_STM32G474_USART,
        .parent = TYPE_STM32G474_USART_BASE,
        .class_init = stm32g474_usart_class_init,
    }, {
        .name = TYPE_STM32G474_UART,
        .parent = TYPE_STM32G474_USART_BASE,
        .class_init = stm32g474_uart_class_init,
    },
};

DEFINE_TYPES(stm32g474_usart_types)
