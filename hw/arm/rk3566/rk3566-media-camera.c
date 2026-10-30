/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GC2093 sensor control and RKISP 2.1 capture from external UYVY frames. */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#define TYPE_RK3566_ISP "rk3566-isp"
#define TYPE_GC2093 "microduck-gc2093"
#define TYPE_RK3566_DPHY "rk3566-csi-dphy"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566ISPState, RK3566_ISP)
OBJECT_DECLARE_SIMPLE_TYPE(GC2093State, GC2093)
OBJECT_DECLARE_SIMPLE_TYPE(RK3566DPHYState, RK3566_DPHY)
#define REG(s, address) ((s)->regs[(address) / 4])

enum {
    ISP_CTRL = 0x400, ISP_H_SIZE = 0x59c, ISP_V_SIZE = 0x5a0,
    ISP_IMSC = 0x5bc, ISP_RIS = 0x5c0, ISP_MIS = 0x5c4,
    ISP_ICR = 0x5c8, ISP_ISR = 0x5cc, ISP_FRAME_COUNT = 0x644,
    MI_CTRL = 0x1400, MI_INIT = 0x1404, MI_CTRL_SHD = 0x1474,
    MI_IMSC = 0x14f8, MI_RIS = 0x14fc, MI_MIS = 0x1500,
    MI_ICR = 0x1504, MI_ISR = 0x1508,
};

#define ISP_REG_WINDOW 0x10000
#define ISP_REG_BANK_SIZE 0x1600
#define ISP_NUM_REGS (ISP_REG_BANK_SIZE / 4)

struct RK3566ISPState {
    SysBusDevice parent_obj;
    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[ISP_NUM_REGS];
    MemoryRegion *dma;
    AddressSpace dma_as;
    CharFrontend camera;
    qemu_irq irq[3];
    QEMUTimer *frame_end;
    uint32_t regs[ISP_REG_WINDOW / 4];
    uint32_t active[ISP_REG_BANK_SIZE / 4];
    uint32_t width, height;
    uint32_t input_size, input_count;
    uint64_t input_frames;
    uint32_t pixel_rate_hz;
    uint8_t *input, *frame;
    bool streaming, link_enabled, busy;
};

struct GC2093State {
    I2CSlave parent_obj;
    uint8_t regs[0x10000];
    uint16_t address;
    uint8_t address_bytes;
    qemu_irq streaming;
    bool powered, resetn;
};

enum {
    DPHY_REG_CTRL,
    DPHY_REG_MAX,
};

struct RK3566DPHYState {
    SysBusDevice parent_obj;
    qemu_irq enabled;
    uint32_t regs[0x1000 / 4];
    RegisterInfo regs_info[DPHY_REG_MAX];
    RegisterInfoArray *reg_array;
};

static void isp_update_irq(RK3566ISPState *s)
{
    REG(s, ISP_MIS) = REG(s, ISP_RIS) & REG(s, ISP_IMSC);
    REG(s, MI_MIS) = REG(s, MI_RIS) & REG(s, MI_IMSC);
    qemu_set_irq(s->irq[0], 0);
    qemu_set_irq(s->irq[1], REG(s, MI_MIS) != 0);
    qemu_set_irq(s->irq[2], REG(s, ISP_MIS) != 0);
}

static void isp_latch(RK3566ISPState *s)
{
    static const uint16_t pairs[][2] = {
        {0x1408, 0x1478}, {0x140c, 0x147c}, {0x1410, 0x1480},
        {0x1418, 0x1484}, {0x141c, 0x1488}, {0x1420, 0x148c},
        {0x1424, 0x1490}, {0x142c, 0x1494}, {0x1430, 0x1498},
        {0x1434, 0x149c}, {0x143c, 0x14a0}, {0x1440, 0x14a4},
        {0x1444, 0x14a8}, {0x1450, 0x14b0}, {0x1454, 0x14b4},
        {0x1458, 0x14b8}, {0x1460, 0x14bc}, {0x1464, 0x14c0},
        {0x1468, 0x14c4}, {0x0594, 0x05ac}, {0x0598, 0x05b0},
        {0x059c, 0x05b4}, {0x05a0, 0x05b8},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(pairs); i++) {
        REG(s, pairs[i][1]) = REG(s, pairs[i][0]);
    }
    REG(s, MI_CTRL_SHD) = (REG(s, MI_CTRL) & 0xffff) |
                          ((REG(s, MI_CTRL) & 15) << 16);
    for (unsigned base = 0xc00; base <= 0x1000; base += 0x400) {
        for (unsigned offset = 0; offset <= 0x24; offset += 4) {
            REG(s, base + offset + 0x30) = REG(s, base + offset);
        }
    }
    memcpy(s->active, s->regs, sizeof(s->active));
}

static unsigned isp_scaled_size(unsigned input, unsigned factor,
                                bool enabled, bool enlarge)
{
    if (!enabled || input < 2 || !factor) {
        return input;
    }
    if (enlarge) {
        return ((uint64_t)(input - 1) * 65536 + factor / 2) / factor + 1;
    }
    return ((uint64_t)(input - 1) * (factor - 1) + 65535) / 65536 + 1;
}

static enum AVPixelFormat isp_output_format(RK3566ISPState *s, bool selfpath)
{
    uint32_t control = s->active[MI_CTRL / 4];
    unsigned packing = extract32(control, selfpath ? 24 : 22, 2);
    bool swap = s->active[0x1548 / 4] & BIT(selfpath ? 1 : 0);
    unsigned chroma;
    if (selfpath) {
        chroma = extract32(control, 28, 3);
        if (chroma == 0) {
            return AV_PIX_FMT_GRAY8;
        }
        if (chroma == 4) {
            return AV_PIX_FMT_RGB565LE;
        }
        if (chroma == 7) {
            return AV_PIX_FMT_BGR0;
        }
    } else {
        unsigned y_size = s->active[0x140c / 4];
        unsigned cb_size = s->active[0x1420 / 4];
        chroma = cb_size && cb_size * (packing == 1 ? 2 : 4) == y_size ? 1 : 2;
    }
    if (packing == 2 && chroma == 2 && !swap) {
        return AV_PIX_FMT_UYVY422;
    }
    if (packing == 1) {
        if (chroma == 1) {
            return swap ? AV_PIX_FMT_NV21 : AV_PIX_FMT_NV12;
        }
        if (chroma == 2 && !swap) {
            return AV_PIX_FMT_NV16;
        }
    }
    if (packing == 0) {
        return chroma == 1 ? AV_PIX_FMT_YUV420P :
               chroma == 2 ? AV_PIX_FMT_YUV422P :
               chroma == 3 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_NONE;
    }
    return AV_PIX_FMT_NONE;
}

static bool isp_capture_path(RK3566ISPState *s, bool selfpath)
{
    enum AVPixelFormat format = isp_output_format(s, selfpath);
    const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(format);
    uint8_t *output[4] = {};
    int output_stride[4] = {};
    struct SwsContext *scaler = NULL;
    unsigned width, height;
    unsigned source_width = s->width, source_height = s->height;
    unsigned source_x = 0, source_y = 0;
    unsigned base = selfpath ? 0x143c : 0x1408;
    unsigned sizes[3] = {}, addresses[3];
    unsigned offset[] = {0, 0x14, 0x24};
    unsigned guest_stride;
    bool result = false;
    const char *failure = "image allocation or luma stride";

    if (format == AV_PIX_FMT_NONE || !description) {
        return false;
    }
    if (selfpath) {
        width = s->active[0x1514 / 4];
        height = s->active[0x1518 / 4];
    } else {
        unsigned crop = s->active[0x880 / 4] & 3;
        unsigned input_width = s->active[(crop ? 0x88c : ISP_H_SIZE) / 4];
        unsigned input_height = s->active[(crop ? 0x890 : ISP_V_SIZE) / 4];
        uint32_t resize = s->active[0xc00 / 4];
        if (crop) {
            source_x = s->active[0x884 / 4];
            source_y = s->active[0x888 / 4];
            source_width = input_width;
            source_height = input_height;
        }
        width = isp_scaled_size(input_width, s->active[0xc04 / 4],
                                 resize & BIT(0), resize & BIT(4));
        height = isp_scaled_size(input_height, s->active[0xc10 / 4],
                                  resize & BIT(2), resize & BIT(6));
        if (crop && ((source_x & 1) || !input_width || !input_height ||
                     (uint64_t)source_x + input_width > s->width ||
                     (uint64_t)source_y + input_height > s->height)) {
            failure = "crop window";
            goto out;
        }
    }
    if (!width || !height || width > 8192 || height > 8192) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        addresses[i] = s->active[(base + offset[i]) / 4];
        sizes[i] = s->active[(base + offset[i] + 4) / 4];
    }
    guest_stride = sizes[0] / height;
    if (guest_stride < av_image_get_linesize(format, width, 0) ||
        av_image_alloc(output, output_stride, width, height, format, 16) < 0) {
        goto out;
    }
    scaler = sws_getContext(source_width, source_height, AV_PIX_FMT_UYVY422,
                            width, height, format, SWS_BILINEAR,
                            NULL, NULL, NULL);
    if (!scaler) {
        failure = "scaler configuration";
        goto out;
    }
    size_t input_offset = ((size_t)source_y * s->width + source_x) * 2;
    const uint8_t *input[] = { s->frame + input_offset, NULL, NULL, NULL };
    int input_stride[] = { s->width * 2, 0, 0, 0 };
    if (sws_scale(scaler, input, input_stride, 0, source_height,
                   output, output_stride) != height) {
        failure = "image conversion";
        goto out;
    }
    unsigned planes = av_pix_fmt_count_planes(format);
    for (unsigned plane = 0; plane < planes; plane++) {
        unsigned rows = height;
        unsigned row_bytes = av_image_get_linesize(format, width, plane);
        unsigned stride = guest_stride;
        if (plane) {
            rows = AV_CEIL_RSHIFT((int)height, description->log2_chroma_h);
            stride = sizes[plane] / rows;
        }
        if (stride < row_bytes || (uint64_t)stride * rows > sizes[plane]) {
            failure = "plane size or stride";
            goto out;
        }
        for (unsigned row = 0; row < rows; row++) {
            if (dma_memory_write(&s->dma_as,
                                  (dma_addr_t)addresses[plane] +
                                  (uint64_t)row * stride,
                                  output[plane] + row * output_stride[plane],
                                  row_bytes,
                                  MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                failure = "DMA write";
                goto out;
            }
        }
    }
    REG(s, MI_RIS) |= BIT(selfpath ? 1 : 0);
    result = true;
out:
    if (!result) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3566-isp: %s path=%u format=%s %ux%u "
                      "sizes=%u,%u,%u\n", failure, selfpath,
                      av_get_pix_fmt_name(format), width, height,
                      sizes[0], sizes[1], sizes[2]);
    }
    sws_freeContext(scaler);
    av_freep(&output[0]);
    return result;
}

static void isp_frame_end(void *opaque)
{
    RK3566ISPState *s = opaque;
    bool good = true;
    uint32_t control = s->active[MI_CTRL / 4];
    for (unsigned path = 0; path < 2; path++) {
        if (control & BIT(path)) {
            good &= isp_capture_path(s, path != 0);
        }
    }
    s->busy = false;
    if (good) {
        REG(s, ISP_RIS) |= BIT(1) | BIT(5);
        REG(s, ISP_FRAME_COUNT)++;
    } else {
        REG(s, ISP_RIS) |= BIT(3);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3566-isp: invalid capture format, dimensions or DMA buffer\n");
    }
    isp_latch(s);
    isp_update_irq(s);
}

static int isp_can_receive(void *opaque)
{
    RK3566ISPState *s = opaque;
    return MIN(65536, s->input_size - s->input_count);
}

static void isp_receive(void *opaque, const uint8_t *data, int size)
{
    RK3566ISPState *s = opaque;
    memcpy(s->input + s->input_count, data, size);
    s->input_count += size;
    if (s->input_count != s->input_size) {
        return;
    }
    s->input_count = 0;
    s->input_frames++;
    if (!s->streaming || !s->link_enabled ||
        !(REG(s, ISP_CTRL) & 1) || s->busy) {
        return;
    }
    uint8_t *swap = s->frame;
    s->frame = s->input;
    s->input = swap;
    s->busy = true;
    REG(s, ISP_RIS) |= BIT(6);
    isp_update_irq(s);
    unsigned raw_width = REG(s, 0x410) ?: s->width;
    unsigned raw_height = REG(s, 0x414) ?: s->height;
    if (raw_width > 8192 || raw_height > 8192) {
        s->busy = false;
        REG(s, ISP_RIS) |= BIT(3);
        isp_update_irq(s);
        return;
    }
    uint64_t frame_ns = muldiv64((uint64_t)raw_width * raw_height,
                                 NANOSECONDS_PER_SECOND, s->pixel_rate_hz);
    timer_mod_ns(s->frame_end, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 MAX(frame_ns, 1));
}

static void isp_sensor_stream(void *opaque, int line, int level)
{
    RK3566_ISP(opaque)->streaming = level;
}

static void isp_link_enable(void *opaque, int line, int level)
{
    RK3566_ISP(opaque)->link_enabled = level;
}

static void isp_camera_event(void *opaque, QEMUChrEvent event)
{
    if (event == CHR_EVENT_CLOSED || event == CHR_EVENT_OPENED) {
        RK3566_ISP(opaque)->input_count = 0;
    }
}

static void isp_sys_reset_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    if (!val) {
        return;
    }
    timer_del(s->frame_end);
    s->busy = false;
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->active, 0, sizeof(s->active));
}

static uint64_t isp_ctrl_pre_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);
    uint32_t old = *(uint32_t *)reg->data;

    if (!(val & 1) && (old & 1)) {
        REG(s, ISP_RIS) |= BIT(0);
        timer_del(s->frame_end);
        s->busy = false;
    }
    return val;
}

static void isp_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    *(uint32_t *)reg->data = val & ~(BIT(9) | BIT(10));
    if (val & (BIT(9) | BIT(10))) {
        isp_latch(s);
    }
}

static void isp_ris_clear_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);
    hwaddr status = reg->access->addr == ISP_ICR ? ISP_RIS : MI_RIS;

    REG(s, status) &= ~(uint32_t)val;
}

static void isp_ris_set_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);
    hwaddr status = reg->access->addr == ISP_ISR ? ISP_RIS : MI_RIS;

    REG(s, status) |= (uint32_t)val;
}

static uint64_t isp_write_only_post_read(RegisterInfo *reg, uint64_t val)
{
    return 0;
}

static void isp_mi_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    if (!s->busy) {
        isp_latch(s);
    }
}

static void isp_mi_init_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    if (val & BIT(4)) {
        isp_latch(s);
    }
}

#define ISP_REG(off) { .name = "ISP_REG_" #off, .addr = (off) }

static const RegisterAccessInfo isp_regs_info[] = {
    { .name = "SYS_RESET", .addr = 0x014,
      .post_write = isp_sys_reset_post_write },
    { .name = "ISP_CTRL", .addr = ISP_CTRL,
      .pre_write = isp_ctrl_pre_write,
      .post_write = isp_ctrl_post_write },
    { .name = "ISP_RIS", .addr = ISP_RIS, .ro = UINT32_MAX },
    { .name = "ISP_MIS", .addr = ISP_MIS, .ro = UINT32_MAX },
    { .name = "ISP_ICR", .addr = ISP_ICR,
      .post_write = isp_ris_clear_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "ISP_ISR", .addr = ISP_ISR,
      .post_write = isp_ris_set_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "ISP_FRAME_COUNT", .addr = ISP_FRAME_COUNT, .ro = UINT32_MAX },
    { .name = "MI_CTRL", .addr = MI_CTRL,
      .post_write = isp_mi_ctrl_post_write },
    { .name = "MI_INIT", .addr = MI_INIT,
      .post_write = isp_mi_init_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "MI_RIS", .addr = MI_RIS, .ro = UINT32_MAX },
    { .name = "MI_MIS", .addr = MI_MIS, .ro = UINT32_MAX },
    { .name = "MI_ICR", .addr = MI_ICR,
      .post_write = isp_ris_clear_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "MI_ISR", .addr = MI_ISR,
      .post_write = isp_ris_set_post_write,
      .post_read = isp_write_only_post_read },
    /* Capture configuration, shadow pairs and shadow write-back ranges. */
    ISP_REG(0x0410), ISP_REG(0x0414), ISP_REG(0x0594), ISP_REG(0x0598),
    ISP_REG(0x059c), ISP_REG(0x05a0), ISP_REG(0x05ac), ISP_REG(0x05b0),
    ISP_REG(0x05b4), ISP_REG(0x05b8), ISP_REG(0x05bc),
    ISP_REG(0x0880), ISP_REG(0x0884), ISP_REG(0x0888), ISP_REG(0x088c),
    ISP_REG(0x0890),
    ISP_REG(0x0c00), ISP_REG(0x0c04), ISP_REG(0x0c08), ISP_REG(0x0c0c),
    ISP_REG(0x0c10), ISP_REG(0x0c14), ISP_REG(0x0c18), ISP_REG(0x0c1c),
    ISP_REG(0x0c20), ISP_REG(0x0c24), ISP_REG(0x0c30), ISP_REG(0x0c34),
    ISP_REG(0x0c38), ISP_REG(0x0c3c), ISP_REG(0x0c40), ISP_REG(0x0c44),
    ISP_REG(0x0c48), ISP_REG(0x0c4c), ISP_REG(0x0c50), ISP_REG(0x0c54),
    ISP_REG(0x1000), ISP_REG(0x1004), ISP_REG(0x1008), ISP_REG(0x100c),
    ISP_REG(0x1010), ISP_REG(0x1014), ISP_REG(0x1018), ISP_REG(0x101c),
    ISP_REG(0x1020), ISP_REG(0x1024), ISP_REG(0x1030), ISP_REG(0x1034),
    ISP_REG(0x1038), ISP_REG(0x103c), ISP_REG(0x1040), ISP_REG(0x1044),
    ISP_REG(0x1048), ISP_REG(0x104c), ISP_REG(0x1050), ISP_REG(0x1054),
    ISP_REG(0x1408), ISP_REG(0x140c), ISP_REG(0x1410), ISP_REG(0x1418),
    ISP_REG(0x141c), ISP_REG(0x1420), ISP_REG(0x1424), ISP_REG(0x142c),
    ISP_REG(0x1430), ISP_REG(0x1434), ISP_REG(0x143c), ISP_REG(0x1440),
    ISP_REG(0x1444), ISP_REG(0x1450), ISP_REG(0x1454), ISP_REG(0x1458),
    ISP_REG(0x1460), ISP_REG(0x1464), ISP_REG(0x1468),
    ISP_REG(0x1474), ISP_REG(0x1478), ISP_REG(0x147c), ISP_REG(0x1480),
    ISP_REG(0x1484), ISP_REG(0x1488), ISP_REG(0x148c), ISP_REG(0x1490),
    ISP_REG(0x1494), ISP_REG(0x1498), ISP_REG(0x149c), ISP_REG(0x14a0),
    ISP_REG(0x14a4), ISP_REG(0x14a8), ISP_REG(0x14b0), ISP_REG(0x14b4),
    ISP_REG(0x14b8), ISP_REG(0x14bc), ISP_REG(0x14c0), ISP_REG(0x14c4),
    ISP_REG(0x14f8), ISP_REG(0x1514), ISP_REG(0x1518), ISP_REG(0x1548),
};

#undef ISP_REG

static RegisterInfo *isp_find_register(RegisterInfoArray *reg_array,
                                       hwaddr offset)
{
    for (unsigned i = 0; i < reg_array->num_elements; i++) {
        RegisterInfo *reg = reg_array->r[i];

        if (reg->access->addr == offset) {
            return reg;
        }
    }
    return NULL;
}

static uint64_t isp_read(void *opaque, hwaddr address, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566ISPState *s = RK3566_ISP(register_array_get_owner(reg_array));

    if (isp_find_register(reg_array, address)) {
        return register_read_memory(reg_array, address, size);
    }
    return s->regs[address / 4];
}

static void isp_write(void *opaque, hwaddr address, uint64_t value,
                      unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566ISPState *s = RK3566_ISP(register_array_get_owner(reg_array));

    if (isp_find_register(reg_array, address)) {
        register_write_memory(reg_array, address, value, size);
    } else {
        s->regs[address / 4] = value;
    }
    switch (address) {
    case ISP_RIS:
    case ISP_MIS:
    case MI_RIS:
    case MI_MIS:
    case ISP_FRAME_COUNT:
        return;
    default:
        isp_update_irq(s);
    }
}

static const MemoryRegionOps isp_ops = {
    .read = isp_read,
    .write = isp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void isp_reset(DeviceState *device)
{
    RK3566ISPState *s = RK3566_ISP(device);

    timer_del(s->frame_end);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->active, 0, sizeof(s->active));
    for (unsigned i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    s->input_count = 0;
    s->input_frames = 0;
    s->busy = false;
    isp_update_irq(s);
}

static void isp_realize(DeviceState *device, Error **errp)
{
    RK3566ISPState *s = RK3566_ISP(device);
    if (!s->dma || !s->pixel_rate_hz || !s->width ||
        !s->height || (s->width & 1) ||
        s->width > 4096 || s->height > 4096) {
        error_setg(errp, "rk3566-isp requires dma memory "
                   "and even input width <=4096");
        return;
    }
    address_space_init(&s->dma_as, s->dma, "rk3566-isp-dma");
    s->input_size = s->width * s->height * 2;
    s->input = g_malloc(s->input_size);
    s->frame = g_malloc(s->input_size);
    qemu_chr_fe_set_handlers(&s->camera, isp_can_receive, isp_receive,
                             isp_camera_event, NULL, s, NULL, true);
}

static void isp_init(Object *object)
{
    RK3566ISPState *s = RK3566_ISP(object);

    s->frame_end = timer_new_ns(QEMU_CLOCK_VIRTUAL, isp_frame_end, s);
    s->reg_array = register_init_block32(DEVICE(object), isp_regs_info,
                                         ARRAY_SIZE(isp_regs_info),
                                         s->regs_info, s->regs,
                                         &isp_ops, false, ISP_REG_WINDOW);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->reg_array->mem);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(s), isp_sensor_stream, "sensor-stream", 1);
    qdev_init_gpio_in_named(DEVICE(s), isp_link_enable, "link-enabled", 1);
    object_property_add_uint64_ptr(object, "input-frames", &s->input_frames,
                                   OBJ_PROP_FLAG_READ);
}

static void isp_finalize(Object *object)
{
    RK3566ISPState *s = RK3566_ISP(object);
    timer_free(s->frame_end);
    if (s->dma_as.root) {
        address_space_destroy(&s->dma_as);
    }
    g_free(s->input);
    g_free(s->frame);
}

static const VMStateDescription isp_vmstate = {
    .name = TYPE_RK3566_ISP, .unmigratable = true,
};

static const Property isp_properties[] = {
    DEFINE_PROP_CHR("chardev", RK3566ISPState, camera),
    DEFINE_PROP_LINK("dma", RK3566ISPState, dma,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT32("input-width", RK3566ISPState, width, 640),
    DEFINE_PROP_UINT32("input-height", RK3566ISPState, height, 360),
    DEFINE_PROP_UINT32("pixel-rate-hz", RK3566ISPState,
                       pixel_rate_hz, 118800000),
};

static void isp_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    device->realize = isp_realize;
    device->vmsd = &isp_vmstate;
    device_class_set_legacy_reset(device, isp_reset);
    device_class_set_props(device, isp_properties);
}

static void gc2093_reset(DeviceState *device)
{
    GC2093State *s = GC2093(device);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x3f0] = 0x20;
    s->regs[0x3f1] = 0x93;
    s->regs[0x3e] = 0x11;
    s->address = s->address_bytes = 0;
    qemu_set_irq(s->streaming, 0);
}

static int gc2093_event(I2CSlave *slave, enum i2c_event event)
{
    GC2093State *s = GC2093(slave);
    if ((event == I2C_START_SEND || event == I2C_START_RECV) &&
        (!s->powered || !s->resetn)) {
        return 1;
    }
    if (event == I2C_START_SEND) {
        s->address_bytes = 0;
    }
    return 0;
}

static int gc2093_send(I2CSlave *slave, uint8_t value)
{
    GC2093State *s = GC2093(slave);
    if (s->address_bytes < 2) {
        s->address = (s->address << 8) | value;
        s->address_bytes++;
    } else {
        if (s->address == 0x3fe && value == 0xf0) {
            gc2093_reset(DEVICE(s));
            return 0;
        }
        if (s->address != 0x3f0 && s->address != 0x3f1) {
            s->regs[s->address] = value;
        }
        if (s->address == 0x3e) {
            qemu_set_irq(s->streaming,
                         s->powered && s->resetn && value == 0x91);
        }
        s->address++;
    }
    return 0;
}

static uint8_t gc2093_recv(I2CSlave *slave)
{
    GC2093State *s = GC2093(slave);
    return s->regs[s->address++];
}

static void gc2093_power(void *opaque, int line, int level)
{
    GC2093State *s = GC2093(opaque);
    s->powered = level;
    if (!level) {
        gc2093_reset(DEVICE(s));
    }
}

static void gc2093_resetn(void *opaque, int line, int level)
{
    GC2093State *s = GC2093(opaque);
    s->resetn = level;
    if (!level) {
        gc2093_reset(DEVICE(s));
    }
}

static void gc2093_init(Object *object)
{
    qdev_init_gpio_out_named(DEVICE(object), &GC2093(object)->streaming,
                             "streaming", 1);
    qdev_init_gpio_in_named(DEVICE(object), gc2093_power, "power", 1);
    qdev_init_gpio_in_named(DEVICE(object), gc2093_resetn, "resetn", 1);
}

static const VMStateDescription gc2093_vmstate = {
    .name = TYPE_GC2093, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, GC2093State),
        VMSTATE_UINT8_ARRAY(regs, GC2093State, 0x10000),
        VMSTATE_UINT16(address, GC2093State),
        VMSTATE_UINT8(address_bytes, GC2093State),
        VMSTATE_BOOL(powered, GC2093State),
        VMSTATE_BOOL(resetn, GC2093State),
        VMSTATE_END_OF_LIST()
    },
};

static void gc2093_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    I2CSlaveClass *slave = I2C_SLAVE_CLASS(class);
    device_class_set_legacy_reset(device, gc2093_reset);
    device->vmsd = &gc2093_vmstate;
    slave->event = gc2093_event;
    slave->send = gc2093_send;
    slave->recv = gc2093_recv;
}

static void dphy_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566DPHYState *s = RK3566_DPHY(reg->opaque);

    qemu_set_irq(s->enabled, (val & 0x4c) == 0x4c);
}

static const RegisterAccessInfo dphy_regs_info[] = {
    { .name = "LANE_ENABLE", .addr = 0, .reset = 1,
      .post_write = dphy_ctrl_post_write },
};

static uint64_t dphy_read(void *opaque, hwaddr address, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566DPHYState *s = RK3566_DPHY(register_array_get_owner(reg_array));

    if (address == 0) {
        return register_read_memory(reg_array, address, size);
    }
    return s->regs[address / 4];
}

static void dphy_write(void *opaque, hwaddr address, uint64_t value,
                       unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566DPHYState *s = RK3566_DPHY(register_array_get_owner(reg_array));

    if (address == 0) {
        register_write_memory(reg_array, address, value, size);
        return;
    }
    s->regs[address / 4] = value;
}

static const MemoryRegionOps dphy_ops = {
    .read = dphy_read, .write = dphy_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void dphy_reset(DeviceState *device)
{
    RK3566DPHYState *s = RK3566_DPHY(device);

    memset(s->regs, 0, sizeof(s->regs));
    register_reset(&s->regs_info[DPHY_REG_CTRL]);
    qemu_set_irq(s->enabled, 0);
}

static void dphy_init(Object *object)
{
    RK3566DPHYState *s = RK3566_DPHY(object);

    s->reg_array = register_init_block32(DEVICE(object), dphy_regs_info,
                                         ARRAY_SIZE(dphy_regs_info),
                                         s->regs_info, s->regs,
                                         &dphy_ops, false, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->reg_array->mem);
    qdev_init_gpio_out_named(DEVICE(s), &s->enabled, "link-enabled", 1);
}

static const VMStateDescription dphy_vmstate = {
    .name = TYPE_RK3566_DPHY, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3566DPHYState, 0x1000 / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void dphy_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    device_class_set_legacy_reset(device, dphy_reset);
    device->vmsd = &dphy_vmstate;
}

static const TypeInfo camera_types[] = {
    { .name = TYPE_RK3566_ISP, .parent = TYPE_SYS_BUS_DEVICE,
      .instance_size = sizeof(RK3566ISPState), .instance_init = isp_init,
      .instance_finalize = isp_finalize, .class_init = isp_class_init },
    { .name = TYPE_GC2093, .parent = TYPE_I2C_SLAVE,
      .instance_size = sizeof(GC2093State), .instance_init = gc2093_init,
      .class_init = gc2093_class_init },
    { .name = TYPE_RK3566_DPHY, .parent = TYPE_SYS_BUS_DEVICE,
      .instance_size = sizeof(RK3566DPHYState), .instance_init = dphy_init,
      .class_init = dphy_class_init },
};

DEFINE_TYPES(camera_types)
