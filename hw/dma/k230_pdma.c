/*
 * K230 PDMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "hw/core/irq.h"
#include "hw/dma/k230_pdma.h"

#define K230_PDMA_CHANNELS 8
#define K230_PDMA_DONE_INT BIT(0)
#define K230_PDMA_ITEM_INT BIT(8)
#define K230_PDMA_PAUSE_INT BIT(16)
#define K230_PDMA_TIMEOUT_INT BIT(24)
#define K230_PDMA_IRQ_MASK_CHANNEL \
    (K230_PDMA_DONE_INT | K230_PDMA_ITEM_INT | K230_PDMA_PAUSE_INT | \
     K230_PDMA_TIMEOUT_INT)
#define K230_PDMA_LLI_LIMIT 1024

REG32(PDMA_CH_EN, 0x000)
REG32(PDMA_INT_MASK, 0x004)
REG32(PDMA_INT_STAT, 0x008)

#define K230_PDMA_CH_BASE(n) (0x020 + (n) * 0x20)
#define K230_PDMA_CH_CTL(n) (K230_PDMA_CH_BASE(n) + 0x00)
#define K230_PDMA_CH_STAT(n) (K230_PDMA_CH_BASE(n) + 0x04)
#define K230_PDMA_CH_CFG(n) (K230_PDMA_CH_BASE(n) + 0x08)
#define K230_PDMA_CH_LLT_SADDR(n) (K230_PDMA_CH_BASE(n) + 0x0c)
#define K230_PDMA_DEV_SEL(n) (0x120 + (n) * 4)
#define K230_PDMA_R(offset) ((offset) / sizeof(uint32_t))

#define K230_PDMA_CH_CTL_START 1
#define K230_PDMA_CH_CTL_STOP 2
#define K230_PDMA_CH_CTL_RESUME 4
#define K230_PDMA_CH_STAT_BUSY BIT(0)
#define K230_PDMA_CH_STAT_PAUSE BIT(1)
#define K230_PDMA_CH_CFG_SRC_TYPE BIT(0)
#define K230_PDMA_CH_CFG_DEV_HSIZE_SHIFT 1
#define K230_PDMA_CH_CFG_DEV_HSIZE_LENGTH 2
#define K230_PDMA_LLI_LINE_SIZE_MASK MAKE_64BIT_MASK(0, 30)

static unsigned int k230_pdma_channel_from_addr(hwaddr addr)
{
    return (addr - K230_PDMA_CH_BASE(0)) / 0x20;
}


static uint32_t k230_pdma_channel_done(unsigned int channel)
{
    return K230_PDMA_DONE_INT << channel;
}

static uint32_t k230_pdma_channel_timeout(unsigned int channel)
{
    return K230_PDMA_TIMEOUT_INT << channel;
}
static uint32_t k230_pdma_enabled_irq_mask(const K230PdmaState *s)
{
    uint32_t enabled = s->regs[R_PDMA_CH_EN];
    uint32_t mask = 0;

    for (unsigned int channel = 0; channel < K230_PDMA_CHANNELS; channel++) {
        if (enabled & BIT(channel)) {
            mask |= k230_pdma_channel_done(channel);
            mask |= K230_PDMA_ITEM_INT << channel;
            mask |= K230_PDMA_PAUSE_INT << channel;
            mask |= k230_pdma_channel_timeout(channel);
        }
    }

    return mask;
}

static bool k230_pdma_is_modeled_addr(hwaddr addr)
{
    if (addr == A_PDMA_CH_EN || addr == A_PDMA_INT_MASK ||
        addr == A_PDMA_INT_STAT) {
        return true;
    }
    if (addr >= K230_PDMA_CH_BASE(0) &&
        addr < K230_PDMA_CH_BASE(K230_PDMA_CHANNELS) &&
        ((addr - K230_PDMA_CH_BASE(0)) % 0x20) <= 0x0c &&
        (((addr - K230_PDMA_CH_BASE(0)) % 4) == 0)) {
        return true;
    }
    if (addr >= K230_PDMA_DEV_SEL(0) &&
        addr < K230_PDMA_DEV_SEL(K230_PDMA_CHANNELS) &&
        ((addr - K230_PDMA_DEV_SEL(0)) % 4) == 0) {
        return true;
    }

    return false;
}

static uint64_t k230_pdma_read_bytes(const uint8_t *regs, hwaddr addr,
                                     unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_pdma_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                  unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static void k230_pdma_sync_legacy_reg(K230PdmaState *s, hwaddr addr)
{
    stl_le_p(&s->legacy_regs[addr], s->regs[K230_PDMA_R(addr)]);
}

static uint64_t k230_pdma_mmio_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    K230PdmaState *s = K230_PDMA(opaque);

    if (size == 4 && addr < K230_PDMA_SIZE && k230_pdma_is_modeled_addr(addr)) {
        return register_read_memory(s->reg_array, addr, size);
    }

    return k230_pdma_read_bytes(s->legacy_regs, addr, size);
}

static void k230_pdma_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned int size)
{
    K230PdmaState *s = K230_PDMA(opaque);

    if (size == 4 && addr < K230_PDMA_SIZE && k230_pdma_is_modeled_addr(addr)) {
        register_write_memory(s->reg_array, addr, val, size);
        k230_pdma_sync_legacy_reg(s, addr);
        return;
    }

    k230_pdma_write_bytes(s->legacy_regs, addr, val, size);
}



static void k230_pdma_update_irq(K230PdmaState *s)
{
    uint32_t pending;
    bool level;

    pending = s->regs[R_PDMA_INT_STAT] & k230_pdma_enabled_irq_mask(s) &
              ~s->regs[R_PDMA_INT_MASK];
    level = pending != 0;
    if (s->irq_level != level) {
        s->irq_level = level;
        qemu_set_irq(s->irq, level);
    }
}

static bool k230_pdma_read32(hwaddr addr, uint32_t *value)
{
    uint32_t raw;

    if (dma_memory_read(&address_space_memory, addr, &raw, sizeof(raw),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }

    *value = ldl_le_p(&raw);
    return true;
}

static bool k230_pdma_copy_tx(hwaddr src, hwaddr dst, uint32_t len,
                              unsigned int width)
{
    uint8_t buf[4];
    uint32_t offset = 0;

    while (offset < len) {
        unsigned int size = MIN(width, len - offset);

        if (dma_memory_read(&address_space_memory, src + offset, buf, size,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
            dma_memory_write(&address_space_memory, dst, buf, size,
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return false;
        }
        offset += size;
    }

    return true;
}

static bool k230_pdma_copy_rx(hwaddr src, hwaddr dst, uint32_t len,
                              unsigned int width)
{
    uint8_t buf[4];
    uint32_t offset = 0;

    while (offset < len) {
        unsigned int size = MIN(width, len - offset);

        if (dma_memory_read(&address_space_memory, src, buf, size,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
            dma_memory_write(&address_space_memory, dst + offset, buf, size,
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return false;
        }
        offset += size;
    }

    return true;
}

static unsigned int k230_pdma_channel_width(uint32_t cfg)
{
    unsigned int hsize = extract32(cfg, K230_PDMA_CH_CFG_DEV_HSIZE_SHIFT,
                                   K230_PDMA_CH_CFG_DEV_HSIZE_LENGTH);

    if (hsize > 2) {
        return sizeof(uint32_t);
    }

    return 1u << hsize;
}

static bool k230_pdma_run_lli(K230PdmaState *s, unsigned int channel,
                              hwaddr desc_addr)
{
    uint32_t cfg = s->regs[K230_PDMA_R(K230_PDMA_CH_CFG(channel))];
    unsigned int width = k230_pdma_channel_width(cfg);
    bool rx = cfg & K230_PDMA_CH_CFG_SRC_TYPE;

    for (int i = 0; desc_addr && i < K230_PDMA_LLI_LIMIT; i++) {
        uint32_t control;
        uint32_t src;
        uint32_t dst;
        uint32_t next;
        uint32_t len;
        bool ok;

        if (!k230_pdma_read32(desc_addr, &control) ||
            !k230_pdma_read32(desc_addr + 4, &src) ||
            !k230_pdma_read32(desc_addr + 8, &dst) ||
            !k230_pdma_read32(desc_addr + 12, &next)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: failed to read channel %u descriptor @0x%"
                          HWADDR_PRIx "\n",
                          TYPE_K230_PDMA, channel, desc_addr);
            return false;
        }

        len = control & K230_PDMA_LLI_LINE_SIZE_MASK;
        if (len) {
            if (rx) {
                ok = k230_pdma_copy_rx(src, dst, len, width);
            } else {
                ok = k230_pdma_copy_tx(src, dst, len, width);
            }
            if (!ok) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: failed channel %u transfer src=0x%08x dst=0x%08x len=0x%x\n",
                              TYPE_K230_PDMA, channel, src, dst, len);
                return false;
            }
        }

        desc_addr = next;
    }

    if (desc_addr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: channel %u descriptor chain did not terminate\n",
                      TYPE_K230_PDMA, channel);
        return false;
    }

    return true;
}

static void k230_pdma_start_channel(K230PdmaState *s, unsigned int channel)
{
    hwaddr desc_addr;
    bool ok;

    if (channel >= K230_PDMA_CHANNELS) {
        return;
    }

    desc_addr = s->regs[K230_PDMA_R(K230_PDMA_CH_LLT_SADDR(channel))];
    s->regs[K230_PDMA_R(K230_PDMA_CH_STAT(channel))] = K230_PDMA_CH_STAT_BUSY;
    ok = k230_pdma_run_lli(s, channel, desc_addr);
    s->regs[K230_PDMA_R(K230_PDMA_CH_STAT(channel))] = 0;

    if (ok) {
        s->regs[R_PDMA_INT_STAT] |= k230_pdma_channel_done(channel);
    } else {
        s->regs[R_PDMA_INT_STAT] |= k230_pdma_channel_timeout(channel);
    }
    k230_pdma_update_irq(s);
}

static void k230_pdma_int_stat_post_write(RegisterInfo *reg, uint64_t val)
{
    k230_pdma_update_irq(K230_PDMA(reg->opaque));
}

static void k230_pdma_ch_en_post_write(RegisterInfo *reg, uint64_t val)
{
    k230_pdma_update_irq(K230_PDMA(reg->opaque));
}

static void k230_pdma_ch_ctl_post_write(RegisterInfo *reg, uint64_t val)
{
    K230PdmaState *s = K230_PDMA(reg->opaque);
    unsigned int channel = k230_pdma_channel_from_addr(reg->access->addr);

    if (val & K230_PDMA_CH_CTL_STOP) {
        s->regs[K230_PDMA_R(K230_PDMA_CH_STAT(channel))] = 0;
        return;
    }
    if (val & K230_PDMA_CH_CTL_RESUME) {
        s->regs[K230_PDMA_R(K230_PDMA_CH_STAT(channel))] &=
            ~K230_PDMA_CH_STAT_PAUSE;
    }
    if (val & K230_PDMA_CH_CTL_START) {
        k230_pdma_start_channel(s, channel);
    }
}

#define K230_PDMA_CH_CTL_REG(n) \
    { .name = "CH" #n "_CTL", .addr = K230_PDMA_CH_CTL(n), \
      .post_write = k230_pdma_ch_ctl_post_write }
#define K230_PDMA_CH_STAT_REG(n) \
    { .name = "CH" #n "_STAT", .addr = K230_PDMA_CH_STAT(n), .ro = ~0ull }
#define K230_PDMA_CH_REG(n, reg) \
    { .name = "CH" #n "_" #reg, .addr = K230_PDMA_CH_ ## reg(n) }

#define K230_PDMA_DEV_SEL_REG(n) \
    { .name = "DEV_SEL" #n, .addr = K230_PDMA_DEV_SEL(n) }

static const RegisterAccessInfo k230_pdma_regs_info[] = {
    { .name = "PDMA_CH_EN", .addr = A_PDMA_CH_EN,
      .rsvd = ~MAKE_64BIT_MASK(0, K230_PDMA_CHANNELS),
      .post_write = k230_pdma_ch_en_post_write },
    { .name = "PDMA_INT_MASK", .addr = A_PDMA_INT_MASK },
    { .name = "PDMA_INT_STAT", .addr = A_PDMA_INT_STAT,
      .w1c = UINT32_MAX, .post_write = k230_pdma_int_stat_post_write },
    K230_PDMA_CH_CTL_REG(0),
    K230_PDMA_CH_STAT_REG(0),
    K230_PDMA_CH_REG(0, CFG),
    K230_PDMA_CH_REG(0, LLT_SADDR),
    K230_PDMA_CH_CTL_REG(1),
    K230_PDMA_CH_STAT_REG(1),
    K230_PDMA_CH_REG(1, CFG),
    K230_PDMA_CH_REG(1, LLT_SADDR),
    K230_PDMA_CH_CTL_REG(2),
    K230_PDMA_CH_STAT_REG(2),
    K230_PDMA_CH_REG(2, CFG),
    K230_PDMA_CH_REG(2, LLT_SADDR),
    K230_PDMA_CH_CTL_REG(3),
    K230_PDMA_CH_STAT_REG(3),
    K230_PDMA_CH_REG(3, CFG),
    K230_PDMA_CH_REG(3, LLT_SADDR),
    K230_PDMA_CH_CTL_REG(4),
    K230_PDMA_CH_STAT_REG(4),
    K230_PDMA_CH_REG(4, CFG),
    K230_PDMA_CH_REG(4, LLT_SADDR),
    K230_PDMA_CH_CTL_REG(5),
    K230_PDMA_CH_STAT_REG(5),
    K230_PDMA_CH_REG(5, CFG),
    K230_PDMA_CH_REG(5, LLT_SADDR),
    K230_PDMA_CH_CTL_REG(6),
    K230_PDMA_CH_STAT_REG(6),
    K230_PDMA_CH_REG(6, CFG),
    K230_PDMA_CH_REG(6, LLT_SADDR),
    K230_PDMA_CH_CTL_REG(7),
    K230_PDMA_CH_STAT_REG(7),
    K230_PDMA_CH_REG(7, CFG),
    K230_PDMA_CH_REG(7, LLT_SADDR),
    K230_PDMA_DEV_SEL_REG(0),
    K230_PDMA_DEV_SEL_REG(1),
    K230_PDMA_DEV_SEL_REG(2),
    K230_PDMA_DEV_SEL_REG(3),
    K230_PDMA_DEV_SEL_REG(4),
    K230_PDMA_DEV_SEL_REG(5),
    K230_PDMA_DEV_SEL_REG(6),
    K230_PDMA_DEV_SEL_REG(7),
};

static const MemoryRegionOps k230_pdma_reg_array_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps k230_pdma_ops = {
    .read = k230_pdma_mmio_read,
    .write = k230_pdma_mmio_write,
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

static void k230_pdma_reset(DeviceState *dev)
{
    K230PdmaState *s = K230_PDMA(dev);

    memset(s->legacy_regs, 0, sizeof(s->legacy_regs));
    for (int i = 0; i < ARRAY_SIZE(k230_pdma_regs_info); i++) {
        register_reset(&s->regs_info[k230_pdma_regs_info[i].addr / 4]);
        k230_pdma_sync_legacy_reg(s, k230_pdma_regs_info[i].addr);
    }
    s->irq_level = false;
    qemu_set_irq(s->irq, 0);
}

static int k230_pdma_post_load(void *opaque, int version_id)
{
    K230PdmaState *s = K230_PDMA(opaque);
    if (version_id == 1) {
        memset(s->regs, 0, sizeof(s->regs));
        for (unsigned int i = 0; i < K230_PDMA_R_MAX; i++) {
            s->regs[i] = ldl_le_p(&s->legacy_regs[i * sizeof(uint32_t)]);
        }
    }
    for (unsigned int i = 0; i < K230_PDMA_R_MAX; i++) {
        k230_pdma_sync_legacy_reg(s, i * sizeof(uint32_t));
    }

    uint32_t pending;

    pending = s->regs[R_PDMA_INT_STAT] & k230_pdma_enabled_irq_mask(s) &
              ~s->regs[R_PDMA_INT_MASK];
    s->irq_level = pending != 0;
    qemu_set_irq(s->irq, s->irq_level);

    return 0;
}

static const VMStateDescription vmstate_k230_pdma = {
    .name = TYPE_K230_PDMA,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = k230_pdma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(legacy_regs, K230PdmaState,
                            K230_PDMA_LEGACY_SIZE),
        VMSTATE_BOOL_V(irq_level, K230PdmaState, 2),
        VMSTATE_UINT32_ARRAY_V(regs, K230PdmaState, K230_PDMA_R_MAX, 2),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_pdma_realize(DeviceState *dev, Error **errp)
{
    K230PdmaState *s = K230_PDMA(dev);
    s->reg_array = register_init_block32(dev, k230_pdma_regs_info,
                                         ARRAY_SIZE(k230_pdma_regs_info),
                                         s->regs_info, s->regs,
                                         &k230_pdma_reg_array_ops,
                                         false, K230_PDMA_SIZE);
    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_pdma_ops, s,
                          TYPE_K230_PDMA, K230_PDMA_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void k230_pdma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_pdma_realize;
    device_class_set_legacy_reset(dc, k230_pdma_reset);
    dc->vmsd = &vmstate_k230_pdma;
    dc->desc = "K230 PDMA controller";
}

static const TypeInfo k230_pdma_type_info = {
    .name = TYPE_K230_PDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230PdmaState),
    .class_init = k230_pdma_class_init,
};

static void k230_register_pdma_types(void)
{
    type_register_static(&k230_pdma_type_info);
}

type_init(k230_register_pdma_types)
