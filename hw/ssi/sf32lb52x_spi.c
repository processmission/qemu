/*
 * SiFli SF32LB52X SPI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout follows the vendor header spi.h (SPI_TypeDef) definitions
 * and the SF32LB52x SVD SPI1 block; behavior follows the Zephyr spi_sf32lb
 * driver and the vendor HAL/LL sources.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/sf32lb52x_spi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

/* Register offsets (SVD SPI1 dump:3-86; HAL spi.h:10-28). */
#define SPI_TOP_CTRL    0x00
#define SPI_FIFO_CTRL   0x04
#define SPI_INTE        0x08
#define SPI_TO          0x0c
#define SPI_DATA        0x10
#define SPI_STATUS      0x14
#define SPI_PSP_CTRL    0x18
#define SPI_NW_CTRL     0x1c
#define SPI_NW_STATUS   0x20
#define SPI_RWOT_CTRL   0x24
#define SPI_RWOT_CCM    0x28
#define SPI_RWOT_CVWRN  0x2c
#define SPI_CLK_CTRL    0x3c
#define SPI_TRIWIRE_CTRL 0x54

/* TOP_CTRL bits (HAL spi.h:31-73). */
#define SPI_TOP_CTRL_SSE          (1U << 0)
#define SPI_TOP_CTRL_FRF_Msk      (0x3U << 1)
#define SPI_TOP_CTRL_SCLKDIR      (1U << 3)
#define SPI_TOP_CTRL_SFRMDIR      (1U << 4)
#define SPI_TOP_CTRL_DSS_Msk      (0x1fU << 5)
#define SPI_TOP_CTRL_SPO          (1U << 10)
#define SPI_TOP_CTRL_SPH          (1U << 11)
#define SPI_TOP_CTRL_LBM          (1U << 12)
#define SPI_TOP_CTRL_TRAIL        (1U << 13)
#define SPI_TOP_CTRL_HOLD_FRAME_LOW (1U << 14)
#define SPI_TOP_CTRL_IFS          (1U << 15)
#define SPI_TOP_CTRL_SCFR         (1U << 16)
#define SPI_TOP_CTRL_TTE          (1U << 17)
#define SPI_TOP_CTRL_TTELP        (1U << 18)
#define SPI_TOP_CTRL_WRITE_Msk    0x0007ffffU

/*
 * FIFO_CTRL bits (HAL spi.h:75-105). TSRE/RSRE are write-1, self-clearing
 * FIFO reset pulses.
 */
#define SPI_FIFO_CTRL_TFT_Msk      0x1fU
#define SPI_FIFO_CTRL_RFT_Msk      (0x1fU << 5)
#define SPI_FIFO_CTRL_TSRE         (1U << 10)
#define SPI_FIFO_CTRL_RSRE         (1U << 11)
#define SPI_FIFO_CTRL_WRITE_Msk    0x000ff3ffU

/* INTE bits (HAL spi.h:107-128). */
#define SPI_INTE_PINTE             (1U << 0)
#define SPI_INTE_TINTE             (1U << 1)
#define SPI_INTE_RIE               (1U << 2)
#define SPI_INTE_TIE               (1U << 3)
#define SPI_INTE_RIM               (1U << 4)
#define SPI_INTE_TIM               (1U << 5)
#define SPI_INTE_EBCEI             (1U << 6)
#define SPI_INTE_WRITE_Msk         0x7fU

/* STATUS bits (HAL spi.h:140-188). TUR/ROR/TINT are rw1c. */
#define SPI_STATUS_BSY             (1U << 0)
#define SPI_STATUS_CSS             (1U << 1)
#define SPI_STATUS_PINT            (1U << 2)
#define SPI_STATUS_TINT            (1U << 3)
#define SPI_STATUS_EOC             (1U << 4)
#define SPI_STATUS_TFS             (1U << 5)
#define SPI_STATUS_TNF             (1U << 6)
#define SPI_STATUS_TFL_Msk         (0xfU << 7)
#define SPI_STATUS_TUR             (1U << 12)
#define SPI_STATUS_RFS             (1U << 13)
#define SPI_STATUS_RNE             (1U << 14)
#define SPI_STATUS_RFL_Msk         (0xfU << 15)
#define SPI_STATUS_ROR             (1U << 20)
#define SPI_STATUS_BCE             (1U << 21)
#define SPI_STATUS_TX_OSS          (1U << 22)
#define SPI_STATUS_OSS             (1U << 23)
#define SPI_STATUS_W1C_Msk                                                     \
    (SPI_STATUS_TINT | SPI_STATUS_TUR | SPI_STATUS_ROR)
#define SPI_STATUS_RFL_EMPTY       0xfU

/* CLK_CTRL / TRIWIRE_CTRL / RWOT_CTRL field masks. */
#define SPI_CLK_CTRL_WRITE_Msk     0x3ffU
#define SPI_TRIWIRE_CTRL_WRITE_Msk 0x7U
#define SPI_RWOT_CTRL_WRITE_Msk    0x1fU
#define SPI_RWOT_CTRL_CYCLE_RWOT_EN (1U << 1)

static bool spi_sse_enabled(SF32LB52XSPIState *s)
{
    return (s->top_ctrl & SPI_TOP_CTRL_SSE) != 0;
}

static uint32_t spi_rx_threshold(SF32LB52XSPIState *s)
{
    return (s->fifo_ctrl & SPI_FIFO_CTRL_RFT_Msk) >> 5;
}

static uint32_t spi_tx_threshold(SF32LB52XSPIState *s)
{
    return s->fifo_ctrl & SPI_FIFO_CTRL_TFT_Msk;
}

static uint32_t spi_status(SF32LB52XSPIState *s)
{
    uint32_t status = s->status_flags;
    uint32_t tfl = MIN(s->tx_count, 15U);
    uint32_t rfl;

    if (s->busy && spi_sse_enabled(s)) {
        status |= SPI_STATUS_BSY;
    }
    if (spi_sse_enabled(s)) {
        if (s->tx_count <= spi_tx_threshold(s)) {
            status |= SPI_STATUS_TFS;
        }
        if (s->rx_count > spi_rx_threshold(s)) {
            status |= SPI_STATUS_RFS;
        }
    }
    if (s->tx_count < SF32LB52X_SPI_FIFO_DEPTH) {
        status |= SPI_STATUS_TNF;
    }
    status |= tfl << 7;

    if (s->rx_count > 0) {
        status |= SPI_STATUS_RNE;
        rfl = MIN(s->rx_count - 1U, 15U);
    } else {
        rfl = SPI_STATUS_RFL_EMPTY;
    }
    status |= rfl << 15;

    return status;
}

static void spi_update_irq(SF32LB52XSPIState *s)
{
    uint32_t status = spi_status(s);
    bool level = false;

    if ((s->inte & SPI_INTE_RIE) && (status & SPI_STATUS_RFS)) {
        level = true;
    }
    if ((s->inte & SPI_INTE_TIE) && (status & SPI_STATUS_TFS)) {
        level = true;
    }
    if ((s->inte & SPI_INTE_TINTE) && (status & SPI_STATUS_TINT)) {
        level = true;
    }
    if ((s->status_flags & SPI_STATUS_ROR) && !(s->inte & SPI_INTE_RIM)) {
        level = true;
    }
    if ((s->status_flags & SPI_STATUS_TUR) && !(s->inte & SPI_INTE_TIM)) {
        level = true;
    }

    qemu_set_irq(s->irq, level);
}

static void spi_shift_reschedule(SF32LB52XSPIState *s)
{
    timer_mod(s->shift_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SF32LB52X_SPI_SHIFT_NS);
}

static void spi_rx_push(SF32LB52XSPIState *s, uint32_t frame)
{
    if (s->rx_count >= SF32LB52X_SPI_FIFO_DEPTH) {
        /* SVD: attempted data write to a full RXFIFO is an overrun. */
        s->status_flags |= SPI_STATUS_ROR;
        return;
    }

    s->rx_fifo[(s->rx_head + s->rx_count) % SF32LB52X_SPI_FIFO_DEPTH] = frame;
    s->rx_count++;
}

static void spi_shift_expire(void *opaque)
{
    SF32LB52XSPIState *s = opaque;

    if (!spi_sse_enabled(s)) {
        s->busy = false;
        spi_update_irq(s);
        return;
    }

    if (s->tx_count > 0) {
        uint32_t frame = s->tx_fifo[s->tx_head];

        s->tx_head = (s->tx_head + 1U) % SF32LB52X_SPI_FIFO_DEPTH;
        s->tx_count--;

        /*
         * No SPI peer exists on the DevKit-LCD. With TOP_CTRL.LBM set the
         * controller loops MOSI back to MISO (vendor HAL spi.h:53-55); with
         * LBM clear the open bus reads as zero. Either way one received frame
         * is queued so the Zephyr polled transceive cannot hang.
         */
        spi_rx_push(s, (s->top_ctrl & SPI_TOP_CTRL_LBM) ? frame : 0U);
    }

    if (s->tx_count > 0) {
        spi_shift_reschedule(s);
    } else {
        s->busy = false;
    }

    spi_update_irq(s);
}

static void spi_fifo_reset(SF32LB52XSPIState *s)
{
    if (s->fifo_ctrl & SPI_FIFO_CTRL_TSRE) {
        s->tx_head = 0;
        s->tx_count = 0;
    }
    if (s->fifo_ctrl & SPI_FIFO_CTRL_RSRE) {
        s->rx_head = 0;
        s->rx_count = 0;
    }
}

static bool spi_is_unimplemented(hwaddr addr)
{
    return addr == SPI_PSP_CTRL || addr == SPI_NW_CTRL || addr == SPI_NW_STATUS;
}

static uint64_t spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    SF32LB52XSPIState *s = opaque;
    uint32_t data;
    uint32_t value;

    if (addr >= SF32LB52X_SPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    if (addr == SPI_DATA) {
        if (s->rx_count == 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: DATA read with empty RX FIFO\n",
                          __func__);
            return 0;
        }
        data = s->rx_fifo[s->rx_head];
        s->rx_head = (s->rx_head + 1U) % SF32LB52X_SPI_FIFO_DEPTH;
        s->rx_count--;
        spi_update_irq(s);
        if (size == 1) {
            return data & 0xffU;
        }
        if (size == 2) {
            return data & 0xffffU;
        }
        return data;
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return 0;
    }

    if (spi_is_unimplemented(addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented register 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }

    switch (addr) {
    case SPI_TOP_CTRL:
        value = s->top_ctrl & SPI_TOP_CTRL_WRITE_Msk;
        break;
    case SPI_FIFO_CTRL:
        value = s->fifo_ctrl & SPI_FIFO_CTRL_WRITE_Msk;
        break;
    case SPI_INTE:
        value = s->inte;
        break;
    case SPI_TO:
        value = s->to;
        break;
    case SPI_STATUS:
        value = spi_status(s);
        break;
    case SPI_RWOT_CTRL:
        value = s->rwot_ctrl;
        break;
    case SPI_RWOT_CCM:
        value = s->rwot_ccm;
        break;
    case SPI_RWOT_CVWRN:
        value = s->rwot_cvwrn;
        break;
    case SPI_CLK_CTRL:
        value = s->clk_ctrl;
        break;
    case SPI_TRIWIRE_CTRL:
        value = s->triwire_ctrl;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        value = 0;
        break;
    }

    return value;
}

static void spi_write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned int size)
{
    SF32LB52XSPIState *s = opaque;
    uint32_t val = value;

    if (addr >= SF32LB52X_SPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    if (addr == SPI_DATA) {
        uint32_t frame;

        if (s->tx_count >= SF32LB52X_SPI_FIFO_DEPTH) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: TX FIFO full, DATA write\n",
                          __func__);
            return;
        }
        if (size == 1) {
            frame = val & 0xffU;
        } else if (size == 2) {
            frame = val & 0xffffU;
        } else {
            frame = val;
        }
        s->tx_fifo[(s->tx_head + s->tx_count) % SF32LB52X_SPI_FIFO_DEPTH] =
            frame;
        s->tx_count++;
        if (!s->busy) {
            s->busy = true;
            spi_shift_reschedule(s);
        }
        spi_update_irq(s);
        return;
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return;
    }

    if (spi_is_unimplemented(addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented register 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return;
    }

    switch (addr) {
    case SPI_TOP_CTRL:
        s->top_ctrl = val & SPI_TOP_CTRL_WRITE_Msk;
        if (!spi_sse_enabled(s)) {
            timer_del(s->shift_timer);
            s->busy = false;
        }
        break;
    case SPI_FIFO_CTRL:
        s->fifo_ctrl = val & SPI_FIFO_CTRL_WRITE_Msk;
        if (val & (SPI_FIFO_CTRL_TSRE | SPI_FIFO_CTRL_RSRE)) {
            uint32_t pulses = val & (SPI_FIFO_CTRL_TSRE | SPI_FIFO_CTRL_RSRE);

            s->fifo_ctrl |= pulses;
            spi_fifo_reset(s);
            s->fifo_ctrl &= ~pulses;
        }
        break;
    case SPI_INTE:
        s->inte = val & SPI_INTE_WRITE_Msk;
        if (s->inte & SPI_INTE_TINTE) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: receiver timeout interrupt not modeled\n",
                          __func__);
        }
        if (s->inte & (SPI_INTE_PINTE | SPI_INTE_EBCEI)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: PINT/EBCE interrupts not modeled\n",
                          __func__);
        }
        break;
    case SPI_TO:
        s->to = val & 0xffffffU;
        break;
    case SPI_STATUS:
        s->status_flags &= ~(val & SPI_STATUS_W1C_Msk);
        break;
    case SPI_RWOT_CTRL:
        s->rwot_ctrl = val & SPI_RWOT_CTRL_WRITE_Msk;
        if (s->rwot_ctrl & SPI_RWOT_CTRL_CYCLE_RWOT_EN) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: RWOT cycle counter not modeled\n", __func__);
        }
        break;
    case SPI_RWOT_CCM:
        s->rwot_ccm = val;
        break;
    case SPI_RWOT_CVWRN:
        s->rwot_cvwrn = val;
        break;
    case SPI_CLK_CTRL:
        s->clk_ctrl = val & SPI_CLK_CTRL_WRITE_Msk;
        break;
    case SPI_TRIWIRE_CTRL:
        s->triwire_ctrl = val & SPI_TRIWIRE_CTRL_WRITE_Msk;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    spi_update_irq(s);
}

static const MemoryRegionOps spi_ops = {
    .read = spi_read,
    .write = spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void spi_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XSPIState *s = SF32LB52X_SPI(obj);

    timer_del(s->shift_timer);
    s->top_ctrl = 0;
    s->fifo_ctrl = 0;
    s->inte = 0;
    s->to = 0;
    s->clk_ctrl = 0;
    s->triwire_ctrl = 0;
    s->rwot_ctrl = 0;
    s->rwot_ccm = 0;
    s->rwot_cvwrn = 0;
    s->status_flags = 0;
    memset(s->tx_fifo, 0, sizeof(s->tx_fifo));
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    s->tx_head = 0;
    s->rx_head = 0;
    s->tx_count = 0;
    s->rx_count = 0;
    s->busy = false;
    qemu_set_irq(s->irq, 0);
}

static void spi_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XSPIState *s = SF32LB52X_SPI(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &spi_ops, s,
                          TYPE_SF32LB52X_SPI, SF32LB52X_SPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->shift_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, spi_shift_expire, s);
}

static void spi_finalize(Object *obj)
{
    SF32LB52XSPIState *s = SF32LB52X_SPI(obj);

    timer_free(s->shift_timer);
}

static int spi_post_load(void *opaque, int version_id)
{
    SF32LB52XSPIState *s = opaque;

    spi_update_irq(s);

    return 0;
}

static const VMStateDescription vmstate_sf32lb52x_spi = {
    .name = TYPE_SF32LB52X_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = spi_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_TIMER_PTR(shift_timer, SF32LB52XSPIState),
        VMSTATE_UINT32(top_ctrl, SF32LB52XSPIState),
        VMSTATE_UINT32(fifo_ctrl, SF32LB52XSPIState),
        VMSTATE_UINT32(inte, SF32LB52XSPIState),
        VMSTATE_UINT32(to, SF32LB52XSPIState),
        VMSTATE_UINT32(clk_ctrl, SF32LB52XSPIState),
        VMSTATE_UINT32(triwire_ctrl, SF32LB52XSPIState),
        VMSTATE_UINT32(rwot_ctrl, SF32LB52XSPIState),
        VMSTATE_UINT32(rwot_ccm, SF32LB52XSPIState),
        VMSTATE_UINT32(rwot_cvwrn, SF32LB52XSPIState),
        VMSTATE_UINT32(status_flags, SF32LB52XSPIState),
        VMSTATE_UINT32_ARRAY(tx_fifo, SF32LB52XSPIState,
                             SF32LB52X_SPI_FIFO_DEPTH),
        VMSTATE_UINT32_ARRAY(rx_fifo, SF32LB52XSPIState,
                             SF32LB52X_SPI_FIFO_DEPTH),
        VMSTATE_UINT8(tx_head, SF32LB52XSPIState),
        VMSTATE_UINT8(rx_head, SF32LB52XSPIState),
        VMSTATE_UINT8(tx_count, SF32LB52XSPIState),
        VMSTATE_UINT8(rx_count, SF32LB52XSPIState),
        VMSTATE_BOOL(busy, SF32LB52XSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = spi_reset_hold;
    dc->realize = spi_realize;
    dc->desc = "SiFli SF32LB52X SPI controller";
    dc->vmsd = &vmstate_sf32lb52x_spi;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_spi_types[] = {
    {
        .name = TYPE_SF32LB52X_SPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XSPIState),
        .instance_finalize = spi_finalize,
        .class_init = spi_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_spi_types)
