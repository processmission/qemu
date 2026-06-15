/*
 * K230 scratch register block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/k230_regs.h"
#include "migration/vmstate.h"
#include "qemu/bitmap.h"
#include "qemu/timer.h"
#include "system/dma.h"
#include "trace.h"

static bool k230_regs_access_hits(hwaddr addr, unsigned int size,
                                  uint64_t offset)
{
    return offset != K230_REGS_NO_IRQ_OFFSET &&
           addr <= offset && offset < addr + size;
}

static bool k230_regs_access_hits_start(K230RegsState *s, hwaddr addr,
                                        unsigned int size)
{
    return k230_regs_access_hits(addr, size, s->irq_start_offset) ||
           k230_regs_access_hits(addr, size, s->irq_start2_offset) ||
           k230_regs_access_hits(addr, size, s->irq_start3_offset);
}

static const char *k230_regs_name(K230RegsState *s)
{
    return object_get_canonical_path_component(OBJECT(s));
}

static bool k230_regs_has_counter(const K230RegsState *s)
{
    return s->counter_offset != K230_REGS_NO_IRQ_OFFSET &&
           s->counter_size != 0 && s->counter_frequency != 0;
}

static bool k230_regs_counter_contains(const K230RegsState *s, hwaddr addr)
{
    if (!k230_regs_has_counter(s) || addr < s->counter_offset) {
        return false;
    }

    return addr - s->counter_offset < s->counter_size;
}

static uint8_t k230_regs_counter_byte(const K230RegsState *s, hwaddr addr)
{
    uint64_t index = addr - s->counter_offset;
    uint64_t counter;

    counter = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                       s->counter_frequency, NANOSECONDS_PER_SECOND);

    return counter >> ((index % sizeof(counter)) * 8);
}

static void k230_regs_store(K230RegsState *s, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    if (addr >= K230_REGS_STORAGE_SIZE ||
        size > K230_REGS_STORAGE_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }
}

static uint64_t k230_regs_load(const K230RegsState *s, hwaddr addr,
                               unsigned int size)
{
    uint64_t val = 0;

    if (addr >= K230_REGS_STORAGE_SIZE ||
        size > K230_REGS_STORAGE_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (i * 8);
    }

    return val;
}

static bool k230_regs_has_irq_status(const K230RegsState *s)
{
    return s->irq_status_offset != K230_REGS_NO_IRQ_OFFSET &&
           s->irq_status_size != 0;
}

static void k230_regs_set_irq_status(K230RegsState *s, bool pending)
{
    unsigned int size;

    if (!k230_regs_has_irq_status(s) ||
        s->irq_status_offset >= K230_REGS_STORAGE_SIZE) {
        return;
    }

    size = MIN(s->irq_status_size, (uint64_t)sizeof(s->irq_status_value));
    if (size > K230_REGS_STORAGE_SIZE - s->irq_status_offset) {
        return;
    }

    k230_regs_store(s, s->irq_status_offset,
                    pending ? s->irq_status_value : 0, size);
}

static void k230_regs_log_irq_command(K230RegsState *s)
{
    uint64_t start;
    uint64_t end;
    uint64_t hi;

    if (s->irq_command_start_offset == K230_REGS_NO_IRQ_OFFSET) {
        return;
    }

    start = k230_regs_load(s, s->irq_command_start_offset, sizeof(uint32_t));
    end = k230_regs_load(s, s->irq_command_end_offset, sizeof(uint32_t));
    hi = k230_regs_load(s, s->irq_command_hi_offset, sizeof(uint32_t));
    trace_k230_regs_irq_command(k230_regs_name(s), start, end, hi);
}

static void k230_regs_completion_zero(K230RegsState *s)
{
    uint64_t command_start;
    uint64_t command_end;
    uint64_t zero_end;
    uint64_t pages;
    long page_count;
    g_autofree unsigned long *seen_pages = NULL;

    if (s->complete_zero_base == K230_REGS_NO_IRQ_OFFSET ||
        s->complete_zero_size == 0 || s->complete_zero_page_size == 0 ||
        (s->complete_zero_page_size & (s->complete_zero_page_size - 1))) {
        return;
    }

    if (s->irq_command_start_offset == K230_REGS_NO_IRQ_OFFSET) {
        dma_memory_set(&address_space_memory, s->complete_zero_base, 0,
                       s->complete_zero_size, MEMTXATTRS_UNSPECIFIED);
        return;
    }

    command_start = k230_regs_load(s, s->irq_command_start_offset,
                                   sizeof(uint32_t));
    command_end = k230_regs_load(s, s->irq_command_end_offset,
                                 sizeof(uint32_t));
    if (!command_start || !command_end || command_end <= command_start) {
        return;
    }

    if (!s->complete_zero_command_pages) {
        dma_memory_set(&address_space_memory, s->complete_zero_base, 0,
                       s->complete_zero_size, MEMTXATTRS_UNSPECIFIED);
        return;
    }

    zero_end = s->complete_zero_base + s->complete_zero_size;
    if (zero_end < s->complete_zero_base) {
        return;
    }
    pages = s->complete_zero_size / s->complete_zero_page_size;
    if (!pages || pages > LONG_MAX) {
        return;
    }
    page_count = pages;
    seen_pages = bitmap_new(page_count);
    for (uint64_t addr = command_start; addr + sizeof(uint32_t) <= command_end;
         addr += sizeof(uint32_t)) {
        uint32_t raw;
        uint32_t value;
        uint64_t page;
        uint64_t page_index;

        if (dma_memory_read(&address_space_memory, addr, &raw, sizeof(raw),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }

        value = ldl_le_p(&raw);
        page = value & ~(s->complete_zero_page_size - 1);
        if (page < s->complete_zero_base ||
            page + s->complete_zero_page_size > zero_end) {
            continue;
        }

        page_index = (page - s->complete_zero_base) /
                     s->complete_zero_page_size;
        if (page_index >= pages ||
            test_and_set_bit((long)page_index, seen_pages)) {
            continue;
        }

        trace_k230_regs_completion_zero_page(k230_regs_name(s), page, value);
        dma_memory_set(&address_space_memory, page, 0,
                       s->complete_zero_page_size,
                       MEMTXATTRS_UNSPECIFIED);
    }
}

static void k230_regs_raise_irq(K230RegsState *s)
{
    s->irq_pending = false;
    k230_regs_completion_zero(s);
    k230_regs_set_irq_status(s, true);
    s->irq_level = true;
    trace_k230_regs_irq(k230_regs_name(s), true);
    qemu_set_irq(s->irq, 1);
}

static void k230_regs_irq_timer(void *opaque)
{
    K230RegsState *s = K230_REGS(opaque);

    if (!s->irq_pending) {
        return;
    }

    k230_regs_raise_irq(s);
}

static uint64_t k230_regs_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230RegsState *s = K230_REGS(opaque);
    uint64_t val = 0;

    if (addr >= K230_REGS_STORAGE_SIZE ||
        size > K230_REGS_STORAGE_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        if (k230_regs_counter_contains(s, addr + i)) {
            val |= (uint64_t)k230_regs_counter_byte(s, addr + i) << (i * 8);
        } else {
            val |= (uint64_t)s->regs[addr + i] << (i * 8);
        }
    }

    trace_k230_regs_read(k230_regs_name(s), addr, val, size);

    return val;
}

static void k230_regs_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    K230RegsState *s = K230_REGS(opaque);

    if (addr >= K230_REGS_STORAGE_SIZE ||
        size > K230_REGS_STORAGE_SIZE - addr) {
        return;
    }

    k230_regs_store(s, addr, val, size);

    trace_k230_regs_write(k230_regs_name(s), addr, val, size);

    if (k230_regs_access_hits(addr, size, s->irq_clear_offset) && val) {
        if (s->irq_clear_clears_status) {
            k230_regs_set_irq_status(s, false);
        }
        s->irq_level = false;
        trace_k230_regs_irq(k230_regs_name(s), false);
        qemu_set_irq(s->irq, 0);
        return;
    }

    if (s->irq_on_any_write ||
        (k230_regs_access_hits_start(s, addr, size) &&
         (val & s->irq_start_mask))) {
        k230_regs_set_irq_status(s, false);
        k230_regs_log_irq_command(s);
        if (s->irq_delay_ns) {
            s->irq_pending = true;
            timer_mod(&s->irq_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      s->irq_delay_ns);
        } else {
            k230_regs_raise_irq(s);
        }
    }
}

static const MemoryRegionOps k230_regs_ops = {
    .read = k230_regs_read,
    .write = k230_regs_write,
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

static void k230_regs_reset(DeviceState *dev)
{
    K230RegsState *s = K230_REGS(dev);

    memset(s->regs, 0, sizeof(s->regs));
    timer_del(&s->irq_timer);
    s->irq_pending = false;
    s->irq_level = false;
    qemu_set_irq(s->irq, 0);
}

static const VMStateDescription vmstate_k230_regs = {
    .name = TYPE_K230_REGS,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(irq_level, K230RegsState),
        VMSTATE_BOOL(irq_pending, K230RegsState),
        VMSTATE_TIMER(irq_timer, K230RegsState),
        VMSTATE_UINT8_ARRAY(regs, K230RegsState, K230_REGS_STORAGE_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_regs_realize(DeviceState *dev, Error **errp)
{
    K230RegsState *s = K230_REGS(dev);

    if (!s->size) {
        s->size = K230_REGS_DEFAULT_SIZE;
    }

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_regs_ops, s,
                          TYPE_K230_REGS, s->size);
    timer_init_ns(&s->irq_timer, QEMU_CLOCK_VIRTUAL,
                  k230_regs_irq_timer, s);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property k230_regs_properties[] = {
    DEFINE_PROP_UINT64("size", K230RegsState, size,
                       K230_REGS_DEFAULT_SIZE),
    DEFINE_PROP_UINT64("irq-start-offset", K230RegsState, irq_start_offset,
                       K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("irq-start2-offset", K230RegsState, irq_start2_offset,
                       K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("irq-start3-offset", K230RegsState, irq_start3_offset,
                       K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("irq-start-mask", K230RegsState, irq_start_mask,
                       UINT64_MAX),
    DEFINE_PROP_UINT64("irq-clear-offset", K230RegsState, irq_clear_offset,
                       K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("irq-status-offset", K230RegsState, irq_status_offset,
                       K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("irq-status-size", K230RegsState, irq_status_size, 0),
    DEFINE_PROP_UINT64("irq-status-value", K230RegsState, irq_status_value, 0),
    DEFINE_PROP_UINT64("irq-delay-ns", K230RegsState, irq_delay_ns, 0),
    DEFINE_PROP_UINT64("irq-command-start-offset", K230RegsState,
                       irq_command_start_offset, K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("irq-command-end-offset", K230RegsState,
                       irq_command_end_offset, K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("irq-command-hi-offset", K230RegsState,
                       irq_command_hi_offset, K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("complete-zero-base", K230RegsState,
                       complete_zero_base, K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("complete-zero-size", K230RegsState,
                       complete_zero_size, 0),
    DEFINE_PROP_UINT64("complete-zero-page-size", K230RegsState,
                       complete_zero_page_size, 4096),
    DEFINE_PROP_UINT64("counter-offset", K230RegsState, counter_offset,
                       K230_REGS_NO_IRQ_OFFSET),
    DEFINE_PROP_UINT64("counter-size", K230RegsState, counter_size, 0),
    DEFINE_PROP_UINT64("counter-frequency", K230RegsState,
                       counter_frequency, 0),
    DEFINE_PROP_BOOL("irq-clear-clears-status", K230RegsState,
                     irq_clear_clears_status, true),
    DEFINE_PROP_BOOL("irq-on-any-write", K230RegsState, irq_on_any_write,
                     false),
    DEFINE_PROP_BOOL("complete-zero-command-pages", K230RegsState,
                     complete_zero_command_pages, false),
};

static void k230_regs_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_regs_realize;
    device_class_set_legacy_reset(dc, k230_regs_reset);
    device_class_set_props(dc, k230_regs_properties);
    dc->vmsd = &vmstate_k230_regs;
    dc->desc = "K230 scratch register block";
}

static const TypeInfo k230_regs_type_info = {
    .name = TYPE_K230_REGS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230RegsState),
    .class_init = k230_regs_class_init,
};

static void k230_regs_register_types(void)
{
    type_register_static(&k230_regs_type_info);
}

type_init(k230_regs_register_types)
