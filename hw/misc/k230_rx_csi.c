/*
 * K230 RX CSI and video input registers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/misc/k230_rx_csi.h"
#include "migration/vmstate.h"
#include "trace.h"

#define K230_RX_CSI_HOST_STRIDE       0x800
#define K230_RX_CSI_HOST_COUNT        3
#define K230_RX_CSI_HOST_ENABLE       0x008
#define K230_RX_CSI_HOST_PHY_STATE    0x014
#define K230_RX_CSI_PHY_STOPSTATE     BIT(16)
#define K230_RX_CSI_PHY_CTRL0         0x850
#define K230_RX_CSI_PHY_DATA0         0x854
#define K230_RX_CSI_PHY_CTRL1         0x858
#define K230_RX_CSI_PHY_DATA1         0x85c

static bool k230_rx_csi_access_hits(hwaddr addr, unsigned int size,
                                    hwaddr offset)
{
    return addr <= offset && offset < addr + size;
}

static uint32_t k230_rx_csi_read32(K230RxCsiState *s, hwaddr addr)
{
    uint32_t val = 0;

    if (addr > K230_RX_CSI_SIZE - sizeof(val)) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        val |= (uint32_t)s->regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_rx_csi_write32(K230RxCsiState *s, hwaddr addr, uint32_t val)
{
    if (addr > K230_RX_CSI_SIZE - sizeof(val)) {
        return;
    }

    for (int i = 0; i < 4; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }
}

static void k230_rx_csi_set_host_ready(K230RxCsiState *s, unsigned int host)
{
    hwaddr state = host * K230_RX_CSI_HOST_STRIDE +
                   K230_RX_CSI_HOST_PHY_STATE;
    uint32_t val = k230_rx_csi_read32(s, state);

    k230_rx_csi_write32(s, state, val | K230_RX_CSI_PHY_STOPSTATE);
    trace_k230_rx_csi_ready(host, val | K230_RX_CSI_PHY_STOPSTATE);
}

static void k230_rx_csi_reset_ready(K230RxCsiState *s)
{
    for (unsigned int i = 0; i < K230_RX_CSI_HOST_COUNT; i++) {
        k230_rx_csi_set_host_ready(s, i);
    }
}

static void k230_rx_csi_mirror_phy_port(K230RxCsiState *s, hwaddr ctrl,
                                        hwaddr data)
{
    uint32_t val = k230_rx_csi_read32(s, ctrl);

    if (!k230_rx_csi_read32(s, data)) {
        k230_rx_csi_write32(s, data, val);
    }
}

static uint64_t k230_rx_csi_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230RxCsiState *s = K230_RX_CSI(opaque);
    uint64_t val = 0;

    if (addr >= K230_RX_CSI_SIZE || size > K230_RX_CSI_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (i * 8);
    }

    trace_k230_rx_csi_read(addr, val, size);

    return val;
}

static void k230_rx_csi_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned int size)
{
    K230RxCsiState *s = K230_RX_CSI(opaque);

    if (addr >= K230_RX_CSI_SIZE || size > K230_RX_CSI_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }

    trace_k230_rx_csi_write(addr, val, size);

    for (unsigned int i = 0; i < K230_RX_CSI_HOST_COUNT; i++) {
        hwaddr enable = i * K230_RX_CSI_HOST_STRIDE +
                        K230_RX_CSI_HOST_ENABLE;

        if (k230_rx_csi_access_hits(addr, size, enable) && val) {
            k230_rx_csi_set_host_ready(s, i);
        }
    }

    if (k230_rx_csi_access_hits(addr, size, K230_RX_CSI_PHY_CTRL0)) {
        k230_rx_csi_mirror_phy_port(s, K230_RX_CSI_PHY_CTRL0,
                                    K230_RX_CSI_PHY_DATA0);
    }
    if (k230_rx_csi_access_hits(addr, size, K230_RX_CSI_PHY_CTRL1)) {
        k230_rx_csi_mirror_phy_port(s, K230_RX_CSI_PHY_CTRL1,
                                    K230_RX_CSI_PHY_DATA1);
    }
}

static const MemoryRegionOps k230_rx_csi_ops = {
    .read = k230_rx_csi_read,
    .write = k230_rx_csi_write,
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

static void k230_rx_csi_reset(DeviceState *dev)
{
    K230RxCsiState *s = K230_RX_CSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    k230_rx_csi_reset_ready(s);
}

static const VMStateDescription vmstate_k230_rx_csi = {
    .name = TYPE_K230_RX_CSI,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230RxCsiState, K230_RX_CSI_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_rx_csi_realize(DeviceState *dev, Error **errp)
{
    K230RxCsiState *s = K230_RX_CSI(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_rx_csi_ops, s,
                          TYPE_K230_RX_CSI, K230_RX_CSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_rx_csi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_rx_csi_realize;
    device_class_set_legacy_reset(dc, k230_rx_csi_reset);
    dc->vmsd = &vmstate_k230_rx_csi;
    dc->desc = "K230 RX CSI and video input registers";
}

static const TypeInfo k230_rx_csi_type_info = {
    .name = TYPE_K230_RX_CSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230RxCsiState),
    .class_init = k230_rx_csi_class_init,
};

static void k230_rx_csi_register_types(void)
{
    type_register_static(&k230_rx_csi_type_info);
}

type_init(k230_rx_csi_register_types)
