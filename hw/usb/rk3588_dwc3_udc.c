/*
 * Rockchip RK3588 DWC3 device-mode CDC bridge
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * This is the device-mode counterpart to the RK3588 host controllers.  It
 * implements the DWC3 registers, event ring, endpoint commands, and DMA TRBs
 * exercised by Zephyr's DWC3 UDC driver.  An internal high-speed USB host
 * performs the minimum CDC ACM enumeration needed by firmware without
 * requiring a second QEMU USB topology.
 *
 * CDC bulk IN is exposed as a QEMU character backend.  Character-backend
 * input is encoded in CDC SET_LINE_CODING requests by the minimal host
 * bridge.
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/rk3588_dwc3_udc.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/dma.h"

#define DWC3_REG_BANK_SIZE              0x10000
#define DWC3_MAX_ENDPOINTS              32
#define DWC3_INPUT_FIFO_SIZE            256

#define DWC3_GSBUSCFG0                  0xc100
#define DWC3_GCTL                       0xc110
#define DWC3_GCOREID                    0xc120
#define DWC3_GUSB2PHYCFG                0xc200
#define DWC3_GUSB3PIPECTL               0xc2c0
#define DWC3_GEVNTADR_LO                0xc400
#define DWC3_GEVNTADR_HI                0xc404
#define DWC3_GEVNTSIZ                   0xc408
#define DWC3_GEVNTCOUNT                 0xc40c
#define DWC3_DCFG                       0xc700
#define DWC3_DCTL                       0xc704
#define DWC3_DEVTEN                     0xc708
#define DWC3_DSTS                       0xc70c
#define DWC3_DALEPENA                   0xc720
#define DWC3_DEPCMDPAR2(ep)             (0xc800 + 16 * (ep))
#define DWC3_DEPCMDPAR1(ep)             (0xc804 + 16 * (ep))
#define DWC3_DEPCMDPAR0(ep)             (0xc808 + 16 * (ep))
#define DWC3_DEPCMD(ep)                 (0xc80c + 16 * (ep))

#define DWC3_GEVNTSIZ_INTMASK           BIT(31)
#define DWC3_DCTL_CSFTRST               BIT(30)
#define DWC3_DCTL_RUNSTOP               BIT(31)
#define DWC3_DEPCMD_CMDACT              BIT(10)
#define DWC3_DEPCMD_RSCIDX              BIT(16)
#define DWC3_DEPCMD_DEPCFG              1
#define DWC3_DEPCMD_DEPSTRTXFER          6
#define DWC3_DEPCMD_DEPENDXFER          8
#define DWC3_DEPCFG_EPTYPE_SHIFT        1
#define DWC3_DEPCFG_EPTYPE_MASK         3
#define DWC3_EP_TYPE_BULK               2
#define DWC3_EP_TYPE_INTERRUPT          3

#define DWC3_TRB_STATUS_BUFSIZ_MASK     0x00ffffff
#define DWC3_TRB_CTRL_HWO               BIT(0)
#define DWC3_TRB_CTRL_TRBCTL_MASK       (0x3f << 4)
#define DWC3_TRBCTL_NORMAL              (1 << 4)
#define DWC3_TRBCTL_CONTROL_SETUP       (2 << 4)
#define DWC3_TRBCTL_CONTROL_STATUS_2    (3 << 4)
#define DWC3_TRBCTL_CONTROL_STATUS_3    (4 << 4)
#define DWC3_TRBCTL_CONTROL_DATA        (5 << 4)
#define DWC3_TRBCTL_NORMAL_ZLP          (9 << 4)

#define DWC3_DEPEVT_XFERCOMPLETE(ep)    (((ep) << 1) | (1 << 6))
#define DWC3_DEVT_USBRST                0x101
#define DWC3_DEVT_CONNECTDONE           0x201

#define USB_REQTYPE_CLASS_INTERFACE_OUT 0x21
#define USB_REQ_SET_ADDRESS             0x05
#define USB_REQ_SET_CONFIGURATION       0x09
#define USB_CDC_SET_LINE_CODING         0x20
#define USB_CDC_SET_CONTROL_LINE_STATE  0x22
#define USB_CDC_CONTROL_DTR             BIT(0)
#define CDC_INPUT_BAUD_BASE             1000000U
#define DWC3_ENUM_SETUP_DELAY_NS        1000000
#define DWC3_INPUT_SETUP_DELAY_NS       10000000

typedef struct DWC3TRB {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t status;
    uint32_t ctrl;
} DWC3TRB;

typedef struct USBSetupPacket {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} QEMU_PACKED USBSetupPacket;

typedef enum DWC3ControlStage {
    DWC3_CONTROL_IDLE,
    DWC3_CONTROL_DATA_OUT,
    DWC3_CONTROL_STATUS_IN,
} DWC3ControlStage;

struct RK3588DWC3UDCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharFrontend chr;
    QEMUTimer *setup_timer;

    uint32_t regs[DWC3_REG_BANK_SIZE / sizeof(uint32_t)];
    uint8_t ep_type[DWC3_MAX_ENDPOINTS];
    uint64_t ep_trb[DWC3_MAX_ENDPOINTS];
    bool ep_pending[DWC3_MAX_ENDPOINTS];

    uint64_t event_addr;
    uint32_t event_size;
    uint32_t event_count;
    uint32_t event_prod;

    uint8_t input[DWC3_INPUT_FIFO_SIZE];
    uint16_t input_head;
    uint16_t input_tail;
    uint16_t input_count;
    uint8_t input_sequence;
    uint8_t current_input;

    uint8_t enumeration_step;
    uint32_t control_stage;
    bool control_input_request;
};

static void rk3588_dwc3_reset_state(RK3588DWC3UDCState *s);

static uint32_t rk3588_dwc3_reg(RK3588DWC3UDCState *s, hwaddr addr)
{
    return s->regs[addr / sizeof(uint32_t)];
}

static void rk3588_dwc3_set_reg(RK3588DWC3UDCState *s, hwaddr addr,
                                uint32_t value)
{
    s->regs[addr / sizeof(uint32_t)] = value;
}

static void rk3588_dwc3_update_irq(RK3588DWC3UDCState *s)
{
    bool masked = rk3588_dwc3_reg(s, DWC3_GEVNTSIZ) &
                  DWC3_GEVNTSIZ_INTMASK;

    qemu_set_irq(s->irq, s->event_count != 0 && !masked);
}

static void rk3588_dwc3_push_event(RK3588DWC3UDCState *s, uint32_t event)
{
    uint32_t value = cpu_to_le32(event);

    if (!s->event_addr || s->event_size < sizeof(value)) {
        return;
    }

    if (s->event_count + sizeof(value) > s->event_size) {
        return;
    }

    dma_memory_write_relaxed(&address_space_memory,
                             s->event_addr + s->event_prod,
                             &value, sizeof(value));
    s->event_prod += sizeof(value);
    if (s->event_prod >= s->event_size) {
        s->event_prod = 0;
    }
    s->event_count += sizeof(value);
    rk3588_dwc3_update_irq(s);
}

static bool rk3588_dwc3_read_trb(uint64_t addr, DWC3TRB *trb)
{
    DWC3TRB raw;

    if (dma_memory_read_relaxed(&address_space_memory, addr,
                                &raw, sizeof(raw)) != MEMTX_OK) {
        return false;
    }

    trb->addr_lo = le32_to_cpu(raw.addr_lo);
    trb->addr_hi = le32_to_cpu(raw.addr_hi);
    trb->status = le32_to_cpu(raw.status);
    trb->ctrl = le32_to_cpu(raw.ctrl);
    return true;
}

static bool rk3588_dwc3_write_trb(uint64_t addr, const DWC3TRB *trb)
{
    DWC3TRB raw = {
        .addr_lo = cpu_to_le32(trb->addr_lo),
        .addr_hi = cpu_to_le32(trb->addr_hi),
        .status = cpu_to_le32(trb->status),
        .ctrl = cpu_to_le32(trb->ctrl),
    };

    return dma_memory_write_relaxed(&address_space_memory, addr,
                                    &raw, sizeof(raw)) == MEMTX_OK;
}

static uint64_t rk3588_dwc3_trb_buffer(const DWC3TRB *trb)
{
    return ((uint64_t)trb->addr_hi << 32) | trb->addr_lo;
}

static void rk3588_dwc3_complete_trb(RK3588DWC3UDCState *s,
                                     unsigned int ep, DWC3TRB *trb,
                                     uint32_t remaining)
{
    trb->status = remaining & DWC3_TRB_STATUS_BUFSIZ_MASK;
    trb->ctrl &= ~DWC3_TRB_CTRL_HWO;
    if (!rk3588_dwc3_write_trb(s->ep_trb[ep], trb)) {
        return;
    }

    s->ep_pending[ep] = false;
    rk3588_dwc3_push_event(s, DWC3_DEPEVT_XFERCOMPLETE(ep));
}

static void rk3588_dwc3_make_setup(RK3588DWC3UDCState *s,
                                    USBSetupPacket *setup)
{
    memset(setup, 0, sizeof(*setup));

    switch (s->enumeration_step) {
    case 0:
        setup->request = USB_REQ_SET_ADDRESS;
        setup->value = cpu_to_le16(1);
        s->control_input_request = false;
        s->control_stage = DWC3_CONTROL_STATUS_IN;
        break;
    case 1:
        setup->request = USB_REQ_SET_CONFIGURATION;
        setup->value = cpu_to_le16(1);
        s->control_input_request = false;
        s->control_stage = DWC3_CONTROL_STATUS_IN;
        break;
    case 2:
        setup->request_type = USB_REQTYPE_CLASS_INTERFACE_OUT;
        setup->request = USB_CDC_SET_CONTROL_LINE_STATE;
        setup->value = cpu_to_le16(USB_CDC_CONTROL_DTR);
        s->control_input_request = false;
        s->control_stage = DWC3_CONTROL_STATUS_IN;
        break;
    default:
        setup->request_type = USB_REQTYPE_CLASS_INTERFACE_OUT;
        setup->request = USB_CDC_SET_LINE_CODING;
        setup->length = cpu_to_le16(7);
        s->current_input = s->input[s->input_head];
        s->input_sequence++;
        s->control_input_request = true;
        s->control_stage = DWC3_CONTROL_DATA_OUT;
        break;
    }
}

static void rk3588_dwc3_try_setup(RK3588DWC3UDCState *s)
{
    USBSetupPacket setup;
    DWC3TRB trb;
    uint64_t buffer;
    uint32_t size;

    if (!s->ep_pending[0] || s->control_stage != DWC3_CONTROL_IDLE) {
        return;
    }

    if (s->enumeration_step >= 3 && s->input_count == 0) {
        return;
    }

    if (!rk3588_dwc3_read_trb(s->ep_trb[0], &trb) ||
        !(trb.ctrl & DWC3_TRB_CTRL_HWO) ||
        (trb.ctrl & DWC3_TRB_CTRL_TRBCTL_MASK) !=
        DWC3_TRBCTL_CONTROL_SETUP) {
        return;
    }

    rk3588_dwc3_make_setup(s, &setup);
    buffer = rk3588_dwc3_trb_buffer(&trb);
    size = trb.status & DWC3_TRB_STATUS_BUFSIZ_MASK;
    if (size < sizeof(setup) ||
        dma_memory_write_relaxed(&address_space_memory, buffer,
                                 &setup, sizeof(setup)) != MEMTX_OK) {
        s->control_stage = DWC3_CONTROL_IDLE;
        return;
    }

    rk3588_dwc3_complete_trb(s, 0, &trb, size - sizeof(setup));
}

static void rk3588_dwc3_setup_timer(void *opaque)
{
    RK3588DWC3UDCState *s = opaque;

    rk3588_dwc3_try_setup(s);
}

static void rk3588_dwc3_schedule_setup(RK3588DWC3UDCState *s)
{
    int64_t delay = s->enumeration_step < 3 ?
                    DWC3_ENUM_SETUP_DELAY_NS : DWC3_INPUT_SETUP_DELAY_NS;

    timer_mod(s->setup_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static void rk3588_dwc3_complete_control_data_out(
    RK3588DWC3UDCState *s, unsigned int ep, DWC3TRB *trb)
{
    uint32_t baud = CDC_INPUT_BAUD_BASE +
                    ((uint32_t)s->input_sequence << 8) + s->current_input;
    uint8_t line_coding[7] = {
        baud,
        baud >> 8,
        baud >> 16,
        baud >> 24,
        0,
        0,
        8,
    };
    uint32_t size = trb->status & DWC3_TRB_STATUS_BUFSIZ_MASK;
    uint32_t length = MIN(size, (uint32_t)sizeof(line_coding));

    if (s->control_stage != DWC3_CONTROL_DATA_OUT) {
        return;
    }

    if (dma_memory_write_relaxed(&address_space_memory,
                                 rk3588_dwc3_trb_buffer(trb),
                                 line_coding, length) != MEMTX_OK) {
        return;
    }

    s->control_stage = DWC3_CONTROL_STATUS_IN;
    rk3588_dwc3_complete_trb(s, ep, trb, size - length);
}

static void rk3588_dwc3_complete_control_in(RK3588DWC3UDCState *s,
                                            unsigned int ep, DWC3TRB *trb)
{
    uint32_t trbctl = trb->ctrl & DWC3_TRB_CTRL_TRBCTL_MASK;

    if (s->control_stage != DWC3_CONTROL_STATUS_IN ||
        (trbctl != DWC3_TRBCTL_CONTROL_STATUS_2 &&
         trbctl != DWC3_TRBCTL_CONTROL_STATUS_3)) {
        return;
    }

    rk3588_dwc3_complete_trb(s, ep, trb, 0);
    s->control_stage = DWC3_CONTROL_IDLE;

    if (s->control_input_request) {
        s->input_head = (s->input_head + 1) % DWC3_INPUT_FIFO_SIZE;
        s->input_count--;
        qemu_chr_fe_accept_input(&s->chr);
    } else if (s->enumeration_step < 3) {
        s->enumeration_step++;
    }
    s->control_input_request = false;
}

static void rk3588_dwc3_complete_bulk_in(RK3588DWC3UDCState *s,
                                         unsigned int ep, DWC3TRB *trb)
{
    uint32_t length = trb->status & DWC3_TRB_STATUS_BUFSIZ_MASK;
    uint8_t buffer[512];
    uint64_t addr = rk3588_dwc3_trb_buffer(trb);

    while (length) {
        uint32_t chunk = MIN(length, (uint32_t)sizeof(buffer));

        if (dma_memory_read_relaxed(&address_space_memory, addr,
                                    buffer, chunk) != MEMTX_OK) {
            return;
        }
        qemu_chr_fe_write_all(&s->chr, buffer, chunk);
        addr += chunk;
        length -= chunk;
    }

    rk3588_dwc3_complete_trb(s, ep, trb, 0);
}

static void rk3588_dwc3_process_ep(RK3588DWC3UDCState *s,
                                   unsigned int ep)
{
    DWC3TRB trb;
    uint32_t trbctl;

    if (ep >= DWC3_MAX_ENDPOINTS || !s->ep_pending[ep] ||
        !rk3588_dwc3_read_trb(s->ep_trb[ep], &trb) ||
        !(trb.ctrl & DWC3_TRB_CTRL_HWO)) {
        return;
    }

    trbctl = trb.ctrl & DWC3_TRB_CTRL_TRBCTL_MASK;
    if (ep == 0) {
        if (trbctl == DWC3_TRBCTL_CONTROL_SETUP) {
            rk3588_dwc3_schedule_setup(s);
        } else if (trbctl == DWC3_TRBCTL_CONTROL_DATA) {
            rk3588_dwc3_complete_control_data_out(s, ep, &trb);
        }
        return;
    }

    if (ep == 1) {
        rk3588_dwc3_complete_control_in(s, ep, &trb);
        return;
    }

    if ((ep & 1) && s->ep_type[ep] == DWC3_EP_TYPE_BULK &&
        (trbctl == DWC3_TRBCTL_NORMAL ||
         trbctl == DWC3_TRBCTL_NORMAL_ZLP)) {
        rk3588_dwc3_complete_bulk_in(s, ep, &trb);
    } else if ((ep & 1) &&
               s->ep_type[ep] == DWC3_EP_TYPE_INTERRUPT &&
               trbctl == DWC3_TRBCTL_NORMAL) {
        rk3588_dwc3_complete_trb(s, ep, &trb, 0);
    }
}

static void rk3588_dwc3_endpoint_command(RK3588DWC3UDCState *s,
                                         unsigned int ep, uint32_t command)
{
    uint32_t command_number = command & 0xf;
    uint32_t result = command & ~DWC3_DEPCMD_CMDACT;

    switch (command_number) {
    case DWC3_DEPCMD_DEPCFG:
        s->ep_type[ep] =
            (rk3588_dwc3_reg(s, DWC3_DEPCMDPAR0(ep)) >>
             DWC3_DEPCFG_EPTYPE_SHIFT) & DWC3_DEPCFG_EPTYPE_MASK;
        break;
    case DWC3_DEPCMD_DEPSTRTXFER:
        s->ep_trb[ep] =
            ((uint64_t)rk3588_dwc3_reg(s, DWC3_DEPCMDPAR0(ep)) << 32) |
            rk3588_dwc3_reg(s, DWC3_DEPCMDPAR1(ep));
        s->ep_pending[ep] = true;
        result |= DWC3_DEPCMD_RSCIDX;
        break;
    case DWC3_DEPCMD_DEPENDXFER:
        s->ep_pending[ep] = false;
        break;
    default:
        break;
    }

    rk3588_dwc3_set_reg(s, DWC3_DEPCMD(ep), result);
    if (command_number == DWC3_DEPCMD_DEPSTRTXFER) {
        rk3588_dwc3_process_ep(s, ep);
    }
}

static uint64_t rk3588_dwc3_read(void *opaque, hwaddr addr, unsigned int size)
{
    RK3588DWC3UDCState *s = opaque;

    if (addr >= DWC3_REG_BANK_SIZE) {
        return 0;
    }

    switch (addr) {
    case DWC3_GEVNTCOUNT:
        return s->event_count;
    default:
        return rk3588_dwc3_reg(s, addr);
    }
}

static void rk3588_dwc3_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    RK3588DWC3UDCState *s = opaque;
    uint32_t old_value;
    uint32_t new_value = value;

    if (addr >= DWC3_REG_BANK_SIZE) {
        return;
    }

    old_value = rk3588_dwc3_reg(s, addr);
    switch (addr) {
    case DWC3_GEVNTADR_LO:
        rk3588_dwc3_set_reg(s, addr, new_value);
        s->event_addr = (s->event_addr & 0xffffffff00000000ULL) | new_value;
        break;
    case DWC3_GEVNTADR_HI:
        rk3588_dwc3_set_reg(s, addr, new_value);
        s->event_addr = (s->event_addr & UINT32_MAX) |
                        ((uint64_t)new_value << 32);
        break;
    case DWC3_GEVNTSIZ:
        rk3588_dwc3_set_reg(s, addr, new_value);
        s->event_size = new_value & 0xffff;
        if (s->event_prod >= s->event_size) {
            s->event_prod = 0;
        }
        rk3588_dwc3_update_irq(s);
        break;
    case DWC3_GEVNTCOUNT: {
        uint32_t consumed = MIN(new_value & 0xffff, s->event_count);

        s->event_count -= consumed;
        rk3588_dwc3_update_irq(s);
        break;
    }
    case DWC3_DCTL:
        if (new_value & DWC3_DCTL_CSFTRST) {
            rk3588_dwc3_reset_state(s);
            break;
        }
        new_value &= ~DWC3_DCTL_CSFTRST;
        rk3588_dwc3_set_reg(s, addr, new_value);
        if (!(old_value & DWC3_DCTL_RUNSTOP) &&
            (new_value & DWC3_DCTL_RUNSTOP)) {
            rk3588_dwc3_push_event(s, DWC3_DEVT_USBRST);
            rk3588_dwc3_push_event(s, DWC3_DEVT_CONNECTDONE);
        }
        break;
    default:
        if (addr >= DWC3_DEPCMD(0) &&
            addr <= DWC3_DEPCMD(DWC3_MAX_ENDPOINTS - 1) &&
            (addr & 0xf) == 0xc) {
            unsigned int ep = (addr - DWC3_DEPCMD(0)) / 16;

            rk3588_dwc3_endpoint_command(s, ep, new_value);
        } else {
            rk3588_dwc3_set_reg(s, addr, new_value);
        }
        break;
    }
}

static const MemoryRegionOps rk3588_dwc3_ops = {
    .read = rk3588_dwc3_read,
    .write = rk3588_dwc3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static int rk3588_dwc3_can_receive(void *opaque)
{
    RK3588DWC3UDCState *s = opaque;

    return DWC3_INPUT_FIFO_SIZE - s->input_count;
}

static void rk3588_dwc3_receive(void *opaque, const uint8_t *buffer, int size)
{
    RK3588DWC3UDCState *s = opaque;

    while (size-- > 0 && s->input_count < DWC3_INPUT_FIFO_SIZE) {
        s->input[s->input_tail] = *buffer++;
        s->input_tail = (s->input_tail + 1) % DWC3_INPUT_FIFO_SIZE;
        s->input_count++;
    }

    rk3588_dwc3_schedule_setup(s);
}

static void rk3588_dwc3_reset_state(RK3588DWC3UDCState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ep_type, 0, sizeof(s->ep_type));
    memset(s->ep_trb, 0, sizeof(s->ep_trb));
    memset(s->ep_pending, 0, sizeof(s->ep_pending));
    rk3588_dwc3_set_reg(s, DWC3_GSBUSCFG0, 0x00000000);
    rk3588_dwc3_set_reg(s, DWC3_GCTL, 0x30c13004);
    rk3588_dwc3_set_reg(s, DWC3_GCOREID, 0x5533330a);
    rk3588_dwc3_set_reg(s, DWC3_GUSB2PHYCFG, 0x40102410);
    rk3588_dwc3_set_reg(s, DWC3_GUSB3PIPECTL, 0x02000000);
    rk3588_dwc3_set_reg(s, DWC3_DSTS, 0);

    s->event_addr = 0;
    s->event_size = 0;
    s->event_count = 0;
    s->event_prod = 0;
    s->input_head = 0;
    s->input_tail = 0;
    s->input_count = 0;
    s->input_sequence = 0;
    s->current_input = 0;
    s->enumeration_step = 0;
    s->control_stage = DWC3_CONTROL_IDLE;
    s->control_input_request = false;
    timer_del(s->setup_timer);
    qemu_set_irq(s->irq, 0);
}

static void rk3588_dwc3_reset(DeviceState *dev)
{
    rk3588_dwc3_reset_state(RK3588_DWC3_UDC(dev));
}

static void rk3588_dwc3_realize(DeviceState *dev, Error **errp)
{
    RK3588DWC3UDCState *s = RK3588_DWC3_UDC(dev);

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_set_handlers(&s->chr, rk3588_dwc3_can_receive,
                                 rk3588_dwc3_receive, NULL, NULL,
                                 s, NULL, true);
    }
}

static bool rk3588_dwc3_migration_valid(RK3588DWC3UDCState *s)
{
    if (s->event_count > s->event_size ||
        (s->event_size && s->event_prod >= s->event_size) ||
        (!s->event_size && s->event_prod) ||
        s->input_head >= DWC3_INPUT_FIFO_SIZE ||
        s->input_tail >= DWC3_INPUT_FIFO_SIZE ||
        s->input_count > DWC3_INPUT_FIFO_SIZE ||
        s->enumeration_step > 3 ||
        s->control_stage > DWC3_CONTROL_STATUS_IN) {
        return false;
    }

    for (unsigned int ep = 0; ep < DWC3_MAX_ENDPOINTS; ep++) {
        if (s->ep_type[ep] > DWC3_DEPCFG_EPTYPE_MASK) {
            return false;
        }
    }

    return true;
}

static int rk3588_dwc3_post_load(void *opaque, int version_id)
{
    RK3588DWC3UDCState *s = opaque;

    if (!rk3588_dwc3_migration_valid(s)) {
        return -EINVAL;
    }

    rk3588_dwc3_update_irq(s);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
    return 0;
}

static const VMStateDescription vmstate_rk3588_dwc3_udc = {
    .name = TYPE_RK3588_DWC3_UDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rk3588_dwc3_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3588DWC3UDCState,
                             DWC3_REG_BANK_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT8_ARRAY(ep_type, RK3588DWC3UDCState,
                            DWC3_MAX_ENDPOINTS),
        VMSTATE_UINT64_ARRAY(ep_trb, RK3588DWC3UDCState,
                             DWC3_MAX_ENDPOINTS),
        VMSTATE_BOOL_ARRAY(ep_pending, RK3588DWC3UDCState,
                           DWC3_MAX_ENDPOINTS),
        VMSTATE_UINT64(event_addr, RK3588DWC3UDCState),
        VMSTATE_UINT32(event_size, RK3588DWC3UDCState),
        VMSTATE_UINT32(event_count, RK3588DWC3UDCState),
        VMSTATE_UINT32(event_prod, RK3588DWC3UDCState),
        VMSTATE_UINT8_ARRAY(input, RK3588DWC3UDCState,
                            DWC3_INPUT_FIFO_SIZE),
        VMSTATE_UINT16(input_head, RK3588DWC3UDCState),
        VMSTATE_UINT16(input_tail, RK3588DWC3UDCState),
        VMSTATE_UINT16(input_count, RK3588DWC3UDCState),
        VMSTATE_UINT8(input_sequence, RK3588DWC3UDCState),
        VMSTATE_UINT8(current_input, RK3588DWC3UDCState),
        VMSTATE_UINT8(enumeration_step, RK3588DWC3UDCState),
        VMSTATE_UINT32(control_stage, RK3588DWC3UDCState),
        VMSTATE_BOOL(control_input_request, RK3588DWC3UDCState),
        VMSTATE_TIMER_PTR(setup_timer, RK3588DWC3UDCState),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property rk3588_dwc3_properties[] = {
    DEFINE_PROP_CHR("chardev", RK3588DWC3UDCState, chr),
};

static void rk3588_dwc3_init(Object *obj)
{
    RK3588DWC3UDCState *s = RK3588_DWC3_UDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rk3588_dwc3_ops, s,
                          TYPE_RK3588_DWC3_UDC,
                          RK3588_DWC3_UDC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->setup_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  rk3588_dwc3_setup_timer, s);
}

static void rk3588_dwc3_finalize(Object *obj)
{
    RK3588DWC3UDCState *s = RK3588_DWC3_UDC(obj);

    timer_free(s->setup_timer);
}

static void rk3588_dwc3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rk3588_dwc3_realize;
    dc->vmsd = &vmstate_rk3588_dwc3_udc;
    device_class_set_legacy_reset(dc, rk3588_dwc3_reset);
    device_class_set_props(dc, rk3588_dwc3_properties);
}

static const TypeInfo rk3588_dwc3_info = {
    .name = TYPE_RK3588_DWC3_UDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588DWC3UDCState),
    .instance_init = rk3588_dwc3_init,
    .instance_finalize = rk3588_dwc3_finalize,
    .class_init = rk3588_dwc3_class_init,
};

static void rk3588_dwc3_register_types(void)
{
    type_register_static(&rk3588_dwc3_info);
}

type_init(rk3588_dwc3_register_types)
