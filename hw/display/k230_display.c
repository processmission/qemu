/*
 * K230 display controller register blocks
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/k230_display.h"
#include "hw/display/framebuffer.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"
#include "ui/pixel_ops.h"

#define K230_VBLANK_NS 16666667

#define K230_VO_DEFAULT_WIDTH  640
#define K230_VO_DEFAULT_HEIGHT 480

#define K230_VO_FBDEV_COMPAT_BASE   0x13100000
#define K230_VO_FBDEV_COMPAT_WIDTH  1920
#define K230_VO_FBDEV_COMPAT_HEIGHT 1080
#define K230_VO_FBDEV_COMPAT_STRIDE \
    (K230_VO_FBDEV_COMPAT_WIDTH * sizeof(uint32_t))

#define K230_VO_DISP_ENABLE    0x118

#define K230_VO_OSD0_BASE      0x280
#define K230_VO_OSD1_BASE      0x2c0
#define K230_VO_OSD2_BASE      0x300
#define K230_VO_OSD3_BASE      0x850
#define K230_VO_OSD4_BASE      0x880
#define K230_VO_OSD5_BASE      0x8b0
#define K230_VO_OSD6_BASE      0x8e0
#define K230_VO_OSD7_BASE      0x910

#define K230_VO_OSD_INFO       0x00
#define K230_VO_OSD_SIZE       0x04
#define K230_VO_OSD_VLU_ADDR0  0x08
#define K230_VO_OSD_STRIDE     0x1c

#define K230_VO_OSD0_ENABLE    BIT(4)
#define K230_VO_OSD1_ENABLE    BIT(5)
#define K230_VO_OSD2_ENABLE    BIT(6)
#define K230_VO_OSD3_ENABLE    BIT(7)
#define K230_VO_OSD4_ENABLE    BIT(8)
#define K230_VO_OSD5_ENABLE    BIT(9)
#define K230_VO_OSD6_ENABLE    BIT(10)
#define K230_VO_OSD7_ENABLE    BIT(11)

#define K230_VO_OSD_FMT_RGB888    0x00
#define K230_VO_OSD_FMT_RGB565    0x02
#define K230_VO_OSD_FMT_ARGB8888  0x53
#define K230_VO_OSD_FMT_ARGB4444  0x54
#define K230_VO_OSD_FMT_ARGB1555  0x55

#define K230_DSI_CMD_STATUS       0x0b0
#define K230_DSI_PHY_STATUS       0x0b8

#define K230_DSI_CMD_STATUS_READY 0x1fbd
#define K230_DSI_PHY_STATUS_READY 0x0580

typedef struct K230VoPlaneConfig {
    uint32_t base;
    uint32_t enable_mask;
} K230VoPlaneConfig;

typedef struct K230VoScanout {
    hwaddr base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    drawfn draw_line;
} K230VoScanout;

static const K230VoPlaneConfig k230_vo_osd_planes[] = {
    { K230_VO_OSD4_BASE, K230_VO_OSD4_ENABLE },
    { K230_VO_OSD5_BASE, K230_VO_OSD5_ENABLE },
    { K230_VO_OSD6_BASE, K230_VO_OSD6_ENABLE },
    { K230_VO_OSD7_BASE, K230_VO_OSD7_ENABLE },
    { K230_VO_OSD0_BASE, K230_VO_OSD0_ENABLE },
    { K230_VO_OSD1_BASE, K230_VO_OSD1_ENABLE },
    { K230_VO_OSD2_BASE, K230_VO_OSD2_ENABLE },
    { K230_VO_OSD3_BASE, K230_VO_OSD3_ENABLE },
};

static uint64_t k230_display_read_bytes(uint8_t *regs, size_t limit,
                                        hwaddr addr, unsigned int size)
{
    uint64_t val = 0;

    if (addr >= limit || size > limit - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_display_write_bytes(uint8_t *regs, size_t limit,
                                     hwaddr addr, uint64_t val,
                                     unsigned int size)
{
    if (addr >= limit || size > limit - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_display_readl(uint8_t *regs, size_t limit, hwaddr addr)
{
    if (addr >= limit || sizeof(uint32_t) > limit - addr) {
        return 0;
    }

    return ldl_le_p(regs + addr);
}

static void k230_vo_draw_line_xrgb8888(void *opaque, uint8_t *dst,
                                       const uint8_t *src, int width,
                                       int dststep)
{
    uint32_t *dst32 = (uint32_t *)dst;

    for (int i = 0; i < width; i++) {
        uint32_t pixel = ldl_le_p(src);
        uint8_t r = (pixel >> 16) & 0xff;
        uint8_t g = (pixel >> 8) & 0xff;
        uint8_t b = pixel & 0xff;

        *dst32++ = rgb_to_pixel32(r, g, b);
        src += 4;
    }
}

static void k230_vo_draw_line_argb8888(void *opaque, uint8_t *dst,
                                       const uint8_t *src, int width,
                                       int dststep)
{
    k230_vo_draw_line_xrgb8888(opaque, dst, src, width, dststep);
}

static void k230_vo_draw_line_argb4444(void *opaque, uint8_t *dst,
                                       const uint8_t *src, int width,
                                       int dststep)
{
    uint32_t *dst32 = (uint32_t *)dst;

    for (int i = 0; i < width; i++) {
        uint16_t pixel = lduw_le_p(src);
        uint8_t r = ((pixel >> 8) & 0xf) * 0x11;
        uint8_t g = ((pixel >> 4) & 0xf) * 0x11;
        uint8_t b = (pixel & 0xf) * 0x11;

        *dst32++ = rgb_to_pixel32(r, g, b);
        src += 2;
    }
}

static void k230_vo_draw_line_argb1555(void *opaque, uint8_t *dst,
                                       const uint8_t *src, int width,
                                       int dststep)
{
    uint32_t *dst32 = (uint32_t *)dst;

    for (int i = 0; i < width; i++) {
        uint16_t pixel = lduw_le_p(src);
        uint8_t r = ((pixel >> 10) & 0x1f) << 3;
        uint8_t g = ((pixel >> 5) & 0x1f) << 3;
        uint8_t b = (pixel & 0x1f) << 3;

        *dst32++ = rgb_to_pixel32(r, g, b);
        src += 2;
    }
}

static void k230_vo_draw_line_rgb888(void *opaque, uint8_t *dst,
                                     const uint8_t *src, int width,
                                     int dststep)
{
    uint32_t *dst32 = (uint32_t *)dst;

    for (int i = 0; i < width; i++) {
        *dst32++ = rgb_to_pixel32(src[0], src[1], src[2]);
        src += 3;
    }
}

static void k230_vo_draw_line_rgb565(void *opaque, uint8_t *dst,
                                     const uint8_t *src, int width,
                                     int dststep)
{
    uint32_t *dst32 = (uint32_t *)dst;

    for (int i = 0; i < width; i++) {
        uint16_t pixel = lduw_le_p(src);
        uint8_t r = ((pixel >> 11) & 0x1f) << 3;
        uint8_t g = ((pixel >> 5) & 0x3f) << 2;
        uint8_t b = (pixel & 0x1f) << 3;

        *dst32++ = rgb_to_pixel32(r, g, b);
        src += 2;
    }
}

static bool k230_vo_decode_osd_format(uint32_t info, drawfn *draw_line,
                                      uint32_t *bytes_per_pixel)
{
    switch (info & 0xff) {
    case K230_VO_OSD_FMT_ARGB8888:
        *draw_line = k230_vo_draw_line_argb8888;
        *bytes_per_pixel = 4;
        return true;
    case K230_VO_OSD_FMT_ARGB4444:
        *draw_line = k230_vo_draw_line_argb4444;
        *bytes_per_pixel = 2;
        return true;
    case K230_VO_OSD_FMT_ARGB1555:
        *draw_line = k230_vo_draw_line_argb1555;
        *bytes_per_pixel = 2;
        return true;
    case K230_VO_OSD_FMT_RGB888:
        *draw_line = k230_vo_draw_line_rgb888;
        *bytes_per_pixel = 3;
        return true;
    case K230_VO_OSD_FMT_RGB565:
        *draw_line = k230_vo_draw_line_rgb565;
        *bytes_per_pixel = 2;
        return true;
    default:
        return false;
    }
}

static bool k230_vo_get_fbdev_compat_scanout(K230VoState *s,
                                             K230VoScanout *scanout)
{
    if (!s->fbdev_compat || !s->fbdev_base ||
        !s->fbdev_width || !s->fbdev_height) {
        return false;
    }

    scanout->base = s->fbdev_base;
    scanout->width = s->fbdev_width;
    scanout->height = s->fbdev_height;
    scanout->stride = s->fbdev_stride;
    if (!scanout->stride) {
        scanout->stride = scanout->width * sizeof(uint32_t);
    }
    if (scanout->stride < scanout->width * sizeof(uint32_t)) {
        return false;
    }

    scanout->draw_line = k230_vo_draw_line_xrgb8888;
    return true;
}

static bool k230_vo_get_osd_scanout(K230VoState *s, K230VoScanout *scanout)
{
    uint32_t disp_en = k230_display_readl(s->regs, sizeof(s->regs),
                                          K230_VO_DISP_ENABLE);

    for (size_t i = 0; i < ARRAY_SIZE(k230_vo_osd_planes); i++) {
        const K230VoPlaneConfig *plane = &k230_vo_osd_planes[i];
        uint32_t size;
        uint32_t info;
        uint32_t stride_reg;
        uint32_t bytes_per_pixel;

        if (!(disp_en & plane->enable_mask)) {
            continue;
        }

        info = k230_display_readl(s->regs, sizeof(s->regs),
                                  plane->base + K230_VO_OSD_INFO);
        if (!k230_vo_decode_osd_format(info, &scanout->draw_line,
                                       &bytes_per_pixel)) {
            continue;
        }

        size = k230_display_readl(s->regs, sizeof(s->regs),
                                  plane->base + K230_VO_OSD_SIZE);
        scanout->width = size & 0xffff;
        scanout->height = size >> 16;
        if (!scanout->width || !scanout->height ||
            scanout->width > 4096 || scanout->height > 4096) {
            continue;
        }

        stride_reg = k230_display_readl(s->regs, sizeof(s->regs),
                                        plane->base + K230_VO_OSD_STRIDE);
        scanout->stride = stride_reg * 8;
        if (scanout->stride < scanout->width * bytes_per_pixel) {
            scanout->stride = scanout->width * bytes_per_pixel;
        }

        scanout->base = k230_display_readl(s->regs, sizeof(s->regs),
                                           plane->base +
                                           K230_VO_OSD_VLU_ADDR0);
        if (!scanout->base) {
            continue;
        }

        return true;
    }

    return false;
}

static bool k230_vo_get_scanout(K230VoState *s, K230VoScanout *scanout)
{
    if (k230_vo_get_osd_scanout(s, scanout)) {
        return true;
    }

    return k230_vo_get_fbdev_compat_scanout(s, scanout);
}

static bool k230_vo_update_display(void *opaque)
{
    K230VoState *s = K230_VO(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    K230VoScanout scanout = { 0 };
    int first = 0;
    int last = 0;

    if (!k230_vo_get_scanout(s, &scanout)) {
        return true;
    }

    if (surface_width(surface) != scanout.width ||
        surface_height(surface) != scanout.height) {
        qemu_console_resize(s->con, scanout.width, scanout.height);
        surface = qemu_console_surface(s->con);
        s->invalidate = true;
    }

    if (s->invalidate || s->fb_base != scanout.base ||
        s->src_width != scanout.stride || s->rows != scanout.height) {
        framebuffer_update_memory_section(&s->fbsection, get_system_memory(),
                                          scanout.base, scanout.height,
                                          scanout.stride);
        s->fb_base = scanout.base;
        s->src_width = scanout.stride;
        s->cols = scanout.width;
        s->rows = scanout.height;
    }

    framebuffer_update_display(surface, &s->fbsection, scanout.width,
                               scanout.height, scanout.stride,
                               surface_stride(surface), 0, s->invalidate,
                               scanout.draw_line, s, &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, scanout.width,
                            last - first + 1);
    }

    s->invalidate = false;
    return true;
}

static void k230_vo_invalidate_display(void *opaque)
{
    K230VoState *s = K230_VO(opaque);

    s->invalidate = true;
}

static const GraphicHwOps k230_vo_gfx_ops = {
    .invalidate = k230_vo_invalidate_display,
    .gfx_update = k230_vo_update_display,
};

static void k230_vo_vblank_tick(void *opaque)
{
    K230VoState *s = K230_VO(opaque);

    if (s->con) {
        qemu_console_hw_update(s->con);
    }

    if (s->irq) {
        trace_k230_vo_irq(true);
        qemu_irq_pulse(s->irq);
    }

    timer_mod(s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + K230_VBLANK_NS);
}

static uint64_t k230_vo_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230VoState *s = K230_VO(opaque);
    uint64_t val;

    val = k230_display_read_bytes(s->regs, sizeof(s->regs), addr, size);
    trace_k230_vo_read(addr, val, size);

    return val;
}

static void k230_vo_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned int size)
{
    K230VoState *s = K230_VO(opaque);

    k230_display_write_bytes(s->regs, sizeof(s->regs), addr, val, size);
    trace_k230_vo_write(addr, val, size);
    s->invalidate = true;
}

static const MemoryRegionOps k230_vo_ops = {
    .read = k230_vo_read,
    .write = k230_vo_write,
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

static void k230_vo_reset(DeviceState *dev)
{
    K230VoState *s = K230_VO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->fb_base = 0;
    s->src_width = 0;
    s->cols = K230_VO_DEFAULT_WIDTH;
    s->rows = K230_VO_DEFAULT_HEIGHT;
    s->invalidate = true;
    if (s->con) {
        qemu_console_resize(s->con, K230_VO_DEFAULT_WIDTH,
                            K230_VO_DEFAULT_HEIGHT);
    }
    timer_mod(s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + K230_VBLANK_NS);
}

static const VMStateDescription vmstate_k230_vo = {
    .name = TYPE_K230_VO,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230VoState, K230_VO_SIZE),
        VMSTATE_UINT64(fb_base, K230VoState),
        VMSTATE_UINT64(fbdev_base, K230VoState),
        VMSTATE_UINT32(fbdev_width, K230VoState),
        VMSTATE_UINT32(fbdev_height, K230VoState),
        VMSTATE_UINT32(fbdev_stride, K230VoState),
        VMSTATE_UINT32(src_width, K230VoState),
        VMSTATE_UINT32(cols, K230VoState),
        VMSTATE_UINT32(rows, K230VoState),
        VMSTATE_BOOL(fbdev_compat, K230VoState),
        VMSTATE_BOOL(invalidate, K230VoState),
        VMSTATE_TIMER_PTR(vblank_timer, K230VoState),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property k230_vo_properties[] = {
    DEFINE_PROP_BOOL("fbdev-compat", K230VoState, fbdev_compat, true),
    DEFINE_PROP_UINT64("fbdev-base", K230VoState, fbdev_base,
                       K230_VO_FBDEV_COMPAT_BASE),
    DEFINE_PROP_UINT32("fbdev-width", K230VoState, fbdev_width,
                       K230_VO_FBDEV_COMPAT_WIDTH),
    DEFINE_PROP_UINT32("fbdev-height", K230VoState, fbdev_height,
                       K230_VO_FBDEV_COMPAT_HEIGHT),
    DEFINE_PROP_UINT32("fbdev-stride", K230VoState, fbdev_stride,
                       K230_VO_FBDEV_COMPAT_STRIDE),
};

static void k230_vo_init(Object *obj)
{
    K230VoState *s = K230_VO(obj);

    memory_region_init_io(&s->mmio, obj, &k230_vo_ops, s,
                          TYPE_K230_VO, K230_VO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   k230_vo_vblank_tick, s);
}

static void k230_vo_realize(DeviceState *dev, Error **errp)
{
    K230VoState *s = K230_VO(dev);

    s->con = qemu_graphic_console_create(dev, 0, &k230_vo_gfx_ops, s);
    qemu_console_resize(s->con, K230_VO_DEFAULT_WIDTH,
                        K230_VO_DEFAULT_HEIGHT);
}

static void k230_vo_unrealize(DeviceState *dev)
{
    K230VoState *s = K230_VO(dev);

    if (s->con) {
        qemu_graphic_console_close(s->con);
        s->con = NULL;
    }
}

static void k230_vo_finalize(Object *obj)
{
    K230VoState *s = K230_VO(obj);

    if (s->fbsection.mr) {
        memory_region_set_log(s->fbsection.mr, false, DIRTY_MEMORY_VGA);
        memory_region_unref(s->fbsection.mr);
        s->fbsection.mr = NULL;
    }
    timer_free(s->vblank_timer);
}

static void k230_vo_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, k230_vo_reset);
    dc->realize = k230_vo_realize;
    dc->unrealize = k230_vo_unrealize;
    dc->vmsd = &vmstate_k230_vo;
    device_class_set_props(dc, k230_vo_properties);
    dc->desc = "K230 video output registers";
}

static const TypeInfo k230_vo_type_info = {
    .name = TYPE_K230_VO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230VoState),
    .instance_init = k230_vo_init,
    .instance_finalize = k230_vo_finalize,
    .class_init = k230_vo_class_init,
};

static uint64_t k230_dsi_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230DsiState *s = K230_DSI(opaque);
    uint64_t val;

    switch (addr) {
    case K230_DSI_CMD_STATUS:
        val = K230_DSI_CMD_STATUS_READY;
        break;
    case K230_DSI_PHY_STATUS:
        val = K230_DSI_PHY_STATUS_READY;
        break;
    default:
        val = k230_display_read_bytes(s->regs, sizeof(s->regs), addr, size);
        break;
    }
    trace_k230_dsi_read(addr, val, size);

    return val;
}

static void k230_dsi_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    K230DsiState *s = K230_DSI(opaque);

    k230_display_write_bytes(s->regs, sizeof(s->regs), addr, val, size);
    trace_k230_dsi_write(addr, val, size);
}

static const MemoryRegionOps k230_dsi_ops = {
    .read = k230_dsi_read,
    .write = k230_dsi_write,
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

static void k230_dsi_reset(DeviceState *dev)
{
    K230DsiState *s = K230_DSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_k230_dsi = {
    .name = TYPE_K230_DSI,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230DsiState, K230_DSI_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_dsi_init(Object *obj)
{
    K230DsiState *s = K230_DSI(obj);

    memory_region_init_io(&s->mmio, obj, &k230_dsi_ops, s,
                          TYPE_K230_DSI, K230_DSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void k230_dsi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, k230_dsi_reset);
    dc->vmsd = &vmstate_k230_dsi;
    dc->desc = "K230 MIPI DSI registers";
}

static const TypeInfo k230_dsi_type_info = {
    .name = TYPE_K230_DSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230DsiState),
    .instance_init = k230_dsi_init,
    .class_init = k230_dsi_class_init,
};

static void k230_display_register_types(void)
{
    type_register_static(&k230_vo_type_info);
    type_register_static(&k230_dsi_type_info);
}

type_init(k230_display_register_types)
