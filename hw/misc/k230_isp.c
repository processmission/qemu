/*
 * K230 ISP media pipeline registers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/misc/k230_isp.h"
#include "migration/vmstate.h"
#include "trace.h"

#define K230_ISP_MAIN_STATUS      0x05c4
#define K230_ISP_MAIN_CLEAR       0x05c8
#define K230_ISP_MCM_CTRL         0x1300
#define K230_ISP_MCM0_STATUS      0x16d0
#define K230_ISP_MCM0_RAW_STATUS  0x16d4
#define K230_ISP_MCM0_CLEAR       0x16d8
#define K230_ISP_MCM0_RAW_CLEAR   0x16dc
#define K230_ISP_MCM1_STATUS      0x16f0
#define K230_ISP_MCM1_CLEAR       0x16f4
#define K230_ISP_MCM2_STATUS      0x56d8
#define K230_ISP_MCM2_CLEAR       0x56dc
#define K230_ISP_MCM3_STATUS      0x72c8
#define K230_ISP_MCM3_CLEAR       0x72cc
#define K230_ISP_TOP_STATUS       0x3d60
#define K230_ISP_FE_START         0x3d64
#define K230_ISP_FE_MI_STATUS     0x3d74
#define K230_ISP_FE_MI_CLEAR      0x3d78

#define K230_ISP_TOP_PENDING      BIT(0)
#define K230_ISP_TOP_ACK          BIT(1)
#define K230_ISP_FE_DONE_STATUS   BIT(0)
#define K230_ISP_MI_FRAME_STATUS  BIT(8)
#define K230_ISP_FE_START_CMD     BIT(16)
#define K230_ISP_MCM_CH0_ENABLE   BIT(6)
#define K230_ISP_MCM_CH1_ENABLE   BIT(7)
#define K230_ISP_MCM_CH2_ENABLE   BIT(17)
#define K230_ISP_MCM_CH2_STATUS   BIT(14)
#define K230_ISP_MCM_CH0_BUFFER   BIT(0)
#define K230_ISP_MCM_CH1_BUFFER   BIT(3)
#define K230_ISP_MCM_CH0_DONE     BIT(6)
#define K230_ISP_MCM_CH1_DONE     BIT(7)
#define K230_ISP_MCM_STREAM_MASK  (K230_ISP_MCM_CH0_ENABLE | \
                                   K230_ISP_MCM_CH1_ENABLE | \
                                   K230_ISP_MCM_CH2_ENABLE)
#define K230_ISP_MCM_FRAME_NS     (NANOSECONDS_PER_SECOND / 30)

static bool k230_isp_access_hits(hwaddr addr, unsigned int size,
                                 hwaddr offset)
{
    return addr <= offset && offset < addr + size;
}

static uint32_t k230_isp_read32(K230IspState *s, hwaddr addr)
{
    uint32_t val = 0;

    if (addr > K230_ISP_SIZE - sizeof(val)) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        val |= (uint32_t)s->regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_isp_write32(K230IspState *s, hwaddr addr, uint32_t val)
{
    if (addr > K230_ISP_SIZE - sizeof(val)) {
        return;
    }

    for (int i = 0; i < 4; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }
}

static void k230_isp_update_irq(qemu_irq irq, bool *level, bool new_level,
                                const char *name)
{
    if (*level == new_level) {
        return;
    }

    *level = new_level;
    trace_k230_isp_irq(name, new_level);
    qemu_set_irq(irq, new_level);
}

static bool k230_isp_has_mi_status(K230IspState *s)
{
    return k230_isp_read32(s, K230_ISP_MCM0_STATUS) ||
           k230_isp_read32(s, K230_ISP_MCM0_RAW_STATUS) ||
           k230_isp_read32(s, K230_ISP_MCM1_STATUS) ||
           k230_isp_read32(s, K230_ISP_MCM2_STATUS) ||
           k230_isp_read32(s, K230_ISP_MCM3_STATUS);
}

static void k230_isp_refresh_irqs(K230IspState *s)
{
    k230_isp_update_irq(s->isp_irq, &s->isp_irq_level,
                        k230_isp_read32(s, K230_ISP_MAIN_STATUS) != 0,
                        "isp");
    k230_isp_update_irq(s->mi_irq, &s->mi_irq_level,
                        k230_isp_has_mi_status(s), "mi");
    k230_isp_update_irq(s->fe_irq, &s->fe_irq_level,
                        k230_isp_read32(s, K230_ISP_FE_MI_STATUS) &
                        K230_ISP_FE_DONE_STATUS,
                        "fe");
}

static void k230_isp_set_top_pending(K230IspState *s)
{
    uint32_t status = k230_isp_read32(s, K230_ISP_TOP_STATUS);

    k230_isp_write32(s, K230_ISP_TOP_STATUS, status | K230_ISP_TOP_PENDING);
}

static void k230_isp_clear_status(K230IspState *s, hwaddr status_offset,
                                  uint64_t val)
{
    uint32_t status = k230_isp_read32(s, status_offset);

    k230_isp_write32(s, status_offset, status & ~(uint32_t)val);
}

static void k230_isp_clear_mcm0_status(K230IspState *s, uint64_t val)
{
    uint32_t clear = val;

    if (clear & K230_ISP_MCM_CH0_DONE) {
        clear |= K230_ISP_MCM_CH0_BUFFER;
    }
    if (clear & K230_ISP_MCM_CH0_BUFFER) {
        clear |= K230_ISP_MCM_CH0_DONE;
    }
    if (clear & K230_ISP_MCM_CH1_DONE) {
        clear |= K230_ISP_MCM_CH1_BUFFER;
    }
    if (clear & K230_ISP_MCM_CH1_BUFFER) {
        clear |= K230_ISP_MCM_CH1_DONE;
    }

    k230_isp_clear_status(s, K230_ISP_MCM0_STATUS, clear);
}

static void k230_isp_raise_fe(K230IspState *s)
{
    uint32_t status = k230_isp_read32(s, K230_ISP_FE_MI_STATUS);

    k230_isp_write32(s, K230_ISP_FE_MI_STATUS,
                     status | K230_ISP_FE_DONE_STATUS);
    k230_isp_set_top_pending(s);
}

static void k230_isp_raise_mcm(K230IspState *s, uint32_t ctrl)
{
    uint32_t mcm0_status = k230_isp_read32(s, K230_ISP_MCM0_STATUS);
    uint32_t mcm2_status = k230_isp_read32(s, K230_ISP_MCM2_STATUS);

    if (ctrl & K230_ISP_MCM_CH0_ENABLE) {
        mcm0_status |= K230_ISP_MCM_CH0_BUFFER | K230_ISP_MCM_CH0_DONE;
    }
    if (ctrl & K230_ISP_MCM_CH1_ENABLE) {
        mcm0_status |= K230_ISP_MCM_CH1_BUFFER | K230_ISP_MCM_CH1_DONE;
    }
    if (ctrl & K230_ISP_MCM_CH2_ENABLE) {
        mcm2_status |= K230_ISP_MCM_CH2_STATUS;
    }

    k230_isp_write32(s, K230_ISP_MCM0_STATUS, mcm0_status);
    k230_isp_write32(s, K230_ISP_MCM2_STATUS, mcm2_status);
    k230_isp_set_top_pending(s);
}

static bool k230_isp_mcm_stream_active(K230IspState *s)
{
    uint32_t ctrl = k230_isp_read32(s, K230_ISP_MCM_CTRL);

    return ctrl & K230_ISP_MCM_STREAM_MASK;
}

static void k230_isp_schedule_mcm_frame(K230IspState *s)
{
    if (!k230_isp_mcm_stream_active(s)) {
        timer_del(&s->mcm_frame_timer);
        return;
    }

    timer_mod(&s->mcm_frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + K230_ISP_MCM_FRAME_NS);
}

static void k230_isp_raise_mcm_frame(K230IspState *s)
{
    uint32_t ctrl = k230_isp_read32(s, K230_ISP_MCM_CTRL);
    uint32_t status = k230_isp_read32(s, K230_ISP_MCM0_STATUS);
    uint32_t top_status = k230_isp_read32(s, K230_ISP_FE_MI_STATUS);

    if (ctrl & K230_ISP_MCM_CH0_ENABLE) {
        status |= K230_ISP_MCM_CH0_BUFFER | K230_ISP_MCM_CH0_DONE;
    }
    if (ctrl & K230_ISP_MCM_CH1_ENABLE) {
        status |= K230_ISP_MCM_CH1_BUFFER | K230_ISP_MCM_CH1_DONE;
    }
    if (ctrl & K230_ISP_MCM_CH2_ENABLE) {
        uint32_t mcm2_status = k230_isp_read32(s, K230_ISP_MCM2_STATUS);

        k230_isp_write32(s, K230_ISP_MCM2_STATUS,
                         mcm2_status | K230_ISP_MCM_CH2_STATUS);
    }

    k230_isp_write32(s, K230_ISP_MCM0_STATUS, status);
    k230_isp_write32(s, K230_ISP_FE_MI_STATUS,
                     top_status | K230_ISP_MI_FRAME_STATUS);
    k230_isp_set_top_pending(s);
}

static void k230_isp_mcm_frame_timer(void *opaque)
{
    K230IspState *s = K230_ISP(opaque);

    if (k230_isp_mcm_stream_active(s)) {
        k230_isp_raise_mcm_frame(s);
        k230_isp_refresh_irqs(s);
        k230_isp_schedule_mcm_frame(s);
    }
}

static uint64_t k230_isp_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230IspState *s = K230_ISP(opaque);
    uint64_t val = 0;

    if (addr >= K230_ISP_SIZE || size > K230_ISP_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (i * 8);
    }

    trace_k230_isp_read(addr, val, size);

    return val;
}

static void k230_isp_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    K230IspState *s = K230_ISP(opaque);

    if (addr >= K230_ISP_SIZE || size > K230_ISP_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }

    trace_k230_isp_write(addr, val, size);

    if (k230_isp_access_hits(addr, size, K230_ISP_MAIN_CLEAR) && val) {
        k230_isp_clear_status(s, K230_ISP_MAIN_STATUS, val);
    }
    if (k230_isp_access_hits(addr, size, K230_ISP_FE_MI_CLEAR) && val) {
        k230_isp_clear_status(s, K230_ISP_FE_MI_STATUS, val);
    }
    if (k230_isp_access_hits(addr, size, K230_ISP_MCM0_CLEAR) && val) {
        k230_isp_clear_mcm0_status(s, val);
    }
    if (k230_isp_access_hits(addr, size, K230_ISP_MCM0_RAW_CLEAR) && val) {
        k230_isp_clear_status(s, K230_ISP_MCM0_RAW_STATUS, val);
    }
    if (k230_isp_access_hits(addr, size, K230_ISP_MCM1_CLEAR) && val) {
        k230_isp_clear_status(s, K230_ISP_MCM1_STATUS, val);
    }
    if (k230_isp_access_hits(addr, size, K230_ISP_MCM2_CLEAR) && val) {
        k230_isp_clear_status(s, K230_ISP_MCM2_STATUS, val);
    }
    if (k230_isp_access_hits(addr, size, K230_ISP_MCM3_CLEAR) && val) {
        k230_isp_clear_status(s, K230_ISP_MCM3_STATUS, val);
    }
    if (k230_isp_access_hits(addr, size, K230_ISP_TOP_STATUS) &&
        (val & K230_ISP_TOP_ACK)) {
        uint32_t status = k230_isp_read32(s, K230_ISP_TOP_STATUS);

        k230_isp_write32(s, K230_ISP_TOP_STATUS,
                         status & ~K230_ISP_TOP_PENDING);
    }

    if (k230_isp_access_hits(addr, size, K230_ISP_FE_START) &&
        (k230_isp_read32(s, K230_ISP_FE_START) & K230_ISP_FE_START_CMD)) {
        k230_isp_raise_fe(s);
    }

    if (k230_isp_access_hits(addr, size, K230_ISP_MCM_CTRL)) {
        uint32_t ctrl = k230_isp_read32(s, K230_ISP_MCM_CTRL);

        if (ctrl & K230_ISP_MCM_STREAM_MASK) {
            k230_isp_raise_mcm(s, ctrl);
        }
        k230_isp_schedule_mcm_frame(s);
    }

    k230_isp_refresh_irqs(s);
}

static const MemoryRegionOps k230_isp_ops = {
    .read = k230_isp_read,
    .write = k230_isp_write,
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

static void k230_isp_reset(DeviceState *dev)
{
    K230IspState *s = K230_ISP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    timer_del(&s->mcm_frame_timer);
    k230_isp_update_irq(s->isp_irq, &s->isp_irq_level, false, "isp");
    k230_isp_update_irq(s->mi_irq, &s->mi_irq_level, false, "mi");
    k230_isp_update_irq(s->fe_irq, &s->fe_irq_level, false, "fe");
}

static const VMStateDescription vmstate_k230_isp = {
    .name = TYPE_K230_ISP,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(isp_irq_level, K230IspState),
        VMSTATE_BOOL(mi_irq_level, K230IspState),
        VMSTATE_BOOL(fe_irq_level, K230IspState),
        VMSTATE_TIMER(mcm_frame_timer, K230IspState),
        VMSTATE_UINT8_ARRAY(regs, K230IspState, K230_ISP_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_isp_realize(DeviceState *dev, Error **errp)
{
    K230IspState *s = K230_ISP(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_isp_ops, s,
                          TYPE_K230_ISP, K230_ISP_SIZE);
    timer_init_ns(&s->mcm_frame_timer, QEMU_CLOCK_VIRTUAL,
                  k230_isp_mcm_frame_timer, s);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->isp_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->mi_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->fe_irq);
}

static void k230_isp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_isp_realize;
    device_class_set_legacy_reset(dc, k230_isp_reset);
    dc->vmsd = &vmstate_k230_isp;
    dc->desc = "K230 ISP media pipeline registers";
}

static const TypeInfo k230_isp_type_info = {
    .name = TYPE_K230_ISP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230IspState),
    .class_init = k230_isp_class_init,
};

static void k230_isp_register_types(void)
{
    type_register_static(&k230_isp_type_info);
}

type_init(k230_isp_register_types)
