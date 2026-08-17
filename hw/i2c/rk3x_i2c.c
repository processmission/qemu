/*
 * Rockchip rk3x I2C controller model
 *
 * Register contract follows drivers/i2c/busses/i2c-rk3x.c (Rockchip
 * 6.1 kernel, "rockchip,rk3588-i2c" / "rockchip,rk3399-i2c"):
 *
 *   0x000 CON       EN(0) MOD[2:1] START(3) STOP(4) LASTACK(5)
 *                   ACTACK(6) SDA/STA/STO_CFG[17:8] VERSION[24:16]
 *   0x004 CLKDIV
 *   0x008 MRXADDR   slave address, VALID(x) in bits 24 + x
 *   0x00c MRXRADDR  register address bytes for REGISTER_TX mode
 *   0x010 MTXCNT    transmit byte count (includes the address byte)
 *   0x014 MRXCNT    receive byte count
 *   0x018 IEN       BTF(0) BRF(1) MBTF(2) MBRF(3) START(4)
 *                   STOP(5) NAKRCV(6)
 *   0x01c IPD       same bit layout as IEN, write-1-to-clear
 *   0x020 FCNT      finished byte count
 *   0x024 SCL_OE_DB
 *   0x100 TXBUFFER  8 words
 *   0x200 RXBUFFER  8 words
 *   0x228 CON1      AUTO_STOP(0) TRANSFER_AUTO_STOP(1)
 *                   NACK_AUTO_STOP(2)
 *
 * The controller reports I2C version 5 (RK_I2C_VERSION5), which makes
 * the Linux driver enable its hardware auto-stop path.
 *
 * Transfers run synchronously when the driver writes the byte count
 * register (MTXCNT/MRXCNT) after arming CON with EN + START: the whole
 * bus transaction is issued against the attached I2CBus and the
 * resulting interrupt bits are posted to IPD, with the level IRQ
 * asserted while (IPD & IEN).
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/rk3x_i2c.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qom/object.h"

#define RK3X_I2C_CON       0x000
#define RK3X_I2C_CLKDIV    0x004
#define RK3X_I2C_MRXADDR   0x008
#define RK3X_I2C_MRXRADDR  0x00c
#define RK3X_I2C_MTXCNT    0x010
#define RK3X_I2C_MRXCNT    0x014
#define RK3X_I2C_IEN       0x018
#define RK3X_I2C_IPD       0x01c
#define RK3X_I2C_FCNT      0x020
#define RK3X_I2C_SCL_OE_DB 0x024
#define RK3X_I2C_TXBUFFER  0x100
#define RK3X_I2C_RXBUFFER  0x200
#define RK3X_I2C_CON1      0x228

#define RK3X_I2C_CON_EN       BIT(0)
#define RK3X_I2C_CON_MOD_MASK (BIT(1) | BIT(2))
#define RK3X_I2C_CON_MOD_TX   (0u << 1)
#define RK3X_I2C_CON_MOD_REGISTER_TX (1u << 1)
#define RK3X_I2C_CON_MOD_RX   (2u << 1)
#define RK3X_I2C_CON_MOD_REGISTER_RX (3u << 1)
#define RK3X_I2C_CON_START    BIT(3)
#define RK3X_I2C_CON_STOP     BIT(4)
#define RK3X_I2C_CON_LASTACK  BIT(5)
#define RK3X_I2C_CON_ACTACK   BIT(6)
#define RK3X_I2C_CON_VERSION_MASK (0x1ffu << 16)
#define RK3X_I2C_VERSION      5

#define RK3X_I2C_INT_BTF    BIT(0)
#define RK3X_I2C_INT_BRF    BIT(1)
#define RK3X_I2C_INT_MBTF   BIT(2)
#define RK3X_I2C_INT_MBRF   BIT(3)
#define RK3X_I2C_INT_START  BIT(4)
#define RK3X_I2C_INT_STOP   BIT(5)
#define RK3X_I2C_INT_NAKRCV BIT(6)

#define RK3X_I2C_CON1_AUTO_STOP        BIT(0)
#define RK3X_I2C_CON1_TRANSFER_AUTO_STOP BIT(1)
#define RK3X_I2C_CON1_NACK_AUTO_STOP   BIT(2)

#define RK3X_I2C_BUFFER_SIZE 32

typedef enum {
    RK3X_I2C_STATE_IDLE,
    RK3X_I2C_STATE_TX,
    RK3X_I2C_STATE_RX,
} Rk3xI2CStateMode;

struct Rk3xI2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t con;
    uint32_t clkdiv;
    uint32_t mrxaddr;
    uint32_t mrxraddr;
    uint32_t mtxcnt;
    uint32_t mrxcnt;
    uint32_t ien;
    uint32_t ipd;
    uint32_t fcnt;
    uint32_t scl_oe_db;
    uint32_t con1;

    uint8_t txbuf[RK3X_I2C_BUFFER_SIZE];
    uint8_t rxbuf[RK3X_I2C_BUFFER_SIZE];

    uint8_t state;
    bool addr_sent;
    bool xfer_active;
};

static void rk3x_i2c_update_irq(Rk3xI2CState *s)
{
    qemu_set_irq(s->irq, (s->ipd & s->ien) != 0);
}

static void rk3x_i2c_end_transfer(Rk3xI2CState *s)
{
    if (s->xfer_active) {
        i2c_end_transfer(s->bus);
        s->xfer_active = false;
    }
}

static void rk3x_i2c_post_interrupt(Rk3xI2CState *s, uint32_t bits)
{
    s->ipd |= bits;
    rk3x_i2c_update_irq(s);
}

static void rk3x_i2c_transfer_tx(Rk3xI2CState *s)
{
    uint8_t addr7;
    unsigned int n, i, skip;
    bool nak = false;

    /* The TX FIFO holds at most 32 bytes (including the address byte). */
    n = MIN(s->mtxcnt, RK3X_I2C_BUFFER_SIZE);
    if (n == 0) {
        return;
    }

    /*
     * The driver refills the FIFO in chunks and writes MTXCNT again for
     * each one, so keep the bus transaction open until software STOP or
     * hardware auto-stop: only the first chunk carries the address byte.
     */
    if (!s->xfer_active) {
        addr7 = s->txbuf[0] >> 1;
        if (i2c_start_transfer(s->bus, addr7, false)) {
            nak = true;
        } else {
            s->xfer_active = true;
        }
        skip = 1;
    } else {
        skip = 0;
    }

    if (!nak) {
        for (i = skip; i < n; i++) {
            if (i2c_send(s->bus, s->txbuf[i])) {
                nak = true;
                break;
            }
        }
    }
    s->fcnt = n;

    if (s->con1 & RK3X_I2C_CON1_TRANSFER_AUTO_STOP) {
        i2c_end_transfer(s->bus);
        s->xfer_active = false;
    }
    if (nak) {
        rk3x_i2c_post_interrupt(s, RK3X_I2C_INT_NAKRCV |
                               ((s->con1 & RK3X_I2C_CON1_NACK_AUTO_STOP) ?
                                RK3X_I2C_INT_STOP : 0));
    } else if (s->con1 & RK3X_I2C_CON1_TRANSFER_AUTO_STOP) {
        rk3x_i2c_post_interrupt(s, RK3X_I2C_INT_STOP);
    } else {
        rk3x_i2c_post_interrupt(s, RK3X_I2C_INT_MBTF);
    }
}

static void rk3x_i2c_transfer_rx(Rk3xI2CState *s)
{
    uint8_t addr7;
    unsigned int n, i;
    bool nak = false;

    /* The RX FIFO holds at most 32 bytes. */
    n = MIN(s->mrxcnt, RK3X_I2C_BUFFER_SIZE);
    if (n == 0) {
        return;
    }

    addr7 = (s->mrxaddr >> 1) & 0x7f;

    if (!s->addr_sent) {
        uint32_t valid = s->mrxraddr >> 24;
        unsigned int reg_len = 0;

        s->addr_sent = true;
        while (valid) {
            if (valid & 1) {
                reg_len++;
            }
            valid >>= 1;
        }

        if (reg_len) {
            /* Write the register address(es), then restart for the read. */
            if (i2c_start_transfer(s->bus, addr7, false)) {
                nak = true;
            } else {
                for (i = 0; i < reg_len; i++) {
                    if (i2c_send(s->bus, (s->mrxraddr >> (i * 8)) & 0xff)) {
                        nak = true;
                        break;
                    }
                }
                i2c_end_transfer(s->bus);
            }
        }
    }

    if (!nak) {
        if (i2c_start_transfer(s->bus, addr7, true)) {
            nak = true;
        } else {
            for (i = 0; i < n; i++) {
                s->rxbuf[i] = i2c_recv(s->bus);
            }
            i2c_end_transfer(s->bus);
        }
    }
    s->fcnt = n;

    if (nak) {
        rk3x_i2c_post_interrupt(s, RK3X_I2C_INT_NAKRCV |
                               ((s->con1 & RK3X_I2C_CON1_NACK_AUTO_STOP) ?
                                RK3X_I2C_INT_STOP : 0));
    } else if (s->con1 & RK3X_I2C_CON1_TRANSFER_AUTO_STOP) {
        rk3x_i2c_post_interrupt(s, RK3X_I2C_INT_STOP);
    } else {
        rk3x_i2c_post_interrupt(s, RK3X_I2C_INT_MBRF);
    }
}

static void rk3x_i2c_write_con(Rk3xI2CState *s, uint32_t val)
{
    s->con = val & ~RK3X_I2C_CON_VERSION_MASK;

    if (!(s->con & RK3X_I2C_CON_EN)) {
        /* Controller disabled. */
        s->state = RK3X_I2C_STATE_IDLE;
        s->addr_sent = false;
        rk3x_i2c_end_transfer(s);
    } else if (s->con & RK3X_I2C_CON_START) {
        /*
         * START pulse: arm a transfer; it runs once the byte count
         * register (MTXCNT or MRXCNT) is written.
         */
        if ((s->con & RK3X_I2C_CON_MOD_MASK) == RK3X_I2C_CON_MOD_TX) {
            s->state = RK3X_I2C_STATE_TX;
        } else {
            s->state = RK3X_I2C_STATE_RX;
        }
        s->addr_sent = false;
        rk3x_i2c_end_transfer(s);
    } else if ((s->con & RK3X_I2C_CON_STOP) &&
               s->state != RK3X_I2C_STATE_IDLE) {
        /* Software-generated STOP after a data interrupt. */
        s->state = RK3X_I2C_STATE_IDLE;
        rk3x_i2c_end_transfer(s);
        rk3x_i2c_post_interrupt(s, RK3X_I2C_INT_STOP);
    }
}

static void rk3x_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    Rk3xI2CState *s = opaque;
    uint32_t val = value;
    unsigned int i;

    if (offset >= RK3X_I2C_TXBUFFER &&
        offset < RK3X_I2C_TXBUFFER + RK3X_I2C_BUFFER_SIZE) {
        for (i = 0; i < 4; i++) {
            s->txbuf[offset - RK3X_I2C_TXBUFFER + i] = val >> (8 * i);
        }
        return;
    }
    if (offset >= RK3X_I2C_RXBUFFER &&
        offset < RK3X_I2C_RXBUFFER + RK3X_I2C_BUFFER_SIZE) {
        for (i = 0; i < 4; i++) {
            s->rxbuf[offset - RK3X_I2C_RXBUFFER + i] = val >> (8 * i);
        }
        return;
    }

    switch (offset) {
    case RK3X_I2C_CON:
        rk3x_i2c_write_con(s, val);
        return;
    case RK3X_I2C_CLKDIV:
        s->clkdiv = val;
        return;
    case RK3X_I2C_MRXADDR:
        s->mrxaddr = val;
        return;
    case RK3X_I2C_MRXRADDR:
        s->mrxraddr = val;
        return;
    case RK3X_I2C_MTXCNT:
        s->mtxcnt = val;
        if ((s->con & RK3X_I2C_CON_EN) && s->state == RK3X_I2C_STATE_TX) {
            rk3x_i2c_transfer_tx(s);
        }
        return;
    case RK3X_I2C_MRXCNT:
        s->mrxcnt = val;
        if ((s->con & RK3X_I2C_CON_EN) && s->state == RK3X_I2C_STATE_RX) {
            rk3x_i2c_transfer_rx(s);
        }
        return;
    case RK3X_I2C_IEN:
        s->ien = val;
        rk3x_i2c_update_irq(s);
        return;
    case RK3X_I2C_IPD:
        /* Write-1-to-clear pending bits. */
        s->ipd &= ~val;
        rk3x_i2c_update_irq(s);
        return;
    case RK3X_I2C_FCNT:
        s->fcnt = val;
        return;
    case RK3X_I2C_SCL_OE_DB:
        s->scl_oe_db = val;
        return;
    case RK3X_I2C_CON1:
        s->con1 = val;
        return;
    default:
        return;
    }
}

static uint64_t rk3x_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    Rk3xI2CState *s = opaque;
    uint32_t val = 0;
    unsigned int i;

    if (offset >= RK3X_I2C_TXBUFFER &&
        offset < RK3X_I2C_TXBUFFER + RK3X_I2C_BUFFER_SIZE) {
        for (i = 0; i < 4; i++) {
            val |= s->txbuf[offset - RK3X_I2C_TXBUFFER + i] << (8 * i);
        }
        return val;
    }
    if (offset >= RK3X_I2C_RXBUFFER &&
        offset < RK3X_I2C_RXBUFFER + RK3X_I2C_BUFFER_SIZE) {
        for (i = 0; i < 4; i++) {
            val |= s->rxbuf[offset - RK3X_I2C_RXBUFFER + i] << (8 * i);
        }
        return val;
    }

    switch (offset) {
    case RK3X_I2C_CON:
        return s->con | (RK3X_I2C_VERSION << 16);
    case RK3X_I2C_CLKDIV:
        return s->clkdiv;
    case RK3X_I2C_MRXADDR:
        return s->mrxaddr;
    case RK3X_I2C_MRXRADDR:
        return s->mrxraddr;
    case RK3X_I2C_MTXCNT:
        return s->mtxcnt;
    case RK3X_I2C_MRXCNT:
        return s->mrxcnt;
    case RK3X_I2C_IEN:
        return s->ien;
    case RK3X_I2C_IPD:
        return s->ipd;
    case RK3X_I2C_FCNT:
        return s->fcnt;
    case RK3X_I2C_SCL_OE_DB:
        return s->scl_oe_db;
    case RK3X_I2C_CON1:
        return s->con1;
    default:
        return 0;
    }
}

static const MemoryRegionOps rk3x_i2c_ops = {
    .read = rk3x_i2c_read,
    .write = rk3x_i2c_write,
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

static void rk3x_i2c_reset(DeviceState *dev)
{
    Rk3xI2CState *s = RK3X_I2C(dev);

    s->con = 0;
    s->clkdiv = 0;
    s->mrxaddr = 0;
    s->mrxraddr = 0;
    s->mtxcnt = 0;
    s->mrxcnt = 0;
    s->ien = 0;
    s->ipd = 0;
    s->fcnt = 0;
    s->scl_oe_db = 0;
    s->con1 = 0;
    memset(s->txbuf, 0, sizeof(s->txbuf));
    memset(s->rxbuf, 0, sizeof(s->rxbuf));
    s->state = RK3X_I2C_STATE_IDLE;
    s->addr_sent = false;
    s->xfer_active = false;
    rk3x_i2c_update_irq(s);
}

static void rk3x_i2c_realize(DeviceState *dev, Error **errp)
{
    Rk3xI2CState *s = RK3X_I2C(dev);

    s->bus = i2c_init_bus(dev, "i2c-bus");
    memory_region_init_io(&s->iomem, OBJECT(dev), &rk3x_i2c_ops, s,
                          TYPE_RK3X_I2C, RK3X_I2C_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static int rk3x_i2c_post_load(void *opaque, int version_id)
{
    Rk3xI2CState *s = opaque;

    /* Reassert the IRQ if pending bits are still enabled after migration. */
    rk3x_i2c_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_rk3x_i2c = {
    .name = "rk3x-i2c",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = rk3x_i2c_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, Rk3xI2CState),
        VMSTATE_UINT32(clkdiv, Rk3xI2CState),
        VMSTATE_UINT32(mrxaddr, Rk3xI2CState),
        VMSTATE_UINT32(mrxraddr, Rk3xI2CState),
        VMSTATE_UINT32(mtxcnt, Rk3xI2CState),
        VMSTATE_UINT32(mrxcnt, Rk3xI2CState),
        VMSTATE_UINT32(ien, Rk3xI2CState),
        VMSTATE_UINT32(ipd, Rk3xI2CState),
        VMSTATE_UINT32(fcnt, Rk3xI2CState),
        VMSTATE_UINT32(scl_oe_db, Rk3xI2CState),
        VMSTATE_UINT32(con1, Rk3xI2CState),
        VMSTATE_UINT8_ARRAY(txbuf, Rk3xI2CState, RK3X_I2C_BUFFER_SIZE),
        VMSTATE_UINT8_ARRAY(rxbuf, Rk3xI2CState, RK3X_I2C_BUFFER_SIZE),
        VMSTATE_UINT8_V(state, Rk3xI2CState, 2),
        VMSTATE_BOOL_V(addr_sent, Rk3xI2CState, 2),
        VMSTATE_BOOL_V(xfer_active, Rk3xI2CState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void rk3x_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rk3x_i2c_realize;
    device_class_set_legacy_reset(dc, rk3x_i2c_reset);
    dc->vmsd = &vmstate_rk3x_i2c;
}

static const TypeInfo rk3x_i2c_info = {
    .name = TYPE_RK3X_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Rk3xI2CState),
    .class_init = rk3x_i2c_class_init,
};

static void rk3x_i2c_register_types(void)
{
    type_register_static(&rk3x_i2c_info);
}

type_init(rk3x_i2c_register_types)
