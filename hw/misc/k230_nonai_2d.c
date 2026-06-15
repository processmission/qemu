/*
 * K230 non-AI 2D engine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/misc/k230_nonai_2d.h"
#include "migration/vmstate.h"
#include "system/dma.h"

#define K230_NONAI_2D_STREAM_COUNT       3
#define K230_NONAI_2D_STREAM_STRIDE      0x400

#define K230_NONAI_2D_SRC_SIZE           0x000
#define K230_NONAI_2D_SRC_CH0_ADDR       0x004
#define K230_NONAI_2D_SRC_CH1_ADDR       0x008
#define K230_NONAI_2D_SRC_CH2_ADDR       0x00c
#define K230_NONAI_2D_SRC_STRIDE01       0x010
#define K230_NONAI_2D_SRC_STRIDE2        0x014
#define K230_NONAI_2D_FMT                0x018
#define K230_NONAI_2D_DST_CH0_ADDR       0x13c
#define K230_NONAI_2D_DST_CH1_ADDR       0x140
#define K230_NONAI_2D_DST_CH2_ADDR       0x144
#define K230_NONAI_2D_DST_STRIDE01       0x148
#define K230_NONAI_2D_DST_STRIDE2        0x14c

#define K230_NONAI_2D_MAIN_CFG           0x3a0
#define K230_NONAI_2D_INTR_STATUS        0x3a8
#define K230_NONAI_2D_INTR_CLEAR         0x3ac
#define K230_NONAI_2D_CALC_EN            BIT(0)
#define K230_NONAI_2D_STREAM_ID_SHIFT    2
#define K230_NONAI_2D_STREAM_ID_LEN      2

#define K230_NONAI_2D_FMT_NV12           0
#define K230_NONAI_2D_FMT_NV21           1
#define K230_NONAI_2D_FMT_I420           2
#define K230_NONAI_2D_FMT_ARGB8888       4
#define K230_NONAI_2D_FMT_ARGB4444       5
#define K230_NONAI_2D_FMT_ARGB1555       6
#define K230_NONAI_2D_FMT_XRGB8888       7
#define K230_NONAI_2D_FMT_XRGB4444       8
#define K230_NONAI_2D_FMT_XRGB1555       9
#define K230_NONAI_2D_FMT_BGRA8888       10
#define K230_NONAI_2D_FMT_BGRA4444       11
#define K230_NONAI_2D_FMT_BGRA5551       12
#define K230_NONAI_2D_FMT_BGRX8888       13
#define K230_NONAI_2D_FMT_BGRX4444       14
#define K230_NONAI_2D_FMT_BGRX5551       15
#define K230_NONAI_2D_FMT_RGB888         16
#define K230_NONAI_2D_FMT_BGR888         17
#define K230_NONAI_2D_FMT_RGB565         18
#define K230_NONAI_2D_FMT_BGR565         19
#define K230_NONAI_2D_FMT_SEPARATE_RGB   20

#define K230_NONAI_2D_MAX_COPY           (64 * MiB)

static uint64_t k230_nonai_2d_read_bytes(uint8_t *regs, hwaddr addr,
                                         unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_nonai_2d_write_bytes(uint8_t *regs, hwaddr addr,
                                      uint64_t val, unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static bool k230_nonai_2d_range_ok(hwaddr addr, unsigned int size)
{
    return addr <= K230_NONAI_2D_SIZE && size <= K230_NONAI_2D_SIZE - addr;
}

static bool k230_nonai_2d_access_hits(hwaddr addr, unsigned int size,
                                      hwaddr offset)
{
    return addr <= offset && offset < addr + size;
}

static uint32_t k230_nonai_2d_readl_regs(K230NonAI2DState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_nonai_2d_writel_regs(K230NonAI2DState *s, hwaddr addr,
                                      uint32_t val)
{
    stl_le_p(s->regs + addr, val);
}

static bool k230_nonai_2d_dma_copy(hwaddr src, hwaddr dst, size_t len)
{
    g_autofree uint8_t *buf = NULL;

    if (!src || !dst || !len || len > K230_NONAI_2D_MAX_COPY) {
        return false;
    }

    buf = g_malloc(len);
    if (dma_memory_read(&address_space_memory, src, buf, len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }

    return dma_memory_write(&address_space_memory, dst, buf, len,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static bool k230_nonai_2d_copy_rows(hwaddr src, hwaddr dst, size_t row_bytes,
                                    size_t rows, size_t src_stride,
                                    size_t dst_stride)
{
    g_autofree uint8_t *row = NULL;

    if (!row_bytes || !rows) {
        return true;
    }

    src_stride = src_stride ? src_stride : row_bytes;
    dst_stride = dst_stride ? dst_stride : row_bytes;
    if (row_bytes > src_stride || row_bytes > dst_stride ||
        rows > K230_NONAI_2D_MAX_COPY / row_bytes) {
        return false;
    }

    if (src_stride == row_bytes && dst_stride == row_bytes) {
        return k230_nonai_2d_dma_copy(src, dst, row_bytes * rows);
    }

    row = g_malloc(row_bytes);
    for (size_t y = 0; y < rows; y++) {
        if (dma_memory_read(&address_space_memory, src + y * src_stride,
                            row, row_bytes, MEMTXATTRS_UNSPECIFIED) !=
            MEMTX_OK ||
            dma_memory_write(&address_space_memory, dst + y * dst_stride,
                             row, row_bytes, MEMTXATTRS_UNSPECIFIED) !=
            MEMTX_OK) {
            return false;
        }
    }

    return true;
}

static uint16_t k230_nonai_2d_stride0(uint32_t val)
{
    return extract32(val, 0, 16);
}

static uint16_t k230_nonai_2d_stride1(uint32_t val)
{
    return extract32(val, 16, 16);
}

static bool k230_nonai_2d_copy_plane(K230NonAI2DState *s, hwaddr base,
                                     hwaddr src_offset, hwaddr dst_offset,
                                     size_t row_bytes, size_t rows,
                                     size_t src_stride, size_t dst_stride)
{
    uint32_t src = k230_nonai_2d_readl_regs(s, base + src_offset);
    uint32_t dst = k230_nonai_2d_readl_regs(s, base + dst_offset);

    return k230_nonai_2d_copy_rows(src, dst, row_bytes, rows,
                                   src_stride, dst_stride);
}

static bool k230_nonai_2d_copy_packed(K230NonAI2DState *s, hwaddr base,
                                      uint32_t width, uint32_t height,
                                      unsigned int bytes_per_pixel)
{
    uint32_t src_stride01 = k230_nonai_2d_readl_regs(s, base +
        K230_NONAI_2D_SRC_STRIDE01);
    uint32_t dst_stride01 = k230_nonai_2d_readl_regs(s, base +
        K230_NONAI_2D_DST_STRIDE01);
    size_t row_bytes = width * bytes_per_pixel;

    return k230_nonai_2d_copy_plane(s, base, K230_NONAI_2D_SRC_CH0_ADDR,
                                    K230_NONAI_2D_DST_CH0_ADDR, row_bytes,
                                    height, k230_nonai_2d_stride0(src_stride01),
                                    k230_nonai_2d_stride0(dst_stride01));
}

static bool k230_nonai_2d_copy_yuv420(K230NonAI2DState *s, hwaddr base,
                                      uint32_t width, uint32_t height,
                                      uint32_t fmt)
{
    uint32_t src_stride01 = k230_nonai_2d_readl_regs(s, base +
        K230_NONAI_2D_SRC_STRIDE01);
    uint32_t dst_stride01 = k230_nonai_2d_readl_regs(s, base +
        K230_NONAI_2D_DST_STRIDE01);
    uint32_t src_stride2 = k230_nonai_2d_readl_regs(s, base +
                                                    K230_NONAI_2D_SRC_STRIDE2);
    uint32_t dst_stride2 = k230_nonai_2d_readl_regs(s, base +
                                                    K230_NONAI_2D_DST_STRIDE2);

    if (!k230_nonai_2d_copy_plane(s, base, K230_NONAI_2D_SRC_CH0_ADDR,
                                  K230_NONAI_2D_DST_CH0_ADDR, width, height,
                                  k230_nonai_2d_stride0(src_stride01),
                                  k230_nonai_2d_stride0(dst_stride01))) {
        return false;
    }

    if (fmt == K230_NONAI_2D_FMT_NV12 || fmt == K230_NONAI_2D_FMT_NV21) {
        return k230_nonai_2d_copy_plane(s, base, K230_NONAI_2D_SRC_CH1_ADDR,
                                        K230_NONAI_2D_DST_CH1_ADDR, width,
                                        height / 2,
                                        k230_nonai_2d_stride1(src_stride01),
                                        k230_nonai_2d_stride1(dst_stride01));
    }

    return k230_nonai_2d_copy_plane(s, base, K230_NONAI_2D_SRC_CH1_ADDR,
                                    K230_NONAI_2D_DST_CH1_ADDR, width / 2,
                                    height / 2,
                                    k230_nonai_2d_stride1(src_stride01),
                                    k230_nonai_2d_stride1(dst_stride01)) &&
           k230_nonai_2d_copy_plane(s, base, K230_NONAI_2D_SRC_CH2_ADDR,
                                    K230_NONAI_2D_DST_CH2_ADDR, width / 2,
                                    height / 2,
                                    k230_nonai_2d_stride0(src_stride2),
                                    k230_nonai_2d_stride0(dst_stride2));
}

static bool k230_nonai_2d_copy_separate_rgb(K230NonAI2DState *s, hwaddr base,
                                            uint32_t width, uint32_t height)
{
    uint32_t src0 = k230_nonai_2d_readl_regs(s, base +
                                             K230_NONAI_2D_SRC_CH0_ADDR);
    uint32_t src1 = k230_nonai_2d_readl_regs(s, base +
                                             K230_NONAI_2D_SRC_CH1_ADDR);
    uint32_t src2 = k230_nonai_2d_readl_regs(s, base +
                                             K230_NONAI_2D_SRC_CH2_ADDR);
    uint32_t dst0 = k230_nonai_2d_readl_regs(s, base +
                                             K230_NONAI_2D_DST_CH0_ADDR);
    uint32_t dst1 = k230_nonai_2d_readl_regs(s, base +
                                             K230_NONAI_2D_DST_CH1_ADDR);
    uint32_t dst2 = k230_nonai_2d_readl_regs(s, base +
                                             K230_NONAI_2D_DST_CH2_ADDR);
    size_t len = (size_t)width * height;

    return k230_nonai_2d_dma_copy(src0, dst0, len) &&
           k230_nonai_2d_dma_copy(src1, dst1, len) &&
           k230_nonai_2d_dma_copy(src2, dst2, len);
}

static bool k230_nonai_2d_copy_stream(K230NonAI2DState *s, unsigned int stream)
{
    hwaddr base = stream * K230_NONAI_2D_STREAM_STRIDE;
    uint32_t size = k230_nonai_2d_readl_regs(s, base +
                                             K230_NONAI_2D_SRC_SIZE);
    uint32_t width = extract32(size, 0, 16);
    uint32_t height = extract32(size, 16, 16);
    uint32_t fmt = k230_nonai_2d_readl_regs(s, base + K230_NONAI_2D_FMT) &
                   0xff;

    if (!width || !height) {
        return false;
    }

    switch (fmt) {
    case K230_NONAI_2D_FMT_NV12:
    case K230_NONAI_2D_FMT_NV21:
    case K230_NONAI_2D_FMT_I420:
        return k230_nonai_2d_copy_yuv420(s, base, width, height, fmt);
    case K230_NONAI_2D_FMT_ARGB8888:
    case K230_NONAI_2D_FMT_XRGB8888:
    case K230_NONAI_2D_FMT_BGRA8888:
    case K230_NONAI_2D_FMT_BGRX8888:
        return k230_nonai_2d_copy_packed(s, base, width, height, 4);
    case K230_NONAI_2D_FMT_RGB888:
    case K230_NONAI_2D_FMT_BGR888:
        return k230_nonai_2d_copy_packed(s, base, width, height, 3);
    case K230_NONAI_2D_FMT_ARGB4444:
    case K230_NONAI_2D_FMT_ARGB1555:
    case K230_NONAI_2D_FMT_XRGB4444:
    case K230_NONAI_2D_FMT_XRGB1555:
    case K230_NONAI_2D_FMT_BGRA4444:
    case K230_NONAI_2D_FMT_BGRA5551:
    case K230_NONAI_2D_FMT_BGRX4444:
    case K230_NONAI_2D_FMT_BGRX5551:
    case K230_NONAI_2D_FMT_RGB565:
    case K230_NONAI_2D_FMT_BGR565:
        return k230_nonai_2d_copy_packed(s, base, width, height, 2);
    case K230_NONAI_2D_FMT_SEPARATE_RGB:
        return k230_nonai_2d_copy_separate_rgb(s, base, width, height);
    default:
        return false;
    }
}

static void k230_nonai_2d_raise_irq(K230NonAI2DState *s)
{
    k230_nonai_2d_writel_regs(s, K230_NONAI_2D_INTR_STATUS, 1);
    s->irq_level = true;
    qemu_set_irq(s->irq, 1);
}

static void k230_nonai_2d_complete(K230NonAI2DState *s, uint32_t cfg)
{
    unsigned int stream = extract32(cfg, K230_NONAI_2D_STREAM_ID_SHIFT,
                                    K230_NONAI_2D_STREAM_ID_LEN);

    if (stream < K230_NONAI_2D_STREAM_COUNT &&
        !k230_nonai_2d_copy_stream(s, stream)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "k230 non-AI 2D stream %u passthrough copy skipped\n",
                      stream);
    }

    k230_nonai_2d_raise_irq(s);
}

static uint64_t k230_nonai_2d_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    K230NonAI2DState *s = K230_NONAI_2D(opaque);

    if (!k230_nonai_2d_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "k230 non-AI 2D bad read offset 0x%" HWADDR_PRIx
                      " size %u\n", addr, size);
        return 0;
    }

    return k230_nonai_2d_read_bytes(s->regs, addr, size);
}

static void k230_nonai_2d_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned int size)
{
    K230NonAI2DState *s = K230_NONAI_2D(opaque);

    if (!k230_nonai_2d_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "k230 non-AI 2D bad write offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 " size %u\n", addr, val, size);
        return;
    }

    k230_nonai_2d_write_bytes(s->regs, addr, val, size);

    if (k230_nonai_2d_access_hits(addr, size, K230_NONAI_2D_INTR_CLEAR) &&
        val) {
        k230_nonai_2d_writel_regs(s, K230_NONAI_2D_INTR_STATUS, 0);
        s->irq_level = false;
        qemu_set_irq(s->irq, 0);
        return;
    }

    if (k230_nonai_2d_access_hits(addr, size, K230_NONAI_2D_MAIN_CFG) &&
        (val & K230_NONAI_2D_CALC_EN)) {
        k230_nonai_2d_complete(s, val);
    }
}

static const MemoryRegionOps k230_nonai_2d_ops = {
    .read = k230_nonai_2d_read,
    .write = k230_nonai_2d_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static void k230_nonai_2d_reset(DeviceState *dev)
{
    K230NonAI2DState *s = K230_NONAI_2D(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->irq_level = false;
    qemu_set_irq(s->irq, 0);
}

static const VMStateDescription vmstate_k230_nonai_2d = {
    .name = TYPE_K230_NONAI_2D,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(irq_level, K230NonAI2DState),
        VMSTATE_UINT8_ARRAY(regs, K230NonAI2DState, K230_NONAI_2D_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_nonai_2d_init(Object *obj)
{
    K230NonAI2DState *s = K230_NONAI_2D(obj);

    memory_region_init_io(&s->mmio, obj, &k230_nonai_2d_ops, s,
                          TYPE_K230_NONAI_2D, K230_NONAI_2D_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void k230_nonai_2d_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, k230_nonai_2d_reset);
    dc->vmsd = &vmstate_k230_nonai_2d;
}

static const TypeInfo k230_nonai_2d_info = {
    .name          = TYPE_K230_NONAI_2D,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230NonAI2DState),
    .instance_init = k230_nonai_2d_init,
    .class_init    = k230_nonai_2d_class_init,
};

static void k230_nonai_2d_register_types(void)
{
    type_register_static(&k230_nonai_2d_info);
}

type_init(k230_nonai_2d_register_types)
