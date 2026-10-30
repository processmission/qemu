/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RK3566 VEPU540 H.264 interface with x264 encoding and libavcodec
 * statistics.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <x264.h>

#define TYPE_RK3566_VENC "rk3566-rkvenc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566VENCState, RK3566_VENC)

enum {
    VERSION = 0x000, START = 0x004, CLEAR = 0x008,
    INT_EN = 0x010, INT_MASK = 0x014, INT_CLEAR = 0x018, INT_STATUS = 0x01c,
    RESOLUTION = 0x030, PICTURE = 0x034, SOURCE_FORMAT = 0x044,
    SOURCE_PROCESS = 0x058, SOURCE_OFFSET = 0x110, SOURCE_STRIDE = 0x114,
    SOURCE0 = 0x118, SOURCE1 = 0x11c, SOURCE2 = 0x120,
    STREAM_TOP = 0x14c, STREAM_BOTTOM = 0x150, STREAM_READ = 0x154,
    STREAM_START = 0x158, SLICE_SPLIT = 0x15c,
    SYNTAX_NAL = 0x19c, SYNTAX_PPS = 0x1a4, SYNTAX_SLICE = 0x1a8,
    STREAM_LENGTH = 0x210, SSE_LOW = 0x214, SSE_QP = 0x218,
    ENCODE_STATUS = 0x228, STREAM_WRITE = 0x234, BUS_STATUS = 0x238,
    SLICE_COUNT = 0x23c, SLICE_LENGTH = 0x240,
    L2_ADDRESS = 0x3f0, L2_WRITE = 0x3f4, L2_READ = 0x3f8,
    CACHE_CLEAR = 0x884,
};

#define IRQ_FRAME BIT(0)
#define IRQ_CLEAR BIT(2)
#define IRQ_OVERFLOW BIT(4)
#define IRQ_WRITE_ERROR BIT(6)
#define IRQ_READ_ERROR BIT(7)
#define IRQ_TIMEOUT BIT(8)
#define REG(s, offset) ((s)->regs[(offset) / 4])

struct RK3566VENCState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion *dma;
    MemoryRegion *source_dma;
    AddressSpace dma_as;
    AddressSpace source_as;
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t regs[0x1000 / 4];
    uint32_t l2[320];
    uint64_t completed, failed, encoded_bytes;
    bool busy;
    x264_t *encoder;
    AVCodecContext *decoder;
    struct SwsContext *conversion;
    uint32_t sequence;
    unsigned profile;
    unsigned width, height;
};

static void venc_update_irq(RK3566VENCState *s)
{
    qemu_set_irq(s->irq, (REG(s, INT_STATUS) & REG(s, INT_EN) &
                          ~REG(s, INT_MASK)) != 0);
}

static void venc_close_codecs(RK3566VENCState *s)
{
    if (s->encoder) {
        x264_encoder_close(s->encoder);
        s->encoder = NULL;
    }
    avcodec_free_context(&s->decoder);
    sws_freeContext(s->conversion);
    s->conversion = NULL;
    s->sequence = 0;
}

static int venc_open_codecs(RK3566VENCState *s, unsigned width, unsigned height,
                            unsigned qp)
{
    unsigned profile = (REG(s, SYNTAX_PPS) & 2) ? AV_PROFILE_H264_HIGH :
                       (REG(s, SYNTAX_PPS) & 1) ? AV_PROFILE_H264_MAIN :
                                                  AV_PROFILE_H264_BASELINE;
    x264_param_t parameters;
    int result;

    if (s->encoder && s->width == width && s->height == height &&
        s->profile == profile) {
        return 0;
    }
    venc_close_codecs(s);
    s->decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    if (!s->decoder) {
        return AVERROR(ENOMEM);
    }
    if (x264_param_default_preset(&parameters, "veryfast", "zerolatency") < 0) {
        return AVERROR(EINVAL);
    }
    parameters.i_width = width;
    parameters.i_height = height;
    parameters.i_csp = X264_CSP_I420;
    parameters.i_bitdepth = 8;
    parameters.i_threads = 1;
    parameters.i_fps_num = 30;
    parameters.i_fps_den = 1;
    parameters.i_timebase_num = 1;
    parameters.i_timebase_den = 30;
    parameters.i_keyint_max = 65535;
    parameters.i_scenecut_threshold = 0;
    parameters.b_repeat_headers = 1;
    parameters.b_annexb = 1;
    parameters.b_cabac = REG(s, SYNTAX_PPS) & 1;
    parameters.analyse.b_transform_8x8 = !!(REG(s, SYNTAX_PPS) & 2);
    /*
     * Per-picture i_qpplus1 supplies the guest's quantizer without CQP
     * clipping.
     */
    parameters.rc.i_rc_method = X264_RC_CRF;
    parameters.rc.f_rf_constant = qp ? qp : 26;
    parameters.rc.i_qp_min = 0;
    parameters.rc.i_qp_max = 51;
    parameters.rc.i_qp_step = 51;
    parameters.rc.b_mb_tree = 0;
    parameters.rc.f_ip_factor = 1;
    parameters.rc.f_pb_factor = 1;
    parameters.rc.i_aq_mode = X264_AQ_NONE;
    parameters.i_log_level = X264_LOG_WARNING;
    if (x264_param_apply_profile(&parameters,
             profile == AV_PROFILE_H264_HIGH ? "high" :
             profile == AV_PROFILE_H264_MAIN ? "main" : "baseline") < 0) {
        x264_param_cleanup(&parameters);
        return AVERROR(EINVAL);
    }
    s->encoder = x264_encoder_open(&parameters);
    x264_param_cleanup(&parameters);
    if (!s->encoder) {
        return AVERROR(EIO);
    }
    s->decoder->thread_count = 1;
    s->decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    result = avcodec_open2(s->decoder, s->decoder->codec, NULL);
    if (result >= 0) {
        s->profile = profile;
        s->width = width;
        s->height = height;
    }
    return result;
}

static enum AVPixelFormat venc_source_format(uint32_t value)
{
    bool swap = value & 2;

    switch ((value >> 2) & 15) {
    case 0:
        return swap ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
    case 1:
        return swap ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_BGR24;
    case 2:
        return swap ? AV_PIX_FMT_RGB565LE : AV_PIX_FMT_BGR565LE;
    case 4:
        return AV_PIX_FMT_NV16;
    case 5:
        return AV_PIX_FMT_YUV422P;
    case 6:
        return swap ? AV_PIX_FMT_NV21 : AV_PIX_FMT_NV12;
    case 7:
        return AV_PIX_FMT_YUV420P;
    case 8:
        return swap ? AV_PIX_FMT_YVYU422 : AV_PIX_FMT_YUYV422;
    case 9:
        return AV_PIX_FMT_UYVY422;
    default:
        return AV_PIX_FMT_NONE;
    }
}

static bool venc_read_source(RK3566VENCState *s, AVFrame *frame,
                             unsigned width, unsigned height)
{
    enum AVPixelFormat format = venc_source_format(REG(s, SOURCE_FORMAT));
    const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(format);
    uint8_t *planes[4] = { NULL };
    int strides[4], max_step[4], max_component[4];
    unsigned y_offset = REG(s, SOURCE_OFFSET) & 0x1fff;
    unsigned x_offset = (REG(s, SOURCE_OFFSET) >> 16) & 0x1fff;
    int result, plane_count;
    bool success = false;

    if (!description || (REG(s, SOURCE_PROCESS) & (0x1c000000 | BIT(30)))) {
        qemu_log_mask(LOG_UNIMP, TYPE_RK3566_VENC
                      ": source requires an unsupported format, transform or AFBC\n");
        return false;
    }
    result = av_image_alloc(planes, strides, width, height, format, 32);
    if (result < 0) {
        return false;
    }
    av_image_fill_max_pixsteps(max_step, max_component, description);
    plane_count = av_pix_fmt_count_planes(format);
    for (unsigned plane = 0; plane < plane_count; plane++) {
        bool chroma = plane == 1 || plane == 2;
        unsigned shift_y = chroma ? description->log2_chroma_h : 0;
        unsigned shift_x = chroma ? description->log2_chroma_w : 0;
        unsigned rows = AV_CEIL_RSHIFT((int)height, shift_y);
        unsigned guest_stride =
            (REG(s, SOURCE_STRIDE) >> (chroma ? 16 : 0)) & 0xffff;
        unsigned row_bytes = av_image_get_linesize(format, width, plane);
        uint64_t address = REG(s, SOURCE0 + plane * 4);

        if (!guest_stride || guest_stride < row_bytes || guest_stride > 32768) {
            goto done;
        }
        address += (uint64_t)(y_offset >> shift_y) * guest_stride +
                   (x_offset >> shift_x) * max_step[plane];
        for (unsigned row = 0; row < rows; row++) {
            if (dma_memory_read(&s->source_as,
                                address + (uint64_t)row * guest_stride,
                                planes[plane] + row * strides[plane], row_bytes,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                goto done;
            }
        }
    }
    s->conversion = sws_getCachedContext(s->conversion, width, height, format,
                                         width, height, AV_PIX_FMT_YUV420P,
                                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->conversion) {
        goto done;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        goto done;
    }
    success = sws_scale(s->conversion, (const uint8_t *const *)planes, strides,
                        0, height, frame->data, frame->linesize) == height;
done:
    av_freep(&planes[0]);
    return success;
}

static bool venc_statistics(RK3566VENCState *s, const AVFrame *input,
                            const AVPacket *packet, unsigned encoded_qp)
{
    AVFrame *decoded = av_frame_alloc();
    unsigned macroblocks = ((input->width + 15) / 16) *
                           ((input->height + 15) / 16);
    uint64_t sse = 0;
    uint32_t qp_sum = encoded_qp * macroblocks;
    bool success = false;

    if (!decoded || avcodec_send_packet(s->decoder, packet) < 0 ||
        avcodec_receive_frame(s->decoder, decoded) < 0) {
        goto done;
    }
    if (encoded_qp > 51 || decoded->width != input->width ||
        decoded->height != input->height ||
        decoded->format != AV_PIX_FMT_YUV420P) {
        goto done;
    }
    /* AQ is disabled: the encoder's output QP applies to every macroblock. */
    for (unsigned row = 0; row < input->height; row++) {
        const uint8_t *source = input->data[0] + row * input->linesize[0];
        const uint8_t *reconstructed = decoded->data[0] +
                                       row * decoded->linesize[0];

        for (unsigned column = 0; column < input->width; column++) {
            int difference = source[column] - reconstructed[column];

            sse += difference * difference;
        }
    }
    REG(s, SSE_LOW) = sse;
    REG(s, SSE_QP) = (qp_sum & 0x3fffff) | ((sse >> 32) << 24);
    success = true;
done:
    av_frame_free(&decoded);
    return success;
}

static void venc_finish(RK3566VENCState *s, uint32_t interrupt)
{
    s->busy = false;
    REG(s, ENCODE_STATUS) = 0;
    REG(s, BUS_STATUS) = 0x7e;
    REG(s, START) &= ~(3 << 8);
    REG(s, INT_STATUS) |= interrupt;
    if (interrupt & IRQ_FRAME) {
        s->completed++;
    } else {
        s->failed++;
    }
    venc_update_irq(s);
}

static void venc_encode(void *opaque)
{
    RK3566VENCState *s = opaque;
    uint32_t resolution = REG(s, RESOLUTION);
    unsigned width = ((resolution & 0x1ff) + 1) * 8;
    unsigned height = (((resolution >> 16) & 0x1ff) + 1) * 8;
    unsigned x_fill = (resolution >> 10) & 63;
    unsigned y_fill = resolution >> 26;
    unsigned qp = (REG(s, PICTURE) >> 8) & 63;
    unsigned nal_type = (REG(s, SYNTAX_NAL) >> 2) & 31;
    uint64_t top = (uint64_t)REG(s, STREAM_TOP) + 1;
    uint64_t output = REG(s, STREAM_START);
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    x264_picture_t input_picture, output_picture;
    x264_nal_t *nals;
    int nal_count, packet_offset = 0;
    uint32_t interrupt = IRQ_READ_ERROR;
    int result;

    if (!frame || !packet || x_fill >= width || y_fill >= height ||
        width > 1920 || height > 1088 || (REG(s, PICTURE) & 1) ||
        (REG(s, SYNTAX_SLICE) & 3) == 1) {
        goto done;
    }
    width -= x_fill;
    height -= y_fill;
    if ((width & 1) || (height & 1) || qp > 51) {
        goto done;
    }
    result = venc_open_codecs(s, width, height, qp);
    if (result < 0 || !venc_read_source(s, frame, width, height)) {
        goto done;
    }
    x264_picture_init(&input_picture);
    x264_picture_init(&output_picture);
    input_picture.i_pts = s->sequence++;
    input_picture.i_qpplus1 = qp + 1;
    input_picture.i_type = nal_type == 5 ? X264_TYPE_IDR :
                           (REG(s, SYNTAX_SLICE) & 3) == 2 ?
                           X264_TYPE_I : X264_TYPE_P;
    input_picture.img.i_csp = X264_CSP_I420;
    input_picture.img.i_plane = 3;
    for (unsigned i = 0; i < 3; i++) {
        input_picture.img.plane[i] = frame->data[i];
        input_picture.img.i_stride[i] = frame->linesize[i];
    }
    result = x264_encoder_encode(s->encoder, &nals, &nal_count,
                                 &input_picture, &output_picture);
    if (result <= 0 || av_new_packet(packet, result) < 0) {
        goto done;
    }
    for (unsigned i = 0; i < nal_count; i++) {
        if (nals[i].i_payload > packet->size - packet_offset) {
            goto done;
        }
        memcpy(packet->data + packet_offset, nals[i].p_payload,
               nals[i].i_payload);
        packet_offset += nals[i].i_payload;
    }
    if (packet_offset != packet->size || output_picture.i_qpplus1 <= 0 ||
        !venc_statistics(s, frame, packet, output_picture.i_qpplus1 - 1)) {
        goto done;
    }
    interrupt = IRQ_OVERFLOW;
    if (output < REG(s, STREAM_BOTTOM) || output >= top ||
        packet->size > top - output) {
        goto done;
    }
    interrupt = IRQ_WRITE_ERROR;
    if (dma_memory_write(&s->dma_as, output, packet->data, packet->size,
                          MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        goto done;
    }
    REG(s, STREAM_LENGTH) = packet->size;
    REG(s, STREAM_WRITE) = output + packet->size;
    REG(s, SLICE_COUNT) = 1;
    REG(s, SLICE_LENGTH) = packet->size;
    s->encoded_bytes += packet->size;
    interrupt = IRQ_FRAME;
done:
    if (interrupt != IRQ_FRAME) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_RK3566_VENC
                      ": encoding failed, interrupt=0x%x format=0x%x source=0x%x\n",
                      interrupt, REG(s, SOURCE_FORMAT), REG(s, SOURCE0));
        venc_close_codecs(s);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    venc_finish(s, interrupt);
}

static uint64_t venc_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566VENCState *s = opaque;

    if (offset == L2_READ) {
        unsigned index = (REG(s, L2_ADDRESS) & ~1u) / 4;

        return index < ARRAY_SIZE(s->l2) ? s->l2[index] : 0;
    }
    if (offset == SLICE_LENGTH) {
        REG(s, SLICE_COUNT) = 0;
    }
    return REG(s, offset);
}

static void venc_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    RK3566VENCState *s = opaque;

    if (offset == VERSION || (offset >= STREAM_LENGTH && offset <= 0x2fc)) {
        return;
    }
    switch (offset) {
    case INT_CLEAR:
        REG(s, INT_STATUS) &= ~value;
        break;
    case INT_STATUS:
        if (!value) {
            REG(s, INT_STATUS) = 0;
        } else {
            REG(s, INT_STATUS) &= ~value;
        }
        break;
    case CLEAR:
        timer_del(s->timer);
        s->busy = false;
        REG(s, ENCODE_STATUS) = 0;
        REG(s, START) &= ~(3 << 8);
        if (value & BIT(1)) {
            venc_close_codecs(s);
        }
        if (value & BIT(0)) {
            REG(s, INT_STATUS) |= IRQ_CLEAR;
        }
        break;
    case START:
        REG(s, START) = value;
        if (((value >> 8) & 3) == 1 && !s->busy) {
            s->busy = true;
            REG(s, ENCODE_STATUS) = 1;
            REG(s, STREAM_LENGTH) = 0;
            REG(s, INT_STATUS) = 0;
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
        } else if ((value >> 8) & 3) {
            REG(s, INT_STATUS) |= IRQ_TIMEOUT;
        }
        break;
    case L2_WRITE: {
        unsigned index = (REG(s, L2_ADDRESS) & ~1u) / 4;

        if (index < ARRAY_SIZE(s->l2)) {
            s->l2[index] = value;
        }
        break;
    }
    case CACHE_CLEAR:
        break;
    default:
        REG(s, offset) = value;
        break;
    }
    venc_update_irq(s);
}

static const MemoryRegionOps venc_ops = {
    .read = venc_read, .write = venc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void venc_reset(DeviceState *dev)
{
    RK3566VENCState *s = RK3566_VENC(dev);

    timer_del(s->timer);
    venc_close_codecs(s);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->l2, 0, sizeof(s->l2));
    /* VEPU5 register layout, H.264, 1920x1088, no OSD/filter/AFBC. */
    REG(s, VERSION) = 0x500f1111;
    REG(s, BUS_STATUS) = 0x7e;
    s->busy = false;
    s->completed = s->failed = s->encoded_bytes = 0;
    venc_update_irq(s);
}

static void venc_init(Object *obj)
{
    RK3566VENCState *s = RK3566_VENC(obj);

    memory_region_init_io(&s->mmio, obj, &venc_ops, s, TYPE_RK3566_VENC,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, venc_encode, s);
}

static void venc_realize(DeviceState *dev, Error **errp)
{
    RK3566VENCState *s = RK3566_VENC(dev);

    if (!s->dma || !avcodec_find_decoder(AV_CODEC_ID_H264)) {
        error_setg(errp, "rk3566-rkvenc requires DMA memory and a "
                   "libavcodec H.264 decoder");
        return;
    }
    address_space_init(&s->dma_as, s->dma, "rk3566-venc-output");
    address_space_init(&s->source_as, s->source_dma ? s->source_dma : s->dma,
                       "rk3566-venc-source");
}

static void venc_finalize(Object *obj)
{
    RK3566VENCState *s = RK3566_VENC(obj);

    timer_free(s->timer);
    venc_close_codecs(s);
    if (s->dma_as.root) {
        address_space_destroy(&s->dma_as);
    }
    if (s->source_as.root) {
        address_space_destroy(&s->source_as);
    }
}

static const Property venc_properties[] = {
    DEFINE_PROP_LINK("dma", RK3566VENCState, dma, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("source-dma", RK3566VENCState, source_dma,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription vmstate_venc = {
    .name = TYPE_RK3566_VENC,
    .unmigratable = true,
};

static void venc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = venc_realize;
    dc->vmsd = &vmstate_venc;
    device_class_set_legacy_reset(dc, venc_reset);
    device_class_set_props(dc, venc_properties);
}

static const TypeInfo venc_type = {
    .name = TYPE_RK3566_VENC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566VENCState),
    .instance_init = venc_init, .instance_finalize = venc_finalize,
    .class_init = venc_class_init,
};

static void venc_register(void)
{
    type_register_static(&venc_type);
}
type_init(venc_register)
