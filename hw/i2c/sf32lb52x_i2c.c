/*
 * SiFli SF32LB52X I2C controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the master-mode register interface used by the Zephyr
 * drivers/i2c/i2c_sf32lb.c driver: START/address/RW phase, DBR data bytes,
 * STOP, SR status/W1C bits, IER interrupt enables and the DBR/FIFO data
 * paths.  Register resets and bit positions follow the SF32LB52x SVD I2C1
 * block and the vendor HAL cmsis/sf32lb52x/i2c.h definitions; the driver
 * sequence (address phase -> TE/RF/MSD handshake) follows i2c_sf32lb.c.
 *
 * Manual MMIO is deliberate: the bank is dominated by the transfer state
 * machine and side-effect bits, so a RegisterInfo bank would still need the
 * same hooks next to it.
 *
 * Unimplemented: slave mode, HS-mode timing, SCL clock generation, DMA
 * request handshake (DMAEN/TXREQ/RXREQ are stored but data moves through
 * DBR/FIFO; the DMAC model performs MMIO-to-MMIO transfers), bus-reset
 * cycle timing, and pin-level BMR monitoring.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/sf32lb52x_i2c.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

enum {
    R_CR = 0x00,
    R_TCR = 0x04,
    R_IER = 0x08,
    R_SR = 0x0c,
    R_DBR = 0x10,
    R_SAR = 0x14,
    R_LCR = 0x18,
    R_WCR = 0x1c,
    R_RCCR = 0x20,
    R_BMR = 0x24,
    R_DNR = 0x28,
    R_RSVD1 = 0x2c,
    R_FIFO = 0x30,
};

/* CR */
#define CR_MODE_MASK    (0x3U << 0)
#define CR_IUE          (1U << 2)
#define CR_SCLE         (1U << 3)
#define CR_DMAEN        (1U << 4)
#define CR_LASTNACK     (1U << 5)
#define CR_LASTSTOP     (1U << 6)
#define CR_GCD          (1U << 7)
#define CR_MSDE         (1U << 8)
#define CR_SCLPP        (1U << 9)
#define CR_SDAGCD       (1U << 10)
#define CR_SLVEN        (1U << 11)
#define CR_DNF_MASK     (0x7U << 12)
#define CR_BRGRST       (1U << 29)
#define CR_RSTREQ       (1U << 30)
#define CR_UR           (1U << 31)
#define CR_STORE_MASK   (CR_MODE_MASK | CR_IUE | CR_SCLE | CR_DMAEN |      \
                         CR_LASTNACK | CR_LASTSTOP | CR_GCD | CR_MSDE |     \
                         CR_SCLPP | CR_SDAGCD | CR_SLVEN | CR_DNF_MASK)

/* TCR */
#define TCR_TB          (1U << 0)
#define TCR_START       (1U << 1)
#define TCR_STOP        (1U << 2)
#define TCR_NACK        (1U << 3)
#define TCR_MA          (1U << 4)
#define TCR_STORE_MASK  (TCR_STOP | TCR_NACK | TCR_MA)

/* IER (bits 4..15 used by the model; positions match SR flags) */
#define IER_STORE_MASK  0xffffU

/*
 * SR flags; positions match IER where an interrupt is defined.  Bits that
 * are derived from bus state (RWM/UB/IBB) are not stored in sr_flags.
 */
#define SR_RWM          (1U << 0)
#define SR_NACK         (1U << 1)
#define SR_UB           (1U << 2)
#define SR_IBB          (1U << 3)
#define SR_SSD          (1U << 4)
#define SR_ALD          (1U << 5)
#define SR_TE           (1U << 6)
#define SR_RF           (1U << 7)
#define SR_GCAD         (1U << 8)
#define SR_SAD          (1U << 9)
#define SR_BED          (1U << 10)
#define SR_EBB          (1U << 11)
#define SR_MSD          (1U << 12)
#define SR_DMADONE      (1U << 13)
#define SR_OF           (1U << 14)
#define SR_UF           (1U << 15)
/*
 * W1C flags.  NACK is deliberately not W1C: the driver writes SR back
 * (clearing TE/RF/MSD event bits) and then tests NACK to report -EIO; it is
 * cleared by the next START instead.
 */
#define SR_W1C_MASK     (SR_SSD | SR_ALD | SR_TE | SR_RF | SR_SAD | SR_BED | \
                         SR_MSD | SR_DMADONE | SR_OF | SR_UF)

/* SAR/LCR/WCR/RCCR/BMR/DNR */
#define SAR_MASK        (0x7fU << 0)
#define LCR_MASK        ((0x1ffU << 0) | (0x1ffU << 9) | (0x1ffU << 18) | \
                         (0x1fU << 27))
#define WCR_MASK        ((0xffU << 0) | (0x1fU << 8) | (0x1fU << 13) |   \
                         (0x3U << 28))
#define RCCR_MASK       (0xfU << 0)
#define BMR_MASK        (0x3U << 0) /* SDA/SCL pin level, idle = both high */
#define DNR_MASK        (0x1ffU << 0)
#define DBR_MASK        0xffU

static const VMStateDescription vmstate_sf32lb52x_i2c;

static void sf32lb52x_i2c_update_irq(SF32LB52XI2CState *s)
{
    qemu_set_irq(s->irq, (s->sr_flags & s->ier & 0xffffU) != 0);
}

static uint32_t sf32lb52x_i2c_sr_value(SF32LB52XI2CState *s)
{
    uint32_t value = s->sr_flags;

    if (s->transfer_active) {
        value |= SR_UB;
        if (s->is_recv) {
            value |= SR_RWM;
        }
    } else if (s->bus && i2c_bus_busy(s->bus)) {
        value |= SR_IBB;
    }

    return value;
}

static void sf32lb52x_i2c_fifo_reset(SF32LB52XI2CState *s)
{
    s->fifo_head = 0;
    s->fifo_count = 0;
}

static void sf32lb52x_i2c_fifo_push(SF32LB52XI2CState *s, uint8_t value)
{
    if (s->fifo_count == SF32LB52X_I2C_FIFO_DEPTH) {
        s->sr_flags |= SR_OF;
        return;
    }
    s->fifo[(s->fifo_head + s->fifo_count) % SF32LB52X_I2C_FIFO_DEPTH] = value;
    s->fifo_count++;
}

static uint8_t sf32lb52x_i2c_fifo_pop(SF32LB52XI2CState *s)
{
    uint8_t value;

    if (s->fifo_count == 0) {
        s->sr_flags |= SR_UF;
        return 0;
    }
    value = s->fifo[s->fifo_head];
    s->fifo_head = (s->fifo_head + 1) % SF32LB52X_I2C_FIFO_DEPTH;
    s->fifo_count--;
    return value;
}

static void sf32lb52x_i2c_unit_reset(SF32LB52XI2CState *s)
{
    if (s->transfer_active && s->bus) {
        i2c_end_transfer(s->bus);
    }
    s->transfer_active = false;
    s->is_recv = false;

    s->cr = 0;
    s->tcr = 0;
    s->ier = 0;
    s->sr_flags = 0;
    s->sar = 0x47;
    s->lcr = 0x081c72ed;
    s->wcr = 0x0a;
    s->rccr = 0x09;
    s->bmr = 0x03;
    s->dnr = 0;
    s->dbr_tx = 0;
    s->dbr_rx = 0;
    sf32lb52x_i2c_fifo_reset(s);
    sf32lb52x_i2c_update_irq(s);
}

/*
 * A STOP condition (or a transfer error) ends the bus transaction.  MSD is
 * reported only when CR.MSDE enables it, matching the driver, which sets
 * MSDE before every transfer and waits for MSD after the last byte.
 */
static void sf32lb52x_i2c_finish_transfer(SF32LB52XI2CState *s)
{
    if (!s->transfer_active) {
        return;
    }
    i2c_end_transfer(s->bus);
    s->transfer_active = false;
    s->is_recv = false;
    if (s->cr & CR_MSDE) {
        s->sr_flags |= SR_MSD;
    }
}

static void sf32lb52x_i2c_address_phase(SF32LB52XI2CState *s)
{
    uint8_t data = s->dbr_tx;
    int ret;

    /* A new START clears the previous ACK/NACK status. */
    s->sr_flags &= ~SR_NACK;
    s->is_recv = (data & 1) != 0;
    ret = i2c_start_transfer(s->bus, data >> 1, s->is_recv);
    s->sr_flags |= SR_TE;
    if (ret) {
        s->transfer_active = false;
        s->sr_flags |= SR_NACK;
        if (s->cr & CR_MSDE) {
            s->sr_flags |= SR_MSD;
        }
    } else {
        s->transfer_active = true;
    }
}

static void sf32lb52x_i2c_data_phase(SF32LB52XI2CState *s, uint32_t tcr)
{
    if (!s->transfer_active) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: TCR.TB without an active transfer\n", __func__);
        return;
    }

    if (!s->is_recv) {
        if (i2c_send(s->bus, s->dbr_tx)) {
            s->sr_flags |= SR_TE | SR_NACK;
            i2c_end_transfer(s->bus);
            s->transfer_active = false;
            if (s->cr & CR_MSDE) {
                s->sr_flags |= SR_MSD;
            }
            return;
        }
        s->sr_flags |= SR_TE;
        if (tcr & TCR_STOP) {
            sf32lb52x_i2c_finish_transfer(s);
        }
    } else {
        s->dbr_rx = i2c_recv(s->bus);
        s->sr_flags |= SR_RF;
        if (tcr & (TCR_NACK | TCR_STOP)) {
            sf32lb52x_i2c_finish_transfer(s);
        }
    }
}

static uint64_t sf32lb52x_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    SF32LB52XI2CState *s = opaque;
    uint64_t value;

    switch (addr) {
    case R_CR:
        return s->cr;
    case R_TCR:
        return s->tcr;
    case R_IER:
        return s->ier;
    case R_SR:
        return sf32lb52x_i2c_sr_value(s);
    case R_DBR:
        return s->dbr_rx;
    case R_SAR:
        return s->sar;
    case R_LCR:
        return s->lcr;
    case R_WCR:
        return s->wcr;
    case R_RCCR:
        return s->rccr;
    case R_BMR:
        /* Pin-level monitor: the modelled bus is idle, both lines high. */
        return 0x3;
    case R_DNR:
        return s->dnr;
    case R_RSVD1:
        qemu_log_mask(LOG_UNIMP, "%s: read of reserved register 0x%x\n",
                      __func__, (unsigned)addr);
        return 0;
    case R_FIFO:
        value = sf32lb52x_i2c_fifo_pop(s);
        sf32lb52x_i2c_update_irq(s);
        return value;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void sf32lb52x_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    SF32LB52XI2CState *s = opaque;

    switch (addr) {
    case R_CR:
        if (value & CR_UR) {
            sf32lb52x_i2c_unit_reset(s);
            return;
        }
        /* BRGRST/RSTREQ are single-pulse (self-clearing) and not stored. */
        s->cr = value & CR_STORE_MASK;
        break;
    case R_TCR: {
        uint32_t tcr = value & TCR_STORE_MASK;

        s->tcr = tcr;
        if (value & TCR_TB) {
            if (!(s->cr & CR_IUE)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: transfer with I2C unit disabled\n",
                              __func__);
                break;
            }
            if (value & TCR_START) {
                sf32lb52x_i2c_address_phase(s);
                if (s->transfer_active && (tcr & TCR_STOP)) {
                    sf32lb52x_i2c_finish_transfer(s);
                }
            } else {
                sf32lb52x_i2c_data_phase(s, tcr);
            }
        }
        break;
    }
    case R_IER:
        s->ier = value & IER_STORE_MASK;
        break;
    case R_SR:
        s->sr_flags &= ~(value & SR_W1C_MASK);
        break;
    case R_DBR:
        s->dbr_tx = value & DBR_MASK;
        break;
    case R_SAR:
        s->sar = value & SAR_MASK;
        break;
    case R_LCR:
        s->lcr = value & LCR_MASK;
        break;
    case R_WCR:
        s->wcr = value & WCR_MASK;
        break;
    case R_RCCR:
        s->rccr = value & RCCR_MASK;
        break;
    case R_BMR:
        /* Pin levels are inputs; software writes have no effect. */
        qemu_log_mask(LOG_UNIMP, "%s: BMR is a pin monitor; write ignored\n",
                      __func__);
        break;
    case R_DNR:
        s->dnr = value & DNR_MASK;
        break;
    case R_RSVD1:
        qemu_log_mask(LOG_UNIMP, "%s: write to reserved register 0x%x\n",
                      __func__, (unsigned)addr);
        break;
    case R_FIFO:
        sf32lb52x_i2c_fifo_push(s, value & DBR_MASK);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return;
    }

    sf32lb52x_i2c_update_irq(s);
}

static const MemoryRegionOps sf32lb52x_i2c_ops = {
    .read = sf32lb52x_i2c_read,
    .write = sf32lb52x_i2c_write,
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

static void sf32lb52x_i2c_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XI2CState *s = SF32LB52X_I2C(obj);

    sf32lb52x_i2c_unit_reset(s);
}

static void sf32lb52x_i2c_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XI2CState *s = SF32LB52X_I2C(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_i2c_ops, s,
                          TYPE_SF32LB52X_I2C, SF32LB52X_I2C_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /*
     * The controller creates the bus; tests and boards can attach I2C
     * slaves to "i2c" (e.g. -device i2c-echo,address=0x42,bus=i2c).
     */
    s->bus = i2c_init_bus(dev, "i2c");
}

static const VMStateDescription vmstate_sf32lb52x_i2c = {
    .name = TYPE_SF32LB52X_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cr, SF32LB52XI2CState),
        VMSTATE_UINT32(tcr, SF32LB52XI2CState),
        VMSTATE_UINT32(ier, SF32LB52XI2CState),
        VMSTATE_UINT32(sr_flags, SF32LB52XI2CState),
        VMSTATE_UINT32(sar, SF32LB52XI2CState),
        VMSTATE_UINT32(lcr, SF32LB52XI2CState),
        VMSTATE_UINT32(wcr, SF32LB52XI2CState),
        VMSTATE_UINT32(rccr, SF32LB52XI2CState),
        VMSTATE_UINT32(bmr, SF32LB52XI2CState),
        VMSTATE_UINT32(dnr, SF32LB52XI2CState),
        VMSTATE_UINT8(dbr_tx, SF32LB52XI2CState),
        VMSTATE_UINT8(dbr_rx, SF32LB52XI2CState),
        VMSTATE_BOOL(transfer_active, SF32LB52XI2CState),
        VMSTATE_BOOL(is_recv, SF32LB52XI2CState),
        VMSTATE_UINT8_ARRAY(fifo, SF32LB52XI2CState,
                            SF32LB52X_I2C_FIFO_DEPTH),
        VMSTATE_UINT32(fifo_head, SF32LB52XI2CState),
        VMSTATE_UINT32(fifo_count, SF32LB52XI2CState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_i2c_reset_hold;
    dc->realize = sf32lb52x_i2c_realize;
    dc->desc = "SiFli SF32LB52X I2C controller";
    dc->vmsd = &vmstate_sf32lb52x_i2c;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_i2c_types[] = {
    {
        .name = TYPE_SF32LB52X_I2C,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XI2CState),
        .class_init = sf32lb52x_i2c_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_i2c_types)
