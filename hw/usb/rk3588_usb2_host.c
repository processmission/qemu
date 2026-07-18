/*
 * Rockchip RK3588 USB2 host register shim
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/usb/rk3588_usb2_host.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define RK3588_USB2_HOST_WINDOW_WORDS \
    (RK3588_USB2_HOST_MMIO_SIZE / sizeof(uint32_t))

#define RK3588_USB2_EHCI_CAPBASE_RESET 0x01000020U
#define RK3588_USB2_EHCI_HCSPARAMS_RESET 0x00000011U
#define RK3588_USB2_EHCI_CMD_RESET BIT(1)
#define RK3588_USB2_EHCI_CMD_RUN BIT(0)
#define RK3588_USB2_EHCI_STS_HALT BIT(12)
#define RK3588_USB2_EHCI_PORT_POWER BIT(12)

#define RK3588_USB2_OHCI_REVISION_RESET 0x00000010U
#define RK3588_USB2_OHCI_HCR BIT(0)
#define RK3588_USB2_OHCI_RH_A_NPS BIT(9)
#define RK3588_USB2_OHCI_RH_A_NOCP BIT(12)
#define RK3588_USB2_OHCI_RH_A_NDP1 1U
#define RK3588_USB2_OHCI_RH_PS_PPS BIT(8)

REG32(EHCI_CAPBASE, 0x00)
REG32(EHCI_HCSPARAMS, 0x04)
REG32(EHCI_USBCMD, 0x20)
REG32(EHCI_USBSTS, 0x24)
REG32(EHCI_PORTSC0, 0x64)

REG32(OHCI_REVISION, 0x00)
REG32(OHCI_CMDSTATUS, 0x08)
REG32(OHCI_INTRSTATUS, 0x0c)
REG32(OHCI_ROOTHUB_A, 0x48)
REG32(OHCI_PORTSTATUS0, 0x54)

#define RK3588_USB2_HOST_REG_WORDS (R_EHCI_PORTSC0 + 1)

struct RK3588USB2HostState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[RK3588_USB2_HOST_MMIO_COUNT];
    RegisterInfo regs_info[RK3588_USB2_HOST_MMIO_COUNT]
                          [RK3588_USB2_HOST_REG_WORDS];
    uint32_t regs[RK3588_USB2_HOST_MMIO_COUNT]
                 [RK3588_USB2_HOST_WINDOW_WORDS];
    bool active;
};

static unsigned int rk3588_usb2_host_window_index(
    RK3588USB2HostState *s, RegisterInfoArray *reg_array)
{
    unsigned int i;

    for (i = 0; i < RK3588_USB2_HOST_MMIO_COUNT; i++) {
        if (s->reg_array[i] == reg_array) {
            return i;
        }
    }

    g_assert_not_reached();
}

static bool rk3588_usb2_host_is_register(RegisterInfoArray *reg_array,
                                         hwaddr addr)
{
    unsigned int i;

    for (i = 0; i < reg_array->num_elements; i++) {
        if (reg_array->r[i]->access->addr == addr) {
            return true;
        }
    }

    return false;
}

static uint64_t rk3588_usb2_host_raw_read(RK3588USB2HostState *s,
                                          unsigned int window,
                                          hwaddr addr, unsigned int size)
{
    uint64_t value = 0;
    unsigned int i;

    for (i = 0; i < size; i++) {
        hwaddr byte_addr = addr + i;
        uint32_t word = s->regs[window][byte_addr / sizeof(uint32_t)];
        unsigned int shift = (byte_addr & 3) * 8;

        value |= (uint64_t)extract32(word, shift, 8) << (i * 8);
    }

    return value;
}

static void rk3588_usb2_host_raw_write(RK3588USB2HostState *s,
                                       unsigned int window, hwaddr addr,
                                       uint64_t value, unsigned int size)
{
    unsigned int i;

    for (i = 0; i < size; i++) {
        hwaddr byte_addr = addr + i;
        uint32_t *word = &s->regs[window][byte_addr / sizeof(uint32_t)];
        unsigned int shift = (byte_addr & 3) * 8;

        *word = deposit32(*word, shift, 8, value >> (i * 8));
    }
}

static uint64_t rk3588_usb2_host_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3588USB2HostState *s = RK3588_USB2_HOST(
        register_array_get_owner(reg_array));
    unsigned int window = rk3588_usb2_host_window_index(s, reg_array);

    if (addr > RK3588_USB2_HOST_MMIO_SIZE - size) {
        return 0;
    }

    if (!s->active) {
        return size == sizeof(uint64_t) ? UINT64_MAX :
               MAKE_64BIT_MASK(0, size * 8);
    }

    if (size == sizeof(uint32_t) &&
        rk3588_usb2_host_is_register(reg_array, addr)) {
        return register_read_memory(reg_array, addr, size);
    }

    return rk3588_usb2_host_raw_read(s, window, addr, size);
}

static void rk3588_usb2_host_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3588USB2HostState *s = RK3588_USB2_HOST(
        register_array_get_owner(reg_array));
    unsigned int window = rk3588_usb2_host_window_index(s, reg_array);

    if (!s->active || addr > RK3588_USB2_HOST_MMIO_SIZE - size) {
        return;
    }

    if (size == sizeof(uint32_t) &&
        rk3588_usb2_host_is_register(reg_array, addr)) {
        register_write_memory(reg_array, addr, value, size);
        return;
    }

    rk3588_usb2_host_raw_write(s, window, addr, value, size);
}

static uint64_t rk3588_usb2_ehci_cmd_pre_write(RegisterInfo *reg,
                                                uint64_t value)
{
    return value & ~RK3588_USB2_EHCI_CMD_RESET;
}

static void rk3588_usb2_ehci_cmd_post_write(RegisterInfo *reg,
                                             uint64_t value)
{
    RK3588USB2HostState *s = RK3588_USB2_HOST(reg->opaque);
    unsigned int window;

    for (window = RK3588_USB2_HOST_EHCI0;
         window < RK3588_USB2_HOST_MMIO_COUNT; window += 2) {
        if (reg->data == &s->regs[window][R_EHCI_USBCMD]) {
            if (value & RK3588_USB2_EHCI_CMD_RUN) {
                s->regs[window][R_EHCI_USBSTS] &=
                    ~RK3588_USB2_EHCI_STS_HALT;
            } else {
                s->regs[window][R_EHCI_USBSTS] |=
                    RK3588_USB2_EHCI_STS_HALT;
            }
            return;
        }
    }

    g_assert_not_reached();
}

static uint64_t rk3588_usb2_ohci_cmd_pre_write(RegisterInfo *reg,
                                                uint64_t value)
{
    return value & ~RK3588_USB2_OHCI_HCR;
}

static const RegisterAccessInfo rk3588_usb2_ehci_regs_info[] = {
    { .name = "CAPBASE", .addr = A_EHCI_CAPBASE,
      .reset = RK3588_USB2_EHCI_CAPBASE_RESET },
    { .name = "HCSPARAMS", .addr = A_EHCI_HCSPARAMS,
      .reset = RK3588_USB2_EHCI_HCSPARAMS_RESET },
    { .name = "USBCMD", .addr = A_EHCI_USBCMD,
      .pre_write = rk3588_usb2_ehci_cmd_pre_write,
      .post_write = rk3588_usb2_ehci_cmd_post_write },
    { .name = "USBSTS", .addr = A_EHCI_USBSTS,
      .reset = RK3588_USB2_EHCI_STS_HALT },
    { .name = "PORTSC0", .addr = A_EHCI_PORTSC0,
      .reset = RK3588_USB2_EHCI_PORT_POWER },
};

static const RegisterAccessInfo rk3588_usb2_ohci_regs_info[] = {
    { .name = "REVISION", .addr = A_OHCI_REVISION,
      .reset = RK3588_USB2_OHCI_REVISION_RESET },
    { .name = "CMDSTATUS", .addr = A_OHCI_CMDSTATUS,
      .pre_write = rk3588_usb2_ohci_cmd_pre_write },
    { .name = "INTRSTATUS", .addr = A_OHCI_INTRSTATUS,
      .w1c = UINT32_MAX },
    { .name = "ROOTHUB_A", .addr = A_OHCI_ROOTHUB_A,
      .reset = RK3588_USB2_OHCI_RH_A_NDP1 |
               RK3588_USB2_OHCI_RH_A_NPS |
               RK3588_USB2_OHCI_RH_A_NOCP },
    { .name = "PORTSTATUS0", .addr = A_OHCI_PORTSTATUS0,
      .reset = RK3588_USB2_OHCI_RH_PS_PPS },
};

static const MemoryRegionOps rk3588_usb2_host_ops = {
    .read = rk3588_usb2_host_read,
    .write = rk3588_usb2_host_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void rk3588_usb2_host_reset_state(RK3588USB2HostState *s)
{
    unsigned int window;

    memset(s->regs, s->active ? 0 : 0xff, sizeof(s->regs));
    if (!s->active) {
        return;
    }

    for (window = 0; window < RK3588_USB2_HOST_MMIO_COUNT; window++) {
        RegisterInfoArray *reg_array = s->reg_array[window];
        unsigned int i;

        for (i = 0; i < reg_array->num_elements; i++) {
            register_reset(reg_array->r[i]);
        }
    }
}

void rk3588_usb2_host_set_active(RK3588USB2HostState *s, bool active)
{
    s->active = active;
    rk3588_usb2_host_reset_state(s);
}

static void rk3588_usb2_host_reset_hold(Object *obj, ResetType type)
{
    RK3588USB2HostState *s = RK3588_USB2_HOST(obj);

    rk3588_usb2_host_reset_state(s);
}

static const VMStateDescription vmstate_rk3588_usb2_host = {
    .name = TYPE_RK3588_USB2_HOST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(active, RK3588USB2HostState),
        VMSTATE_UINT32_2DARRAY(regs, RK3588USB2HostState,
                              RK3588_USB2_HOST_MMIO_COUNT,
                              RK3588_USB2_HOST_WINDOW_WORDS),
        VMSTATE_END_OF_LIST(),
    },
};

static void rk3588_usb2_host_init(Object *obj)
{
    RK3588USB2HostState *s = RK3588_USB2_HOST(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned int window;

    for (window = 0; window < RK3588_USB2_HOST_MMIO_COUNT; window++) {
        const RegisterAccessInfo *access_info;
        size_t access_info_count;

        if (window == RK3588_USB2_HOST_EHCI0 ||
            window == RK3588_USB2_HOST_EHCI1) {
            access_info = rk3588_usb2_ehci_regs_info;
            access_info_count = ARRAY_SIZE(rk3588_usb2_ehci_regs_info);
        } else {
            access_info = rk3588_usb2_ohci_regs_info;
            access_info_count = ARRAY_SIZE(rk3588_usb2_ohci_regs_info);
        }

        s->reg_array[window] = register_init_block32(
            DEVICE(obj), access_info, access_info_count,
            s->regs_info[window], s->regs[window], &rk3588_usb2_host_ops,
            false, RK3588_USB2_HOST_MMIO_SIZE);
        sysbus_init_mmio(sbd, &s->reg_array[window]->mem);
    }

    rk3588_usb2_host_reset_state(s);
}

static void rk3588_usb2_host_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_rk3588_usb2_host;
    rc->phases.hold = rk3588_usb2_host_reset_hold;
}

static const TypeInfo rk3588_usb2_host_info = {
    .name = TYPE_RK3588_USB2_HOST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588USB2HostState),
    .instance_init = rk3588_usb2_host_init,
    .class_init = rk3588_usb2_host_class_init,
};

static void rk3588_usb2_host_register_types(void)
{
    type_register_static(&rk3588_usb2_host_info);
}

type_init(rk3588_usb2_host_register_types)
