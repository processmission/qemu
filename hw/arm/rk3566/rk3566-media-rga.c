/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RK3566 RGA2 command DMA, page translation and image conversion. */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#define TYPE_RK3566_RGA "rk3566-rga2"
#define RK3566_RGA_MMIO_SIZE 0x1000
#define RK3566_RGA_VERSION 0x02000000
#define RK3566_RGA_NUM_WORDS (RK3566_RGA_MMIO_SIZE / 4)

OBJECT_DECLARE_SIMPLE_TYPE(RK3566RGAState, RK3566_RGA)

REG32(SYS_CTRL, 0x00)
REG32(CMD_CTRL, 0x04)
REG32(CMD_BASE, 0x08)
REG32(STATUS, 0x0c)
REG32(INTERRUPT, 0x10)
REG32(VERSION, 0x28)
REG32(COMMAND, 0x100)

#define RK3566_RGA_COMMAND_WORDS 32
#define RK3566_RGA_NUM_REGS ((A_COMMAND / 4) + RK3566_RGA_COMMAND_WORDS)

enum {
    MODE = 0x00, SRC_INFO = 0x04, SRC_BASE = 0x08, SRC_VIR = 0x18,
    SRC_ACT = 0x1c, SRC_X_FACTOR = 0x20, SRC_Y_FACTOR = 0x24,
    FG_COLOR = 0x2c, DST_INFO = 0x38, DST_BASE = 0x3c, DST_VIR = 0x48,
    DST_ACT = 0x4c, ALPHA = 0x50, MMU_CTRL = 0x6c,
    MMU_SRC = 0x70, MMU_DST = 0x78,
};
#define CMD(s, address) ((s)->regs[(A_COMMAND + (address)) / 4])

struct RK3566RGAState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[RK3566_RGA_NUM_REGS];
    uint32_t regs[RK3566_RGA_NUM_WORDS];

    qemu_irq irq;
    QEMUTimer *timer;
    uint64_t completed;
    uint64_t failed;
    uint64_t max_bytes;
    bool busy;
};

typedef struct RGAImage {
    uint8_t *data[4];
    int stride[4];
    enum AVPixelFormat format;
    unsigned width, height;
    uint32_t info, base[3], guest_stride;
    uint32_t table;
    bool translated;
} RGAImage;

static void rga_update_irq(RK3566RGAState *s)
{
    uint32_t value = s->regs[R_INTERRUPT];
    qemu_set_irq(s->irq, (value & (value >> 8) & 7) != 0);
}

static bool rga_dma(uint32_t address, void *buffer, size_t length,
                    uint32_t table, bool translated, bool write)
{
    while (length) {
        hwaddr physical = address;
        size_t chunk = MIN(length, 0x1000 - (address & 0xfff));
        if (translated) {
            uint32_t entry;
            if (address_space_read(&address_space_memory,
                                   (hwaddr)table + ((address >> 12) * 4),
                                   MEMTXATTRS_UNSPECIFIED, &entry, 4) !=
                MEMTX_OK) {
                return false;
            }
            physical = (le32_to_cpu(entry) & ~0xfffu) | (address & 0xfff);
        }
        if (address_space_rw(&address_space_memory, physical,
                              MEMTXATTRS_UNSPECIFIED, buffer, chunk,
                              write) != MEMTX_OK) {
            return false;
        }
        address += chunk;
        buffer = (uint8_t *)buffer + chunk;
        length -= chunk;
    }
    return true;
}

static enum AVPixelFormat rga_format(uint32_t info)
{
    bool rb = info & BIT(4);
    bool alpha = info & BIT(5);
    bool uv = info & BIT(6);
    switch (info & 15) {
    case 0:
        return alpha ? (rb ? AV_PIX_FMT_ABGR : AV_PIX_FMT_ARGB) :
                       (rb ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA);
    case 1:
        return alpha ? (rb ? AV_PIX_FMT_0BGR : AV_PIX_FMT_0RGB) :
                       (rb ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0);
    case 2:
        return rb ? AV_PIX_FMT_BGR24 : AV_PIX_FMT_RGB24;
    case 4:
        return rb ? AV_PIX_FMT_BGR565LE : AV_PIX_FMT_RGB565LE;
    case 7:
        if (uv) {
            return rb ? AV_PIX_FMT_YVYU422 : AV_PIX_FMT_NONE;
        }
        return rb ? AV_PIX_FMT_YUYV422 : AV_PIX_FMT_UYVY422;
    case 8:
        return uv ? AV_PIX_FMT_NONE : AV_PIX_FMT_NV16;
    case 9:
        return AV_PIX_FMT_YUV422P;
    case 10:
        return uv ? AV_PIX_FMT_NV21 : AV_PIX_FMT_NV12;
    case 11:
        return AV_PIX_FMT_YUV420P;
    default:
        return AV_PIX_FMT_NONE;
    }
}

static bool rga_image_setup(RK3566RGAState *s, RGAImage *image,
                            bool destination)
{
    uint32_t active = CMD(s, destination ? DST_ACT : SRC_ACT);
    uint32_t base = destination ? DST_BASE : SRC_BASE;
    image->width = (active & 0x1fff) + 1;
    image->height = ((active >> 16) & 0x1fff) + 1;
    image->info = CMD(s, destination ? DST_INFO : SRC_INFO);
    image->format = rga_format(image->info);
    image->guest_stride =
        (CMD(s, destination ? DST_VIR : SRC_VIR) & 0x7fff) * 4;
    image->translated = CMD(s, MMU_CTRL) & BIT(destination ? 8 : 0);
    image->table = CMD(s, destination ? MMU_DST : MMU_SRC) << 4;
    for (unsigned i = 0; i < 3; i++) {
        image->base[i] = CMD(s, base + i * 4);
    }
    if ((image->info & BIT(6)) &&
        ((image->info & 15) == 9 || (image->info & 15) == 11)) {
        uint32_t swap = image->base[1];
        image->base[1] = image->base[2];
        image->base[2] = swap;
    }
    if (image->format == AV_PIX_FMT_NONE ||
        (uint64_t)image->width * image->height * 4 > s->max_bytes ||
        image->guest_stride <
        av_image_get_linesize(image->format, image->width, 0)) {
        return false;
    }
    return av_image_alloc(image->data, image->stride, image->width,
                           image->height, image->format, 16) >= 0;
}

static bool rga_image_transfer(RGAImage *image, bool write)
{
    const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(image->format);
    bool planar = description->flags & AV_PIX_FMT_FLAG_PLANAR;
    bool semiplanar = (image->info & 15) == 8 || (image->info & 15) == 10;
    unsigned planes = av_pix_fmt_count_planes(image->format);
    for (unsigned plane = 0; plane < planes; plane++) {
        unsigned rows = image->height;
        unsigned stride = image->guest_stride;
        unsigned row_bytes =
            av_image_get_linesize(image->format, image->width, plane);
        if (plane && planar) {
            rows = AV_CEIL_RSHIFT((int)rows, description->log2_chroma_h);
            if (!semiplanar) {
                stride = ROUND_UP(AV_CEIL_RSHIFT((int)stride,
                                                description->log2_chroma_w), 4);
            }
        }
        if (stride < row_bytes) {
            return false;
        }
        for (unsigned row = 0; row < rows; row++) {
            uint64_t address = (uint64_t)image->base[plane] +
                               (uint64_t)stride * row;
            if (address + row_bytes > UINT64_C(0x100000000) ||
                !rga_dma(address,
                          image->data[plane] + row * image->stride[plane],
                          row_bytes, image->table, image->translated, write)) {
                return false;
            }
        }
    }
    return true;
}

static bool rga_execute(RK3566RGAState *s)
{
    RGAImage source = {}, destination = {};
    struct SwsContext *scaler = NULL;
    uint8_t fill[4];
    bool success = false;
    uint32_t mode = CMD(s, MODE);
    uint32_t source_info = CMD(s, SRC_INFO);
    uint32_t destination_info = CMD(s, DST_INFO);

    /* Unsupported operations report the hardware error interrupt. */
    if ((mode & 7) != 0 && (mode & 7) != 2) {
        goto out;
    }
    if (mode & (BIT(3) | BIT(4) | BIT(5)) || CMD(s, ALPHA) & BIT(0) ||
        source_info & (0xfu << 10 | 0x1fu << 18 | BIT(27)) ||
        destination_info & (BIT(24) | BIT(25) | BIT(26))) {
        goto out;
    }
    if (!rga_image_setup(s, &destination, true)) {
        goto out;
    }
    if ((mode & 7) == 2) {
        if (CMD(s, 0x30) || CMD(s, 0x34) || CMD(s, 0x60) || CMD(s, 0x64)) {
            goto out;
        }
        stl_le_p(fill, CMD(s, FG_COLOR));
        source.data[0] = fill;
        source.stride[0] = 4;
        source.width = source.height = 1;
        source.format = AV_PIX_FMT_BGRA;
    } else if (!rga_image_setup(s, &source, false) ||
               !rga_image_transfer(&source, false)) {
        goto out;
    }
    scaler = sws_getContext(source.width, source.height, source.format,
                            destination.width, destination.height,
                            destination.format,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!scaler) {
        goto out;
    }
    unsigned input_csc = extract32(source_info, 8, 2);
    unsigned output_csc = extract32(destination_info, 16, 2);
    const int *input_coeff =
        sws_getCoefficients(input_csc == 2 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    const int *output_coeff =
        sws_getCoefficients(output_csc == 2 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    if (sws_setColorspaceDetails(scaler, input_coeff, input_csc == 1,
                                 output_coeff, output_csc == 1,
                                 0, 1 << 16, 1 << 16) < 0) {
        goto out;
    }
    if (sws_scale(scaler, (const uint8_t * const *)source.data, source.stride,
                   0, source.height, destination.data,
                   destination.stride) != destination.height) {
        goto out;
    }
    success = rga_image_transfer(&destination, true);
out:
    sws_freeContext(scaler);
    if (source.data[0] != fill) {
        av_freep(&source.data[0]);
    }
    av_freep(&destination.data[0]);
    return success;
}

static void rga_complete(void *opaque)
{
    RK3566RGAState *s = opaque;
    uint32_t command[32];
    bool success = false;
    if (rga_dma(s->regs[R_CMD_BASE], command, sizeof(command), 0, false,
                false)) {
        for (unsigned i = 0; i < ARRAY_SIZE(command); i++) {
            CMD(s, i * 4) = le32_to_cpu(command[i]);
        }
        success = rga_execute(s);
    }
    s->busy = false;
    s->regs[R_CMD_CTRL] &= ~1;
    if (success) {
        s->regs[R_INTERRUPT] |= BIT(2) | BIT(3);
        s->completed++;
    } else {
        s->regs[R_INTERRUPT] |= BIT(0);
        s->failed++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3566-rga2: DMA failure or unsupported image operation "
                      "mode=0x%x src=0x%x dst=0x%x\n",
                      CMD(s, MODE), CMD(s, SRC_INFO), CMD(s, DST_INFO));
    }
    rga_update_irq(s);
}

static uint64_t rga_sys_ctrl_pre_write(RegisterInfo *reg, uint64_t val)
{
    RK3566RGAState *s = RK3566_RGA(reg->opaque);

    if (val & BIT(3)) {
        timer_del(s->timer);
        s->busy = false;
        s->regs[R_CMD_CTRL] = 0;
        s->regs[R_INTERRUPT] &= ~0xf;
        rga_update_irq(s);
    }
    return val & ~BIT(3);
}

static uint64_t rga_sys_ctrl_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566RGAState *s = RK3566_RGA(reg->opaque);

    return (val & ~1u) | s->busy;
}

static uint64_t rga_status_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566RGAState *s = RK3566_RGA(reg->opaque);

    return s->busy;
}

static void rga_cmd_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566RGAState *s = RK3566_RGA(reg->opaque);

    if (!(val & 1)) {
        return;
    }
    if (s->busy) {
        s->regs[R_INTERRUPT] |= BIT(0);
        rga_update_irq(s);
    } else {
        s->busy = true;
        timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
    }
}

static uint64_t rga_interrupt_pre_write(RegisterInfo *reg, uint64_t val)
{
    RK3566RGAState *s = RK3566_RGA(reg->opaque);
    uint32_t status = s->regs[R_INTERRUPT];
    uint32_t clear = extract32(val, 4, 4);

    /* Bits 4..7 are write-one-to-clear for status bits 0..3. */
    return (status & 0xf & ~clear) | (val & 0x700);
}

static void rga_interrupt_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566RGAState *s = RK3566_RGA(reg->opaque);

    rga_update_irq(s);
}

/*
 * The 32 words at 0x100 are the command block that the DMA engine loads before
 * execution.  They stay plain storage for the guest.
 */
#define RGA_COMMAND_REG(_index) \
    { .name = "COMMAND" #_index, .addr = A_COMMAND + ((_index) * 4) }

static const RegisterAccessInfo rga_regs_info[] = {
    {
        .name = "SYS_CTRL",
        .addr = A_SYS_CTRL,
        .pre_write = rga_sys_ctrl_pre_write,
        .post_read = rga_sys_ctrl_post_read,
    }, {
        .name = "CMD_CTRL",
        .addr = A_CMD_CTRL,
        .post_write = rga_cmd_ctrl_post_write,
    }, {
        .name = "CMD_BASE",
        .addr = A_CMD_BASE,
    }, {
        .name = "STATUS",
        .addr = A_STATUS,
        .ro = UINT32_MAX,
        .post_read = rga_status_post_read,
    }, {
        .name = "INTERRUPT",
        .addr = A_INTERRUPT,
        .pre_write = rga_interrupt_pre_write,
        .post_write = rga_interrupt_post_write,
    }, {
        .name = "VERSION",
        .addr = A_VERSION,
        .reset = RK3566_RGA_VERSION,
        .ro = UINT32_MAX,
    },
    RGA_COMMAND_REG(0),
    RGA_COMMAND_REG(1),
    RGA_COMMAND_REG(2),
    RGA_COMMAND_REG(3),
    RGA_COMMAND_REG(4),
    RGA_COMMAND_REG(5),
    RGA_COMMAND_REG(6),
    RGA_COMMAND_REG(7),
    RGA_COMMAND_REG(8),
    RGA_COMMAND_REG(9),
    RGA_COMMAND_REG(10),
    RGA_COMMAND_REG(11),
    RGA_COMMAND_REG(12),
    RGA_COMMAND_REG(13),
    RGA_COMMAND_REG(14),
    RGA_COMMAND_REG(15),
    RGA_COMMAND_REG(16),
    RGA_COMMAND_REG(17),
    RGA_COMMAND_REG(18),
    RGA_COMMAND_REG(19),
    RGA_COMMAND_REG(20),
    RGA_COMMAND_REG(21),
    RGA_COMMAND_REG(22),
    RGA_COMMAND_REG(23),
    RGA_COMMAND_REG(24),
    RGA_COMMAND_REG(25),
    RGA_COMMAND_REG(26),
    RGA_COMMAND_REG(27),
    RGA_COMMAND_REG(28),
    RGA_COMMAND_REG(29),
    RGA_COMMAND_REG(30),
    RGA_COMMAND_REG(31),
};

#undef RGA_COMMAND_REG

static RegisterInfo *rga_find_register(RegisterInfoArray *reg_array,
                                       hwaddr addr)
{
    for (unsigned int i = 0; i < reg_array->num_elements; i++) {
        RegisterInfo *reg = reg_array->r[i];

        if (reg->access->addr == addr) {
            return reg;
        }
    }
    return NULL;
}

static uint64_t rga_read(void *opaque, hwaddr addr, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566RGAState *s = RK3566_RGA(register_array_get_owner(reg_array));

    if (rga_find_register(reg_array, addr)) {
        return register_read_memory(reg_array, addr, size);
    }
    return extract64(s->regs[addr / sizeof(uint32_t)], 0, size * 8);
}

static void rga_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566RGAState *s = RK3566_RGA(register_array_get_owner(reg_array));
    uint32_t *reg = &s->regs[addr / sizeof(uint32_t)];
    uint64_t mask;

    if (rga_find_register(reg_array, addr)) {
        register_write_memory(reg_array, addr, value, size);
        return;
    }
    mask = MAKE_64BIT_MASK(0, size * 8);
    *reg = (*reg & ~mask) | (value & mask);
}

static const MemoryRegionOps rga_ops = {
    .read = rga_read,
    .write = rga_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rga_reset(DeviceState *device)
{
    RK3566RGAState *s = RK3566_RGA(device);
    unsigned int i;

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < ARRAY_SIZE(rga_regs_info); i++) {
        register_reset(&s->regs_info[rga_regs_info[i].addr / 4]);
    }
    s->busy = false;
    s->completed = s->failed = 0;
    rga_update_irq(s);
}

static void rga_init(Object *object)
{
    RK3566RGAState *s = RK3566_RGA(object);
    SysBusDevice *sbd = SYS_BUS_DEVICE(object);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rga_complete, s);
    s->reg_array = register_init_block32(DEVICE(object), rga_regs_info,
                                         ARRAY_SIZE(rga_regs_info),
                                         s->regs_info, s->regs, &rga_ops,
                                         false, RK3566_RGA_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
}

static void rga_finalize(Object *object)
{
    timer_free(RK3566_RGA(object)->timer);
}

static const VMStateDescription rga_vmstate = {
    .name = TYPE_RK3566_RGA, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3566RGAState, RK3566_RGA_NUM_WORDS),
        VMSTATE_BOOL(busy, RK3566RGAState),
        VMSTATE_UINT64(completed, RK3566RGAState),
        VMSTATE_UINT64(failed, RK3566RGAState),
        VMSTATE_TIMER_PTR(timer, RK3566RGAState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rga_properties[] = {
    DEFINE_PROP_UINT64("max-host-bytes", RK3566RGAState, max_bytes, 256 * MiB),
};

static void rga_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    device->vmsd = &rga_vmstate;
    device_class_set_legacy_reset(device, rga_reset);
    device_class_set_props(device, rga_properties);
}

static const TypeInfo rga_type = {
    .name = TYPE_RK3566_RGA, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566RGAState), .instance_init = rga_init,
    .instance_finalize = rga_finalize, .class_init = rga_class_init,
};

static void register_rga_type(void)
{
    type_register_static(&rga_type);
}

type_init(register_rga_type)
