/*
 * K230 security register block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_security.h"
#include "system/dma.h"

#define K230_DMA_STAT_0         0x0010
#define K230_DMA_START          0x0020
#define K230_DMA_DSC_CFG_0      0x0034
#define K230_DMA_DSC_CFG_1      0x0038
#define K230_DMA_DSC_CFG_2      0x003c

#define K230_CRYPTO_DGST_OUT    0x01c0

#define K230_GCM_BASE           0x0200
#define K230_GCM_STAT           0x0070

#define K230_KWP_BASE           0x0300
#define K230_KWP_STATUS         0x0010
#define K230_KWP_START          0x0014
#define K230_KWP_CONFIG         0x0018

#define K230_HASH_BASE          0x0800
#define K230_HASH_STATUS        0x0010
#define K230_HASH_PLEN          0x0020
#define K230_HASH_ALEN          0x0030

#define K230_KA_BASE            0x0c00
#define K230_KA_SK_FREE         0x0010
#define K230_KA_SK_0            0x0020

#define K230_RSA_BASE           0x1000
#define K230_RSA_CTRL           0x0008
#define K230_RSA_STATUS         0x000c

#define K230_TRNG_BASE          0x3000
#define K230_TRNG_DATA          0x02a0
#define K230_OTP_BASE           0x3500

#define K230_DMA_MAX_COPY       (4 * MiB)

static uint64_t k230_security_read_bytes(uint8_t *regs, hwaddr addr,
                                         unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_security_write_bytes(uint8_t *regs, hwaddr addr,
                                      uint64_t val, unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static bool k230_security_range_ok(hwaddr addr, unsigned int size)
{
    return addr <= K230_SECURITY_SIZE && size <= K230_SECURITY_SIZE - addr;
}

static bool k230_otp_range(hwaddr addr, unsigned int size)
{
    hwaddr end = addr + size;

    return addr >= K230_OTP_BASE && end <= K230_OTP_BASE + K230_OTP_SIZE;
}

static uint32_t k230_security_readl_regs(K230SecurityState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_security_writel_regs(K230SecurityState *s, hwaddr addr,
                                      uint32_t val)
{
    stl_le_p(s->regs + addr, val);
}

static void k230_security_zero_digest(K230SecurityState *s)
{
    memset(s->regs + K230_CRYPTO_DGST_OUT, 0, 64);
}

static void k230_security_dma_complete(K230SecurityState *s)
{
    uint32_t src = k230_security_readl_regs(s, K230_DMA_DSC_CFG_0);
    uint32_t dst = k230_security_readl_regs(s, K230_DMA_DSC_CFG_1);
    uint32_t len = k230_security_readl_regs(s, K230_DMA_DSC_CFG_2);
    uint32_t plen = k230_security_readl_regs(s, K230_HASH_BASE +
                                             K230_HASH_PLEN);

    if (src && dst && len && len <= K230_DMA_MAX_COPY) {
        g_autofree uint8_t *buf = g_malloc(len);

        if (dma_memory_read(&address_space_memory, src, buf, len,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
            dma_memory_write(&address_space_memory, dst, buf, len,
                             MEMTXATTRS_UNSPECIFIED);
        }
    }

    k230_security_writel_regs(s, K230_DMA_STAT_0, 0);
    k230_security_writel_regs(s, K230_GCM_BASE + K230_GCM_STAT, 0);
    k230_security_writel_regs(s, K230_HASH_BASE + K230_HASH_STATUS, 0);
    k230_security_writel_regs(s, K230_HASH_BASE + K230_HASH_ALEN, plen + len);
    k230_security_zero_digest(s);
}

static uint32_t k230_security_ka_metadata(uint32_t keybits, uint32_t tag)
{
    return 1 | (keybits << 4) | (tag << 16);
}

static void k230_security_ka_set_slot(K230SecurityState *s, uint32_t slot,
                                      uint32_t keybits)
{
    uint32_t tag;
    uint32_t addr;

    if (slot >= 16 || !keybits) {
        return;
    }

    addr = K230_KA_BASE + K230_KA_SK_0 + slot * 4;
    if (keybits <= 128) {
        tag = 0x30 + slot;
        k230_security_writel_regs(s, addr,
                                  k230_security_ka_metadata(keybits, tag));
    } else if (keybits <= 256) {
        tag = 0x50 + slot / 2;
        k230_security_writel_regs(s, addr,
                                  k230_security_ka_metadata(keybits, tag));
        k230_security_writel_regs(s, addr + 4, tag << 16);
    } else {
        tag = 0x60 + slot / 4;
        k230_security_writel_regs(s, addr,
                                  k230_security_ka_metadata(keybits, tag));
        for (int i = 1; i < 4; i++) {
            k230_security_writel_regs(s, addr + i * 4, tag << 16);
        }
    }
}

static void k230_security_ka_free(K230SecurityState *s, uint32_t mask)
{
    for (int i = 0; i < 16; i++) {
        if (mask & BIT(i)) {
            k230_security_writel_regs(s, K230_KA_BASE + K230_KA_SK_0 + i * 4,
                                      0);
        }
    }
}

static void k230_security_kwp_start(K230SecurityState *s)
{
    uint32_t config = k230_security_readl_regs(s, K230_KWP_BASE +
                                               K230_KWP_CONFIG);
    uint32_t keybits = (config >> 8) & 0x7ff;
    uint32_t slot = (config >> 20) & 0xf;

    k230_security_ka_set_slot(s, slot, keybits);
    k230_security_writel_regs(s, K230_KWP_BASE + K230_KWP_STATUS, 0);
}

static void k230_security_write_side_effect(K230SecurityState *s, hwaddr addr,
                                            uint64_t val, unsigned int size)
{
    if (size != 4) {
        return;
    }

    switch (addr) {
    case K230_DMA_START:
        if (val & 1) {
            k230_security_dma_complete(s);
        }
        break;
    case K230_KWP_BASE + K230_KWP_START:
        if (val & 1) {
            k230_security_kwp_start(s);
        }
        break;
    case K230_KA_BASE + K230_KA_SK_FREE:
        k230_security_ka_free(s, val);
        break;
    case K230_RSA_BASE + K230_RSA_CTRL:
        if (val & 1) {
            k230_security_writel_regs(s, K230_RSA_BASE + K230_RSA_STATUS, 0);
        }
        break;
    default:
        break;
    }
}

static uint64_t k230_security_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    K230SecurityState *s = K230_SECURITY(opaque);

    if (!k230_security_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_SECURITY, addr);
        return 0;
    }

    if (addr == K230_TRNG_BASE + K230_TRNG_DATA && size == 4) {
        uint32_t value;

        qemu_guest_getrandom_nofail(&value, sizeof(value));
        return value;
    }

    if (k230_otp_range(addr, size)) {
        return k230_security_read_bytes(s->otp, addr - K230_OTP_BASE, size);
    }

    return k230_security_read_bytes(s->regs, addr, size);
}

static void k230_security_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned int size)
{
    K230SecurityState *s = K230_SECURITY(opaque);

    if (!k230_security_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_SECURITY, addr);
        return;
    }

    if (k230_otp_range(addr, size)) {
        return;
    }

    k230_security_write_bytes(s->regs, addr, val, size);
    k230_security_write_side_effect(s, addr, val, size);
}

static const MemoryRegionOps k230_security_ops = {
    .read = k230_security_read,
    .write = k230_security_write,
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

static void k230_security_reset(DeviceState *dev)
{
    K230SecurityState *s = K230_SECURITY(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->otp, 0, sizeof(s->otp));
}

static const VMStateDescription vmstate_k230_security = {
    .name = TYPE_K230_SECURITY,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230SecurityState, K230_SECURITY_SIZE),
        VMSTATE_UINT8_ARRAY(otp, K230SecurityState, K230_OTP_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_security_realize(DeviceState *dev, Error **errp)
{
    K230SecurityState *s = K230_SECURITY(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_security_ops, s,
                          TYPE_K230_SECURITY, K230_SECURITY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_security_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_security_realize;
    device_class_set_legacy_reset(dc, k230_security_reset);
    dc->vmsd = &vmstate_k230_security;
    dc->desc = "K230 security registers";
}

static const TypeInfo k230_security_type_info = {
    .name = TYPE_K230_SECURITY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SecurityState),
    .class_init = k230_security_class_init,
};

static void k230_security_register_types(void)
{
    type_register_static(&k230_security_type_info);
}

type_init(k230_security_register_types)
