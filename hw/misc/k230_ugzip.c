/*
 * K230 UGZIP decompressor
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "hw/misc/k230_ugzip.h"
#include <zlib.h>

#define K230_UGZIP_START      0x00
#define K230_UGZIP_SRC_SIZE   0x04
#define K230_UGZIP_OUT_SIZE   0x08
#define K230_UGZIP_INTSTAT    0x0c
#define K230_UGZIP_SRC_VALID  BIT(31)
#define K230_UGZIP_DONE       BIT(10)

#define K230_SDMA_LLT_SIZE      24
#define K230_SDMA_LLT_SRC_ADDR  4
#define K230_SDMA_LLT_LINE_SIZE 8
#define K230_SDMA_LLT_DST_ADDR  16
#define K230_SDMA_LLT_NEXT_ADDR 20

static uint64_t k230_ugzip_read_bytes(uint8_t *regs, hwaddr addr,
                                      unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_ugzip_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                   unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_ugzip_reg_read32(K230UgzipState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_ugzip_reg_write32(K230UgzipState *s, hwaddr addr,
                                   uint32_t val)
{
    stl_le_p(s->regs + addr, val);
}

static bool k230_dma_read(hwaddr addr, void *buf, size_t len)
{
    return dma_memory_read(&address_space_memory, addr, buf, len,
                           MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static bool k230_dma_write(hwaddr addr, const void *buf, size_t len)
{
    return dma_memory_write(&address_space_memory, addr, buf, len,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static bool k230_ugzip_read_src_llt(uint32_t llt_addr, uint8_t *buf,
                                    size_t len)
{
    uint8_t desc[K230_SDMA_LLT_SIZE];
    size_t done = 0;

    for (int i = 0; llt_addr && done < len && i < 1024; i++) {
        uint32_t src_addr;
        uint32_t line_size;
        size_t copy_len;

        if (!k230_dma_read(llt_addr, desc, sizeof(desc))) {
            return false;
        }

        src_addr = ldl_le_p(desc + K230_SDMA_LLT_SRC_ADDR);
        line_size = ldl_le_p(desc + K230_SDMA_LLT_LINE_SIZE);
        copy_len = MIN((size_t)line_size, len - done);
        if (!copy_len || !k230_dma_read(src_addr, buf + done, copy_len)) {
            return false;
        }

        done += copy_len;
        llt_addr = ldl_le_p(desc + K230_SDMA_LLT_NEXT_ADDR);
    }

    return done == len;
}

static bool k230_ugzip_write_dst_llt(uint32_t llt_addr, const uint8_t *buf,
                                     size_t len)
{
    uint8_t desc[K230_SDMA_LLT_SIZE];
    size_t done = 0;

    for (int i = 0; llt_addr && done < len && i < 1024; i++) {
        uint32_t dst_addr;
        uint32_t line_size;
        size_t copy_len;

        if (!k230_dma_read(llt_addr, desc, sizeof(desc))) {
            return false;
        }

        dst_addr = ldl_le_p(desc + K230_SDMA_LLT_DST_ADDR);
        line_size = ldl_le_p(desc + K230_SDMA_LLT_LINE_SIZE);
        copy_len = MIN((size_t)line_size, len - done);
        if (!copy_len || !k230_dma_write(dst_addr, buf + done, copy_len)) {
            return false;
        }

        done += copy_len;
        llt_addr = ldl_le_p(desc + K230_SDMA_LLT_NEXT_ADDR);
    }

    return done == len;
}

static int k230_ugzip_inflate(uint8_t *dst, size_t *dst_len,
                              uint8_t *src, size_t src_len)
{
    z_stream strm = { 0 };
    int ret;

    if (src_len >= 3 && src[0] == 0x1f && src[1] == 0x8b && src[2] == 0x09) {
        src[2] = 0x08;
    }

    ret = inflateInit2(&strm, 16 + MAX_WBITS);
    if (ret != Z_OK) {
        return ret;
    }

    strm.next_in = src;
    strm.avail_in = src_len;
    strm.next_out = dst;
    strm.avail_out = *dst_len;

    ret = inflate(&strm, Z_FINISH);
    *dst_len -= strm.avail_out;
    inflateEnd(&strm);

    return ret == Z_STREAM_END ? Z_OK : ret;
}

static void k230_ugzip_start(K230UgzipState *s)
{
    uint32_t src_len = k230_ugzip_reg_read32(s, K230_UGZIP_SRC_SIZE) &
                       ~K230_UGZIP_SRC_VALID;
    uint32_t out_limit = k230_ugzip_reg_read32(s, K230_UGZIP_OUT_SIZE);
    uint32_t src_llt;
    uint32_t dst_llt;
    g_autofree uint8_t *src = NULL;
    g_autofree uint8_t *dst = NULL;
    size_t out_len = out_limit;
    bool ok = false;

    if (!s->gsdma) {
        warn_report("k230 ugzip has no gsdma link");
        return;
    }

    src_llt = k230_gsdma_get_llt_saddr(s->gsdma, K230_GSDMA_UGZIP_RD_CH);
    dst_llt = k230_gsdma_get_llt_saddr(s->gsdma, K230_GSDMA_UGZIP_WR_CH);

    if (src_len && out_limit && src_llt && dst_llt) {
        src = g_malloc(src_len);
        dst = g_malloc0(out_limit);
        ok = k230_ugzip_read_src_llt(src_llt, src, src_len) &&
             k230_ugzip_inflate(dst, &out_len, src, src_len) == Z_OK &&
             k230_ugzip_write_dst_llt(dst_llt, dst, out_len);
    }

    k230_ugzip_reg_write32(s, K230_UGZIP_INTSTAT,
                           ok ? K230_UGZIP_DONE : 0);
    k230_gsdma_ugzip_complete(s->gsdma);

    if (!ok) {
        warn_report("k230 ugzip decompression failed");
    }
}

static uint64_t k230_ugzip_read(void *opaque, hwaddr addr, unsigned int size)
{
    return k230_ugzip_read_bytes(K230_UGZIP(opaque)->regs, addr, size);
}

static void k230_ugzip_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned int size)
{
    K230UgzipState *s = K230_UGZIP(opaque);

    k230_ugzip_write_bytes(s->regs, addr, val, size);

    if (addr == K230_UGZIP_START && size == 4 && (val & BIT(0))) {
        k230_ugzip_start(s);
    }
}

static const MemoryRegionOps k230_ugzip_ops = {
    .read = k230_ugzip_read,
    .write = k230_ugzip_write,
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

static void k230_ugzip_reset(DeviceState *dev)
{
    K230UgzipState *s = K230_UGZIP(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_k230_ugzip = {
    .name = TYPE_K230_UGZIP,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230UgzipState, K230_UGZIP_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_ugzip_realize(DeviceState *dev, Error **errp)
{
    K230UgzipState *s = K230_UGZIP(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_ugzip_ops, s,
                          TYPE_K230_UGZIP, K230_UGZIP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_ugzip_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_ugzip_realize;
    device_class_set_legacy_reset(dc, k230_ugzip_reset);
    dc->vmsd = &vmstate_k230_ugzip;
    dc->desc = "K230 UGZIP decompressor";
}

static const TypeInfo k230_ugzip_type_info = {
    .name = TYPE_K230_UGZIP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230UgzipState),
    .class_init = k230_ugzip_class_init,
};

static void k230_register_ugzip_types(void)
{
    type_register_static(&k230_ugzip_type_info);
}

type_init(k230_register_ugzip_types)
