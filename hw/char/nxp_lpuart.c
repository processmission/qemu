/*
 * NXP Low Power UART
 *
 * This is a small model for firmware and RTOS console smoke tests. It
 * implements the register subset used by Zephyr's MCUX LPUART driver when
 * it initializes a UART and transmits through poll_out().
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/nxp_lpuart.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

REG32(VERID, 0x00)
REG32(PARAM, 0x04)
REG32(GLOBAL, 0x08)
REG32(PINCFG, 0x0c)
REG32(BAUD, 0x10)
REG32(STAT, 0x14)
    FIELD(STAT, PF, 16, 1)
    FIELD(STAT, FE, 17, 1)
    FIELD(STAT, NF, 18, 1)
    FIELD(STAT, OR, 19, 1)
    FIELD(STAT, RDRF, 21, 1)
    FIELD(STAT, TC, 22, 1)
    FIELD(STAT, TDRE, 23, 1)
REG32(CTRL, 0x18)
    FIELD(CTRL, RE, 18, 1)
    FIELD(CTRL, TE, 19, 1)
    FIELD(CTRL, RIE, 21, 1)
    FIELD(CTRL, TCIE, 22, 1)
    FIELD(CTRL, TIE, 23, 1)
REG32(DATA, 0x1c)
    FIELD(DATA, DATA, 0, 8)
REG32(MATCH, 0x20)
REG32(MODIR, 0x24)
REG32(FIFO, 0x28)
    FIELD(FIFO, RXEMPT, 22, 1)
    FIELD(FIFO, TXEMPT, 23, 1)
REG32(WATER, 0x2c)

#define LPUART_SIZE 0x4000

#define R_STAT_ERROR_MASK \
    (R_STAT_OR_MASK | R_STAT_NF_MASK | R_STAT_FE_MASK | R_STAT_PF_MASK)
#define R_STAT_TX_READY_MASK \
    (R_STAT_TDRE_MASK | R_STAT_TC_MASK)
#define R_FIFO_EMPTY_MASK \
    (R_FIFO_TXEMPT_MASK | R_FIFO_RXEMPT_MASK)

static void nxp_lpuart_update_irq(NXPLPUARTState *s)
{
    uint32_t ctrl = s->regs[R_CTRL];
    uint32_t stat = s->regs[R_STAT];
    bool level = false;

    if ((ctrl & R_CTRL_TIE_MASK) && (stat & R_STAT_TDRE_MASK)) {
        level = true;
    }

    if ((ctrl & R_CTRL_TCIE_MASK) && (stat & R_STAT_TC_MASK)) {
        level = true;
    }

    if ((ctrl & R_CTRL_RIE_MASK) && (stat & R_STAT_RDRF_MASK)) {
        level = true;
    }

    qemu_set_irq(s->irq, level);
}

static void nxp_lpuart_stat_post_write(RegisterInfo *reg, uint64_t val)
{
    NXPLPUARTState *s = NXP_LPUART(reg->opaque);

    nxp_lpuart_update_irq(s);
}

static void nxp_lpuart_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    NXPLPUARTState *s = NXP_LPUART(reg->opaque);

    nxp_lpuart_update_irq(s);
}

static void nxp_lpuart_data_post_write(RegisterInfo *reg, uint64_t val)
{
    NXPLPUARTState *s = NXP_LPUART(reg->opaque);
    uint8_t ch = val & R_DATA_DATA_MASK;

    if (!device_is_in_reset(DEVICE(s)) &&
        qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
    }

    s->regs[R_STAT] |= R_STAT_TX_READY_MASK;
    nxp_lpuart_update_irq(s);
}

static uint64_t nxp_lpuart_data_post_read(RegisterInfo *reg, uint64_t val)
{
    NXPLPUARTState *s = NXP_LPUART(reg->opaque);

    s->regs[R_STAT] &= ~R_STAT_RDRF_MASK;
    nxp_lpuart_update_irq(s);

    return val;
}

static const RegisterAccessInfo nxp_lpuart_regs_info[] = {
    {   .name = "VERID",   .addr = A_VERID,
        .reset = 0x04010003,
        .ro = UINT32_MAX,
    },{ .name = "PARAM",   .addr = A_PARAM,
        .ro = UINT32_MAX,
    },{ .name = "GLOBAL",  .addr = A_GLOBAL,
    },{ .name = "PINCFG",  .addr = A_PINCFG,
    },{ .name = "BAUD",    .addr = A_BAUD,
        .reset = 0x0f000004,
    },{ .name = "STAT",    .addr = A_STAT,
        .reset = R_STAT_TX_READY_MASK,
        .ro = R_STAT_TX_READY_MASK | R_STAT_RDRF_MASK,
        .w1c = R_STAT_ERROR_MASK,
        .post_write = nxp_lpuart_stat_post_write,
    },{ .name = "CTRL",    .addr = A_CTRL,
        .post_write = nxp_lpuart_ctrl_post_write,
    },{ .name = "DATA",    .addr = A_DATA,
        .post_write = nxp_lpuart_data_post_write,
        .post_read = nxp_lpuart_data_post_read,
    },{ .name = "MATCH",   .addr = A_MATCH,
    },{ .name = "MODIR",   .addr = A_MODIR,
    },{ .name = "FIFO",    .addr = A_FIFO,
        .reset = R_FIFO_EMPTY_MASK,
        .ro = R_FIFO_EMPTY_MASK,
    },{ .name = "WATER",   .addr = A_WATER,
    }
};

static const MemoryRegionOps nxp_lpuart_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
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

static void nxp_lpuart_reset(DeviceState *dev)
{
    NXPLPUARTState *s = NXP_LPUART(dev);

    for (int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }

    nxp_lpuart_update_irq(s);
}

static void nxp_lpuart_init(Object *obj)
{
    NXPLPUARTState *s = NXP_LPUART(obj);
    DeviceState *dev = DEVICE(obj);

    s->reg_array = register_init_block32(dev, nxp_lpuart_regs_info,
                                         ARRAY_SIZE(nxp_lpuart_regs_info),
                                         s->regs_info, s->regs,
                                         &nxp_lpuart_ops, false,
                                         LPUART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_nxp_lpuart = {
    .name = TYPE_NXP_LPUART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, NXPLPUARTState, NXP_LPUART_R_MAX),
        VMSTATE_END_OF_LIST()
    }
};

static const Property nxp_lpuart_properties[] = {
    DEFINE_PROP_CHR("chardev", NXPLPUARTState, chr),
};

static void nxp_lpuart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, nxp_lpuart_reset);
    dc->vmsd = &vmstate_nxp_lpuart;
    device_class_set_props(dc, nxp_lpuart_properties);
}

static const TypeInfo nxp_lpuart_info = {
    .name = TYPE_NXP_LPUART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NXPLPUARTState),
    .instance_init = nxp_lpuart_init,
    .class_init = nxp_lpuart_class_init,
};

static void nxp_lpuart_register_types(void)
{
    type_register_static(&nxp_lpuart_info);
}

type_init(nxp_lpuart_register_types)
