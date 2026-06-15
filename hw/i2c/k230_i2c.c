/*
 * K230 DesignWare I2C controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/i2c/k230_i2c.h"
#include "migration/vmstate.h"

#define K230_I2C_CON             0x00
#define K230_I2C_TAR             0x04
#define K230_I2C_SAR             0x08
#define K230_I2C_DATA_CMD        0x10
#define K230_I2C_SS_SCL_HCNT     0x14
#define K230_I2C_SS_SCL_LCNT     0x18
#define K230_I2C_FS_SCL_HCNT     0x1c
#define K230_I2C_FS_SCL_LCNT     0x20
#define K230_I2C_HS_SCL_HCNT     0x24
#define K230_I2C_HS_SCL_LCNT     0x28
#define K230_I2C_INTR_STAT       0x2c
#define K230_I2C_INTR_MASK       0x30
#define K230_I2C_RAW_INTR_STAT   0x34
#define K230_I2C_RX_TL           0x38
#define K230_I2C_TX_TL           0x3c
#define K230_I2C_CLR_INTR        0x40
#define K230_I2C_CLR_RX_UNDER    0x44
#define K230_I2C_CLR_RX_OVER     0x48
#define K230_I2C_CLR_TX_OVER     0x4c
#define K230_I2C_CLR_RD_REQ      0x50
#define K230_I2C_CLR_TX_ABRT     0x54
#define K230_I2C_CLR_RX_DONE     0x58
#define K230_I2C_CLR_ACTIVITY    0x5c
#define K230_I2C_CLR_STOP_DET    0x60
#define K230_I2C_CLR_START_DET   0x64
#define K230_I2C_CLR_GEN_CALL    0x68
#define K230_I2C_ENABLE          0x6c
#define K230_I2C_STATUS          0x70
#define K230_I2C_TXFLR           0x74
#define K230_I2C_RXFLR           0x78
#define K230_I2C_SDA_HOLD        0x7c
#define K230_I2C_TX_ABRT_SOURCE  0x80
#define K230_I2C_DMA_CR          0x88
#define K230_I2C_DMA_TDLR        0x8c
#define K230_I2C_DMA_RDLR        0x90
#define K230_I2C_ENABLE_STATUS   0x9c
#define K230_I2C_START           0xa0
#define K230_I2C_CLR_RESTART_DET 0xa8
#define K230_I2C_COMP_PARAM_1    0xf4
#define K230_I2C_COMP_VERSION    0xf8
#define K230_I2C_COMP_TYPE       0xfc

#define K230_I2C_INTR_RX_UNDER   BIT(0)
#define K230_I2C_INTR_RX_OVER    BIT(1)
#define K230_I2C_INTR_RX_FULL    BIT(2)
#define K230_I2C_INTR_TX_OVER    BIT(3)
#define K230_I2C_INTR_TX_EMPTY   BIT(4)
#define K230_I2C_INTR_RD_REQ     BIT(5)
#define K230_I2C_INTR_TX_ABRT    BIT(6)
#define K230_I2C_INTR_RX_DONE    BIT(7)
#define K230_I2C_INTR_ACTIVITY   BIT(8)
#define K230_I2C_INTR_STOP_DET   BIT(9)
#define K230_I2C_INTR_START_DET  BIT(10)
#define K230_I2C_INTR_GEN_CALL   BIT(11)
#define K230_I2C_INTR_RESTART_DET BIT(12)

#define K230_I2C_STATUS_ACTIVITY BIT(0)
#define K230_I2C_STATUS_TFNF     BIT(1)
#define K230_I2C_STATUS_TFE      BIT(2)
#define K230_I2C_STATUS_RFNE     BIT(3)
#define K230_I2C_STATUS_MST_ACTIVITY BIT(5)

#define K230_I2C_CMD_READ        BIT(8)
#define K230_I2C_CMD_STOP        BIT(9)
#define K230_I2C_CMD_RESTART     BIT(10)

#define K230_I2C_ABRT_7B_ADDR_NOACK BIT(0)
#define K230_I2C_ABRT_TXDATA_NOACK  BIT(3)
#define K230_I2C_COMP_TYPE_VALUE    0x44570140
#define K230_I2C_COMP_VERSION_VALUE 0x3131312a

static uint32_t k230_i2c_reg(K230I2CState *s, hwaddr addr)
{
    return s->regs[addr / 4];
}

static void k230_i2c_set_reg(K230I2CState *s, hwaddr addr, uint32_t value)
{
    s->regs[addr / 4] = value;
}

static bool k230_i2c_enabled(K230I2CState *s)
{
    return k230_i2c_reg(s, K230_I2C_ENABLE) & 1;
}

static uint32_t k230_i2c_raw_intr(K230I2CState *s)
{
    uint32_t raw = k230_i2c_reg(s, K230_I2C_RAW_INTR_STAT);

    if (k230_i2c_enabled(s)) {
        raw |= K230_I2C_INTR_TX_EMPTY;
    }

    if (s->rx_len) {
        raw |= K230_I2C_INTR_RX_FULL;
    } else {
        raw &= ~K230_I2C_INTR_RX_FULL;
    }

    if (s->started) {
        raw |= K230_I2C_INTR_ACTIVITY;
    } else {
        raw &= ~K230_I2C_INTR_ACTIVITY;
    }

    return raw;
}

static void k230_i2c_update_irq(K230I2CState *s)
{
    uint32_t raw = k230_i2c_raw_intr(s);
    uint32_t stat = raw & k230_i2c_reg(s, K230_I2C_INTR_MASK);

    qemu_set_irq(s->irq, stat ? 1 : 0);
}

static void k230_i2c_raise(K230I2CState *s, uint32_t mask)
{
    k230_i2c_set_reg(s, K230_I2C_RAW_INTR_STAT,
                     k230_i2c_reg(s, K230_I2C_RAW_INTR_STAT) | mask);
    k230_i2c_update_irq(s);
}

static void k230_i2c_clear(K230I2CState *s, uint32_t mask)
{
    k230_i2c_set_reg(s, K230_I2C_RAW_INTR_STAT,
                     k230_i2c_reg(s, K230_I2C_RAW_INTR_STAT) & ~mask);
    k230_i2c_update_irq(s);
}

static void k230_i2c_finish(K230I2CState *s)
{
    if (s->started) {
        i2c_end_transfer(s->bus);
        s->started = false;
    }
}

static void k230_i2c_abort(K230I2CState *s, uint32_t source)
{
    k230_i2c_set_reg(s, K230_I2C_TX_ABRT_SOURCE, source);
    k230_i2c_finish(s);
    k230_i2c_raise(s, K230_I2C_INTR_TX_ABRT | K230_I2C_INTR_STOP_DET);
}

static bool k230_i2c_start(K230I2CState *s, bool recv, bool restart)
{
    uint8_t address = k230_i2c_reg(s, K230_I2C_TAR) & 0x7f;

    if (s->started && (!restart && s->recv == recv && s->address == address)) {
        return true;
    }

    k230_i2c_finish(s);
    if (i2c_start_transfer(s->bus, address, recv)) {
        k230_i2c_abort(s, K230_I2C_ABRT_7B_ADDR_NOACK);
        return false;
    }

    s->started = true;
    s->recv = recv;
    s->address = address;
    k230_i2c_raise(s, K230_I2C_INTR_START_DET);
    return true;
}

static void k230_i2c_rx_push(K230I2CState *s, uint8_t value)
{
    if (s->rx_len >= K230_I2C_FIFO_DEPTH) {
        k230_i2c_raise(s, K230_I2C_INTR_RX_OVER);
        return;
    }

    s->rx_fifo[(s->rx_pos + s->rx_len) % K230_I2C_FIFO_DEPTH] = value;
    s->rx_len++;
    k230_i2c_update_irq(s);
}

static uint8_t k230_i2c_rx_pop(K230I2CState *s)
{
    uint8_t value;

    if (!s->rx_len) {
        k230_i2c_raise(s, K230_I2C_INTR_RX_UNDER);
        return 0xff;
    }

    value = s->rx_fifo[s->rx_pos];
    s->rx_pos = (s->rx_pos + 1) % K230_I2C_FIFO_DEPTH;
    s->rx_len--;
    k230_i2c_update_irq(s);

    return value;
}

static void k230_i2c_data_write(K230I2CState *s, uint32_t value)
{
    bool recv = value & K230_I2C_CMD_READ;
    bool restart = value & K230_I2C_CMD_RESTART;
    bool stop = value & K230_I2C_CMD_STOP;

    if (!k230_i2c_enabled(s)) {
        k230_i2c_abort(s, BIT(11));
        return;
    }

    if (!k230_i2c_start(s, recv, restart)) {
        return;
    }

    if (recv) {
        k230_i2c_rx_push(s, i2c_recv(s->bus));
    } else if (i2c_send(s->bus, value & 0xff)) {
        k230_i2c_abort(s, K230_I2C_ABRT_TXDATA_NOACK);
        return;
    }

    if (stop) {
        k230_i2c_finish(s);
        k230_i2c_raise(s, K230_I2C_INTR_STOP_DET);
    }
}

static uint64_t k230_i2c_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230I2CState *s = K230_I2C(opaque);
    uint32_t raw;

    if (addr >= K230_I2C_SIZE || size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_I2C, addr);
        return 0;
    }

    switch (addr) {
    case K230_I2C_DATA_CMD:
        return k230_i2c_rx_pop(s);
    case K230_I2C_INTR_STAT:
        raw = k230_i2c_raw_intr(s);
        return raw & k230_i2c_reg(s, K230_I2C_INTR_MASK);
    case K230_I2C_RAW_INTR_STAT:
        return k230_i2c_raw_intr(s);
    case K230_I2C_CLR_INTR:
        k230_i2c_clear(s, (uint32_t)~K230_I2C_INTR_RX_FULL);
        return 1;
    case K230_I2C_CLR_RX_UNDER:
        k230_i2c_clear(s, K230_I2C_INTR_RX_UNDER);
        return 1;
    case K230_I2C_CLR_RX_OVER:
        k230_i2c_clear(s, K230_I2C_INTR_RX_OVER);
        return 1;
    case K230_I2C_CLR_TX_OVER:
        k230_i2c_clear(s, K230_I2C_INTR_TX_OVER);
        return 1;
    case K230_I2C_CLR_RD_REQ:
        k230_i2c_clear(s, K230_I2C_INTR_RD_REQ);
        return 1;
    case K230_I2C_CLR_TX_ABRT:
        k230_i2c_clear(s, K230_I2C_INTR_TX_ABRT);
        k230_i2c_set_reg(s, K230_I2C_TX_ABRT_SOURCE, 0);
        return 1;
    case K230_I2C_CLR_RX_DONE:
        k230_i2c_clear(s, K230_I2C_INTR_RX_DONE);
        return 1;
    case K230_I2C_CLR_ACTIVITY:
        k230_i2c_clear(s, K230_I2C_INTR_ACTIVITY);
        return 1;
    case K230_I2C_CLR_STOP_DET:
        k230_i2c_clear(s, K230_I2C_INTR_STOP_DET);
        return 1;
    case K230_I2C_CLR_START_DET:
        k230_i2c_clear(s, K230_I2C_INTR_START_DET);
        return 1;
    case K230_I2C_CLR_GEN_CALL:
        k230_i2c_clear(s, K230_I2C_INTR_GEN_CALL);
        return 1;
    case K230_I2C_CLR_RESTART_DET:
        k230_i2c_clear(s, K230_I2C_INTR_RESTART_DET);
        return 1;
    case K230_I2C_ENABLE_STATUS:
        return k230_i2c_enabled(s) ? 1 : 0;
    case K230_I2C_STATUS:
        return K230_I2C_STATUS_TFNF | K230_I2C_STATUS_TFE |
               (s->rx_len ? K230_I2C_STATUS_RFNE : 0) |
               (s->started ? K230_I2C_STATUS_ACTIVITY |
                             K230_I2C_STATUS_MST_ACTIVITY : 0);
    case K230_I2C_TXFLR:
        return 0;
    case K230_I2C_RXFLR:
        return s->rx_len;
    case K230_I2C_COMP_PARAM_1:
        return (K230_I2C_FIFO_DEPTH - 1) << 16 |
               (K230_I2C_FIFO_DEPTH - 1) << 8 |
               0xc;
    case K230_I2C_COMP_VERSION:
        return K230_I2C_COMP_VERSION_VALUE;
    case K230_I2C_COMP_TYPE:
        return K230_I2C_COMP_TYPE_VALUE;
    default:
        return k230_i2c_reg(s, addr);
    }
}

static void k230_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    K230I2CState *s = K230_I2C(opaque);
    uint32_t value = val;

    if (addr >= K230_I2C_SIZE || size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_I2C, addr);
        return;
    }

    switch (addr) {
    case K230_I2C_DATA_CMD:
        k230_i2c_data_write(s, value);
        break;
    case K230_I2C_INTR_MASK:
        k230_i2c_set_reg(s, addr, value);
        k230_i2c_update_irq(s);
        break;
    case K230_I2C_ENABLE:
        k230_i2c_set_reg(s, addr, value & 1);
        if (!(value & 1)) {
            k230_i2c_finish(s);
            s->rx_pos = 0;
            s->rx_len = 0;
        }
        k230_i2c_update_irq(s);
        break;
    case K230_I2C_RX_TL:
    case K230_I2C_TX_TL:
        k230_i2c_set_reg(s, addr, MIN(value, K230_I2C_FIFO_DEPTH - 1));
        break;
    case K230_I2C_START:
        if (value & 1) {
            k230_i2c_set_reg(s, K230_I2C_ENABLE, 1);
        }
        k230_i2c_update_irq(s);
        break;
    default:
        k230_i2c_set_reg(s, addr, value);
        break;
    }
}

static const MemoryRegionOps k230_i2c_ops = {
    .read = k230_i2c_read,
    .write = k230_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void k230_i2c_reset(DeviceState *dev)
{
    K230I2CState *s = K230_I2C(dev);

    k230_i2c_finish(s);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    s->rx_pos = 0;
    s->rx_len = 0;
    s->address = 0;
    s->started = false;
    s->recv = false;
    k230_i2c_set_reg(s, K230_I2C_CON, 0x65);
    k230_i2c_set_reg(s, K230_I2C_SAR, 0x55);
    k230_i2c_set_reg(s, K230_I2C_SDA_HOLD, 1 << 16);
    k230_i2c_update_irq(s);
}

static const VMStateDescription vmstate_k230_i2c = {
    .name = TYPE_K230_I2C,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, K230I2CState, K230_I2C_REG_COUNT),
        VMSTATE_UINT8_ARRAY(rx_fifo, K230I2CState, K230_I2C_FIFO_DEPTH),
        VMSTATE_UINT8(rx_pos, K230I2CState),
        VMSTATE_UINT8(rx_len, K230I2CState),
        VMSTATE_UINT8(address, K230I2CState),
        VMSTATE_BOOL(started, K230I2CState),
        VMSTATE_BOOL(recv, K230I2CState),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_i2c_realize(DeviceState *dev, Error **errp)
{
    K230I2CState *s = K230_I2C(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_i2c_ops, s,
                          TYPE_K230_I2C, K230_I2C_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(dev, "i2c");
}

static void k230_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = k230_i2c_realize;
    device_class_set_legacy_reset(dc, k230_i2c_reset);
    dc->vmsd = &vmstate_k230_i2c;
    dc->desc = "K230 DesignWare I2C controller";
}

static const TypeInfo k230_i2c_info = {
    .name = TYPE_K230_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230I2CState),
    .class_init = k230_i2c_class_init,
};

static void k230_i2c_register_types(void)
{
    type_register_static(&k230_i2c_info);
}

type_init(k230_i2c_register_types)
