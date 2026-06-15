/*
 * K230 GSDMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/dma/k230_gsdma.h"

#define K230_GSDMA_INT_STAT      0x08
#define K230_GSDMA_CH_BASE       0x50
#define K230_GSDMA_CH_STRIDE     0x30
#define K230_GSDMA_CH_CTL        0x00
#define K230_GSDMA_CH_STATUS     0x04
#define K230_GSDMA_CH_LLT_SADDR  0x10
#define K230_GSDMA_CH_COUNT      4
#define K230_GSDMA_CH_BUSY       BIT(0)
#define K230_GSDMA_UGZIP_WR_DONE 0x222

static uint64_t k230_gsdma_read_bytes(uint8_t *regs, hwaddr addr,
                                      unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_gsdma_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                   unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_gsdma_reg_read32(K230GsdmaState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_gsdma_reg_write32(K230GsdmaState *s, hwaddr addr,
                                   uint32_t val)
{
    stl_le_p(s->regs + addr, val);
}

static hwaddr k230_gsdma_ch_addr(unsigned int ch, hwaddr reg)
{
    return K230_GSDMA_CH_BASE + ch * K230_GSDMA_CH_STRIDE + reg;
}

static void k230_gsdma_update_irq(K230GsdmaState *s)
{
    qemu_set_irq(s->irq, k230_gsdma_reg_read32(s, K230_GSDMA_INT_STAT) != 0);
}

uint32_t k230_gsdma_get_llt_saddr(K230GsdmaState *s, unsigned int ch)
{
    assert(ch < K230_GSDMA_CH_COUNT);

    return k230_gsdma_reg_read32(s,
        k230_gsdma_ch_addr(ch, K230_GSDMA_CH_LLT_SADDR));
}

void k230_gsdma_ugzip_complete(K230GsdmaState *s)
{
    k230_gsdma_reg_write32(s,
        k230_gsdma_ch_addr(K230_GSDMA_UGZIP_RD_CH, K230_GSDMA_CH_STATUS), 0);
    k230_gsdma_reg_write32(s,
        k230_gsdma_ch_addr(K230_GSDMA_UGZIP_WR_CH, K230_GSDMA_CH_STATUS), 0);
    k230_gsdma_reg_write32(s, K230_GSDMA_INT_STAT,
                           k230_gsdma_reg_read32(s, K230_GSDMA_INT_STAT) |
                           K230_GSDMA_UGZIP_WR_DONE);
    k230_gsdma_update_irq(s);
}

static uint64_t k230_gsdma_read(void *opaque, hwaddr addr, unsigned int size)
{
    return k230_gsdma_read_bytes(K230_GSDMA(opaque)->regs, addr, size);
}

static void k230_gsdma_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned int size)
{
    K230GsdmaState *s = K230_GSDMA(opaque);

    if (addr == K230_GSDMA_INT_STAT && size == 4) {
        uint32_t int_stat = k230_gsdma_reg_read32(s, K230_GSDMA_INT_STAT);

        k230_gsdma_reg_write32(s, K230_GSDMA_INT_STAT,
                               int_stat & ~(uint32_t)val);
        k230_gsdma_update_irq(s);
        return;
    }

    k230_gsdma_write_bytes(s->regs, addr, val, size);

    if (size == 4) {
        for (int ch = 0; ch < K230_GSDMA_CH_COUNT; ch++) {
            hwaddr ctl = k230_gsdma_ch_addr(ch, K230_GSDMA_CH_CTL);

            if (addr == ctl) {
                hwaddr status = k230_gsdma_ch_addr(ch, K230_GSDMA_CH_STATUS);

                if (val & BIT(1)) {
                    k230_gsdma_reg_write32(s, status, 0);
                } else if (val & BIT(0)) {
                    k230_gsdma_reg_write32(s, status, 0);
                    k230_gsdma_reg_write32(s, K230_GSDMA_INT_STAT,
                                           k230_gsdma_reg_read32(s,
                                               K230_GSDMA_INT_STAT) |
                                           BIT(ch));
                    k230_gsdma_update_irq(s);
                }
                break;
            }
        }
    }
}

static const MemoryRegionOps k230_gsdma_ops = {
    .read = k230_gsdma_read,
    .write = k230_gsdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_gsdma_reset(DeviceState *dev)
{
    K230GsdmaState *s = K230_GSDMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static const VMStateDescription vmstate_k230_gsdma = {
    .name = TYPE_K230_GSDMA,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230GsdmaState, K230_GSDMA_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_gsdma_realize(DeviceState *dev, Error **errp)
{
    K230GsdmaState *s = K230_GSDMA(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_gsdma_ops, s,
                          TYPE_K230_GSDMA, K230_GSDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void k230_gsdma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_gsdma_realize;
    device_class_set_legacy_reset(dc, k230_gsdma_reset);
    dc->vmsd = &vmstate_k230_gsdma;
    dc->desc = "K230 GSDMA controller";
}

static const TypeInfo k230_gsdma_type_info = {
    .name = TYPE_K230_GSDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230GsdmaState),
    .class_init = k230_gsdma_class_init,
};

static void k230_register_gsdma_types(void)
{
    type_register_static(&k230_gsdma_type_info);
}

type_init(k230_register_gsdma_types)
