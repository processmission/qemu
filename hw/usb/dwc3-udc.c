/*
 * Synopsys DWC3 USB device-mode controller (UDC) with an internal CDC ACM
 * host bridge
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/usb/dwc3-udc.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/dma.h"

/* Register bank implemented by the device; the MMIO window is 4 MiB. */
#define DWC3_REG_BANK_SIZE              0x10000
#define DWC3_REG_BANK_WORDS             (DWC3_REG_BANK_SIZE / sizeof(uint32_t))

#define DWC3_MAX_ENDPOINTS              32
#define DWC3_INPUT_FIFO_SIZE            256

/* DWC3 global registers. */
#define DWC3_GSBUSCFG0                  0xc100
#define DWC3_GCTL                       0xc110
#define DWC3_GCOREID                    0xc120
#define DWC3_GHWPARAMS0                 0xc140
#define DWC3_GHWPARAMS1                 0xc144
#define DWC3_GHWPARAMS3                 0xc14c
#define DWC3_GHWPARAMS7                 0xc15c
#define DWC3_GTXFIFOSIZ(ep)             (0xc300 + 4 * (ep))
#define DWC3_GRXFIFOSIZ                 0xc380
#define DWC3_GUSB2PHYCFG                0xc200
#define DWC3_GUSB3PIPECTL               0xc2c0
#define DWC3_GEVNTADR_LO                0xc400
#define DWC3_GEVNTADR_HI                0xc404
#define DWC3_GEVNTSIZ                   0xc408
#define DWC3_GEVNTCOUNT                 0xc40c

/* DWC3 device registers. */
#define DWC3_DCFG                       0xc700
#define DWC3_DCTL                       0xc704
#define DWC3_DEVTEN                     0xc708
#define DWC3_DSTS                       0xc70c
#define DWC3_DGCMDPAR                   0xc710
#define DWC3_DGCMD                      0xc714
#define DWC3_DALEPENA                   0xc720

/*
 * Endpoint command registers.  Per the databook PAR2 is the lowest offset of
 * the four-register group; the firmware UDC driver uses the same layout.
 */
#define DWC3_DEPCMDPAR2(ep)             (0xc800 + 16 * (ep))
#define DWC3_DEPCMDPAR1(ep)             (0xc804 + 16 * (ep))
#define DWC3_DEPCMDPAR0(ep)             (0xc808 + 16 * (ep))
#define DWC3_DEPCMD(ep)                 (0xc80c + 16 * (ep))

/* Reset values exposed by the DWC3 IP; used by the proven downstream model. */
#define DWC3_GSBUSCFG0_RESET            0x00000000
#define DWC3_GCTL_RESET                 0x30c13004
#define DWC3_GCOREID_RESET              0x5533330a
#define DWC3_GUSB2PHYCFG_RESET          0x40102410
#define DWC3_GUSB3PIPECTL_RESET         0x02000000
#define DWC3_DSTS_DEVCTRLHLT            BIT(22)
#define DWC3_DSTS_USBLNKST_RX_DET        (5 << 18)
#define DWC3_DSTS_RESET                 (DWC3_DSTS_DEVCTRLHLT | \
                                        DWC3_DSTS_USBLNKST_RX_DET)

#define DWC3_GEVNTSIZ_INTMASK           BIT(31)

#define DWC3_DCTL_CSFTRST               BIT(30)
#define DWC3_DCTL_RUNSTOP               BIT(31)

#define DWC3_DEPCMD_CMDACT              BIT(10)
/* Resource index 1, echoed back by the driver in DEPENDXFER flags. */
#define DWC3_DEPCMD_RSCIDX              BIT(16)
#define DWC3_DEPCMD_DEPCFG              1
#define DWC3_DEPCMD_DEPXFERCFG          2
#define DWC3_DEPCMD_DEPSETSTALL         4
#define DWC3_DEPCMD_DEPCLEARSTALL       5
#define DWC3_DEPCMD_DEPSTRTXFER         6
#define DWC3_DEPCMD_DEPENDXFER          8
#define DWC3_DEPCMD_DEPSTARTCFG         9

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

/*
 * Minimal-host CDC ACM requests.  The enumeration script matches what the
 * proven model issued to the Zephyr UDC/CDC stack.
 */
#define USB_REQTYPE_CLASS_INTERFACE_OUT 0x21
#define USB_REQ_SET_ADDRESS             0x05
#define USB_REQ_SET_CONFIGURATION       0x09
#define USB_CDC_SET_LINE_CODING         0x20
#define USB_CDC_SET_CONTROL_LINE_STATE  0x22
#define USB_CDC_CONTROL_DTR             BIT(0)
#define CDC_INPUT_BAUD_BASE             1000000U

/*
 * Pacing of the emulated host requests, in virtual-clock nanoseconds.
 * 1 ms between enumeration requests; console characters are spaced further
 * apart: the control-shell bridge samples the CDC line coding every
 * 2 ms and decodes one byte per baud-rate change, so a byte can be lost if
 * two line-coding updates land inside one delayed poll.  25 ms is still much
 * faster than a human types and is validated under RTL8125 + VM load.
 */
#define DWC3_ENUM_SETUP_DELAY_NS        1000000
#define DWC3_INPUT_SETUP_DELAY_NS       50000000
/* Retry pacing for a setup request whose endpoint is transiently busy. */
#define DWC3_SETUP_RETRY_NS             5000000
/* CDC bulk-IN data buffered while the chardev backend is closed/short. */
#define DWC3_TX_FIFO_SIZE               4096
#define DWC3_TX_RETRY_NS                2000000

typedef struct DWC3TRB {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t status;
    uint32_t ctrl;
} DWC3TRB;

typedef struct DWC3SetupPacket {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} QEMU_PACKED DWC3SetupPacket;

typedef enum DWC3ControlStage {
    DWC3_CONTROL_IDLE,
    DWC3_CONTROL_DATA_OUT,
    DWC3_CONTROL_STATUS_IN,
} DWC3ControlStage;

struct DWC3UDCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharFrontend chr;
    QEMUTimer *setup_timer;

    /* Guest-visible DWC3 register bank and endpoint state. */
    uint32_t regs[DWC3_REG_BANK_WORDS];
    uint8_t ep_type[DWC3_MAX_ENDPOINTS];
    uint64_t ep_trb[DWC3_MAX_ENDPOINTS];
    bool ep_pending[DWC3_MAX_ENDPOINTS];
    bool ep_stalled[DWC3_MAX_ENDPOINTS];

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

    /*
     * Local logging caches; deliberately not migrated because they only gate
     * diagnostic output.
     */
    bool unimp_window_read_logged;
    bool unimp_window_write_logged;
    bool unimp_no_backend_logged;
    uint16_t unimp_cmd_logged;

    /*
     * CDC bulk-IN bytes that the chardev backend has not accepted yet.  A
     * socket backend reports an error while it has no client connected, so
     * console output must be retained instead of silently dropped; it is
     * flushed on CHR_EVENT_OPENED and retried on a short timer.
     */
    uint8_t tx_buf[DWC3_TX_FIFO_SIZE];
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t tx_count;
    QEMUTimer *tx_timer;
    bool tx_error_logged;
    bool runstop_retry;

    /* Opt-in diagnostic tracing, see the "debug" property. */
    bool debug;
    bool host_connected;
    int64_t depcmd_write_ns[DWC3_MAX_ENDPOINTS];
};

static void dwc3_udc_reset_state(DWC3UDCState *s);

static uint32_t dwc3_udc_reg(DWC3UDCState *s, hwaddr addr)
{
    return s->regs[addr / sizeof(uint32_t)];
}

static void dwc3_udc_set_reg(DWC3UDCState *s, hwaddr addr, uint32_t value)
{
    s->regs[addr / sizeof(uint32_t)] = value;
}

/*
 * Diagnostic visibility for the intentionally unimplemented paths.  The model
 * never fakes a success status for these paths; they are simply not part of
 * the CDC console contract.
 */
static void dwc3_udc_log_unimp_window(DWC3UDCState *s, hwaddr addr,
                                      bool is_write)
{
    bool *logged = is_write ? &s->unimp_window_write_logged :
                              &s->unimp_window_read_logged;

    if (*logged) {
        return;
    }
    *logged = true;
    qemu_log_mask(LOG_UNIMP,
                  TYPE_DWC3_UDC ": unimplemented %s at 0x%" HWADDR_PRIx
                  " (implemented register bank is 0x%x bytes)\n",
                  is_write ? "write" : "read", addr, DWC3_REG_BANK_SIZE);
}

static void dwc3_udc_log_unimp_command(DWC3UDCState *s, unsigned int ep,
                                       uint32_t command)
{
    uint32_t cmd = command & 0xf;

    if (s->unimp_cmd_logged & BIT(cmd)) {
        return;
    }
    s->unimp_cmd_logged |= BIT(cmd);
    qemu_log_mask(LOG_UNIMP,
                  TYPE_DWC3_UDC ": unimplemented endpoint command 0x%x on "
                  "ep %u (command error)\n", cmd, ep);
}

static void dwc3_udc_log_no_backend(DWC3UDCState *s)
{
    if (s->unimp_no_backend_logged) {
        return;
    }
    s->unimp_no_backend_logged = true;
    qemu_log_mask(LOG_UNIMP,
                  TYPE_DWC3_UDC ": CDC bulk IN data dropped, no chardev "
                  "backend connected\n");
}

/* Opt-in diagnostic tracing: -global driver=dwc3-udc,property=debug,value=on */
#define DWC3_UDC_DBG(s, fmt, ...)                                        \
    do {                                                                 \
        if ((s)->debug) {                                                \
            qemu_log_mask(LOG_UNIMP, TYPE_DWC3_UDC ": " fmt,             \
                          ## __VA_ARGS__);                               \
        }                                                                \
    } while (0)

static void dwc3_udc_update_irq(DWC3UDCState *s)
{
    bool masked = dwc3_udc_reg(s, DWC3_GEVNTSIZ) & DWC3_GEVNTSIZ_INTMASK;

    qemu_set_irq(s->irq, s->event_count != 0 && !masked);
}

/*
 * CDC bulk-IN transmit path.
 *
 * The device must not depend on the chardev being open when console output is
 * produced: a unix-socket backend with server=on,wait=off is disconnected
 * until a client connects and reports EIO/EAGAIN for writes in the meantime,
 * and a short write may also happen under load.  Bytes are therefore queued in
 * a bounded FIFO and drained with non-blocking writes; the queue is flushed
 * when the chardev reports CHR_EVENT_OPENED and retried on a 2 ms timer.  The
 * fast path (backend connected and writable) drains in the same call, so the
 * normal case has no added latency.
 */
static void dwc3_udc_tx_flush(DWC3UDCState *s)
{
    while (s->tx_count && qemu_chr_fe_backend_connected(&s->chr)) {
        uint32_t chunk = MIN(s->tx_count, DWC3_TX_FIFO_SIZE - s->tx_head);
        int ret;

        ret = qemu_chr_fe_write(&s->chr, &s->tx_buf[s->tx_head], chunk);
        if (ret <= 0) {
            if (!s->tx_error_logged) {
                s->tx_error_logged = true;
                DWC3_UDC_DBG(s, "tx: backend not writable (%d), %u bytes "
                             "retained\n", ret, s->tx_count);
            }
            break;
        }
        s->tx_error_logged = false;
        s->tx_head = (s->tx_head + ret) % DWC3_TX_FIFO_SIZE;
        s->tx_count -= ret;
    }

    if (s->tx_count && qemu_chr_fe_backend_connected(&s->chr)) {
        timer_mod(s->tx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + DWC3_TX_RETRY_NS);
    }
}

static void dwc3_udc_tx_timer(void *opaque)
{
    DWC3UDCState *s = opaque;

    dwc3_udc_tx_flush(s);
}

static void dwc3_udc_tx_enqueue(DWC3UDCState *s, const uint8_t *buf,
                                uint32_t len)
{
    if (len > DWC3_TX_FIFO_SIZE - s->tx_count) {
        uint32_t dropped = len - (DWC3_TX_FIFO_SIZE - s->tx_count);

        qemu_log_mask(LOG_UNIMP,
                      TYPE_DWC3_UDC ": TX FIFO full, dropping %u of %u "
                      "bytes\n", dropped, len);
        len = DWC3_TX_FIFO_SIZE - s->tx_count;
    }

    while (len--) {
        s->tx_buf[s->tx_tail] = *buf++;
        s->tx_tail = (s->tx_tail + 1) % DWC3_TX_FIFO_SIZE;
        s->tx_count++;
    }

    dwc3_udc_tx_flush(s);
}

static void dwc3_udc_chr_event(void *opaque, QEMUChrEvent event)
{
    DWC3UDCState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        DWC3_UDC_DBG(s, "chardev opened, flushing %u TX bytes\n",
                     s->tx_count);
        /*
         * The backend may have become writable after console output was
         * produced; deliver the retained bytes before accepting new input.
         */
        dwc3_udc_tx_flush(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case CHR_EVENT_CLOSED:
        DWC3_UDC_DBG(s, "chardev closed\n");
        break;
    default:
        break;
    }
}

static void dwc3_udc_push_event(DWC3UDCState *s, uint32_t event)
{
    uint32_t value = cpu_to_le32(event);

    if (s->event_size < sizeof(value)) {
        qemu_log_mask(LOG_UNIMP,
                      TYPE_DWC3_UDC ": event 0x%x dropped, event ring is "
                      "disabled (size %u)\n", event, s->event_size);
        return;
    }

    if (s->event_count + sizeof(value) > s->event_size) {
        /*
         * The proven model drops events instead of raising EVNTOVERFLOW; keep
         * that behaviour so the observed enumeration timing is unchanged.
         */
        qemu_log_mask(LOG_UNIMP,
                      TYPE_DWC3_UDC ": event 0x%x dropped, event ring full "
                      "(count %u size %u)\n", event, s->event_count,
                      s->event_size);
        return;
    }

    if (dma_memory_write_relaxed(&address_space_memory,
                                 s->event_addr + s->event_prod,
                                 &value, sizeof(value)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": event write failed at 0x%" HWADDR_PRIx
                      "\n", (hwaddr)(s->event_addr + s->event_prod));
        return;
    }
    s->event_prod += sizeof(value);
    if (s->event_prod >= s->event_size) {
        s->event_prod = 0;
    }
    s->event_count += sizeof(value);
    DWC3_UDC_DBG(s, "event 0x%x queued count=%u prod=%u\n",
                 event, s->event_count, s->event_prod);
    dwc3_udc_update_irq(s);
}

static bool dwc3_udc_read_trb(uint64_t addr, DWC3TRB *trb)
{
    DWC3TRB raw;

    if (dma_memory_read_relaxed(&address_space_memory, addr,
                                &raw, sizeof(raw)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": TRB read failed at 0x%" HWADDR_PRIx
                      "\n", (hwaddr)addr);
        return false;
    }

    trb->addr_lo = le32_to_cpu(raw.addr_lo);
    trb->addr_hi = le32_to_cpu(raw.addr_hi);
    trb->status = le32_to_cpu(raw.status);
    trb->ctrl = le32_to_cpu(raw.ctrl);
    return true;
}

static bool dwc3_udc_write_trb(uint64_t addr, const DWC3TRB *trb)
{
    DWC3TRB raw = {
        .addr_lo = cpu_to_le32(trb->addr_lo),
        .addr_hi = cpu_to_le32(trb->addr_hi),
        .status = cpu_to_le32(trb->status),
        .ctrl = cpu_to_le32(trb->ctrl),
    };

    if (dma_memory_write_relaxed(&address_space_memory, addr,
                                 &raw, sizeof(raw)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": TRB write failed at 0x%" HWADDR_PRIx
                      "\n", (hwaddr)addr);
        return false;
    }
    return true;
}

static uint64_t dwc3_udc_trb_buffer(const DWC3TRB *trb)
{
    return ((uint64_t)trb->addr_hi << 32) | trb->addr_lo;
}

static void dwc3_udc_complete_trb(DWC3UDCState *s, unsigned int ep,
                                  DWC3TRB *trb, uint32_t remaining)
{
    trb->status = remaining & DWC3_TRB_STATUS_BUFSIZ_MASK;
    trb->ctrl &= ~DWC3_TRB_CTRL_HWO;
    if (!dwc3_udc_write_trb(s->ep_trb[ep], trb)) {
        return;
    }

    s->ep_pending[ep] = false;
    dwc3_udc_push_event(s, DWC3_DEPEVT_XFERCOMPLETE(ep));
}

static void dwc3_udc_make_setup(DWC3UDCState *s, DWC3SetupPacket *setup)
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

static void dwc3_udc_try_setup(DWC3UDCState *s)
{
    DWC3SetupPacket setup;
    DWC3TRB trb;
    uint64_t buffer;
    uint32_t size;

    if (!s->host_connected) {
        return;
    }
    if (!s->ep_pending[0] || s->control_stage != DWC3_CONTROL_IDLE) {
        DWC3_UDC_DBG(s, "try_setup skip pending=%d stage=%u step=%u "
                     "input=%u\n", s->ep_pending[0], s->control_stage,
                     s->enumeration_step, s->input_count);
        /*
         * If console input is waiting because the firmware workqueue has not
         * re-armed the endpoint yet (or a previous stage is still settling),
         * retry: a lost one-shot trigger must never strand the input FIFO.
         */
        if (s->input_count) {
            timer_mod(s->setup_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      DWC3_SETUP_RETRY_NS);
        }
        return;
    }

    if (s->enumeration_step >= 3 && s->input_count == 0) {
        return;
    }

    if (!dwc3_udc_read_trb(s->ep_trb[0], &trb) ||
        !(trb.ctrl & DWC3_TRB_CTRL_HWO) ||
        (trb.ctrl & DWC3_TRB_CTRL_TRBCTL_MASK) !=
        DWC3_TRBCTL_CONTROL_SETUP) {
        return;
    }

    dwc3_udc_make_setup(s, &setup);
    buffer = dwc3_udc_trb_buffer(&trb);
    size = trb.status & DWC3_TRB_STATUS_BUFSIZ_MASK;
    if (size < sizeof(setup)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": SETUP buffer too small (%u bytes)\n",
                      size);
        s->control_stage = DWC3_CONTROL_IDLE;
        return;
    }
    if (dma_memory_write_relaxed(&address_space_memory, buffer,
                                 &setup, sizeof(setup)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": SETUP write failed at 0x%" HWADDR_PRIx
                      "\n", (hwaddr)buffer);
        s->control_stage = DWC3_CONTROL_IDLE;
        return;
    }

    DWC3_UDC_DBG(s, "inject step=%u request=0x%02x value=0x%x len=%u\n",
                 s->enumeration_step, setup.request,
                 le16_to_cpu(setup.value), setup.length);
    dwc3_udc_complete_trb(s, 0, &trb, size - sizeof(setup));
}

static void dwc3_udc_setup_timer(void *opaque)
{
    DWC3UDCState *s = opaque;

    dwc3_udc_try_setup(s);
}

static void dwc3_udc_schedule_setup(DWC3UDCState *s)
{
    int64_t delay = s->enumeration_step < 3 ?
                    DWC3_ENUM_SETUP_DELAY_NS : DWC3_INPUT_SETUP_DELAY_NS;

    timer_mod(s->setup_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static void dwc3_udc_complete_control_data_out(DWC3UDCState *s,
                                               unsigned int ep, DWC3TRB *trb)
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
                                 dwc3_udc_trb_buffer(trb),
                                 line_coding, length) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": CDC line coding write failed at "
                      "0x%" HWADDR_PRIx "\n",
                      (hwaddr)dwc3_udc_trb_buffer(trb));
        return;
    }

    DWC3_UDC_DBG(s, "line-coding char=0x%02x seq=%u baud=%u size=%u\n",
                 s->current_input, s->input_sequence, baud, size);
    s->control_stage = DWC3_CONTROL_STATUS_IN;
    dwc3_udc_complete_trb(s, ep, trb, size - length);
}

static void dwc3_udc_complete_control_in(DWC3UDCState *s, unsigned int ep,
                                         DWC3TRB *trb)
{
    uint32_t trbctl = trb->ctrl & DWC3_TRB_CTRL_TRBCTL_MASK;

    if (s->control_stage != DWC3_CONTROL_STATUS_IN ||
        (trbctl != DWC3_TRBCTL_CONTROL_STATUS_2 &&
         trbctl != DWC3_TRBCTL_CONTROL_STATUS_3)) {
        return;
    }

    DWC3_UDC_DBG(s, "status-in ep=%u step=%u input_req=%d input_count=%u\n",
                 ep, s->enumeration_step, s->control_input_request,
                 s->input_count);
    dwc3_udc_complete_trb(s, ep, trb, 0);
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

static void dwc3_udc_complete_bulk_in(DWC3UDCState *s, unsigned int ep,
                                      DWC3TRB *trb)
{
    uint32_t length = trb->status & DWC3_TRB_STATUS_BUFSIZ_MASK;
    uint8_t buffer[512];
    uint64_t addr = dwc3_udc_trb_buffer(trb);
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        dwc3_udc_log_no_backend(s);
    }

    while (length) {
        uint32_t chunk = MIN(length, (uint32_t)sizeof(buffer));

        if (dma_memory_read_relaxed(&address_space_memory, addr,
                                    buffer, chunk) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          TYPE_DWC3_UDC ": bulk IN data read failed at "
                          "0x%" HWADDR_PRIx "\n", (hwaddr)addr);
            return;
        }
        dwc3_udc_tx_enqueue(s, buffer, chunk);
        addr += chunk;
        length -= chunk;
    }

    DWC3_UDC_DBG(s, "bulk-in ep=%u len=%u backend=%d tx_pending=%u "
                 "handler=%" PRId64 " us\n",
                 ep, trb->status & DWC3_TRB_STATUS_BUFSIZ_MASK,
                 qemu_chr_fe_backend_connected(&s->chr), s->tx_count,
                 (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0) / 1000);
    dwc3_udc_complete_trb(s, ep, trb, 0);
}

static void dwc3_udc_process_ep(DWC3UDCState *s, unsigned int ep)
{
    DWC3TRB trb;
    uint32_t trbctl;

    if (!s->host_connected || ep >= DWC3_MAX_ENDPOINTS || s->ep_stalled[ep] ||
        !s->ep_pending[ep] ||
        !dwc3_udc_read_trb(s->ep_trb[ep], &trb) ||
        !(trb.ctrl & DWC3_TRB_CTRL_HWO)) {
        return;
    }

    trbctl = trb.ctrl & DWC3_TRB_CTRL_TRBCTL_MASK;
    DWC3_UDC_DBG(s, "process_ep ep=%u trbctl=0x%x type=%u\n",
                 ep, trbctl, s->ep_type[ep]);
    if (ep == 0) {
        if (trbctl == DWC3_TRBCTL_CONTROL_SETUP) {
            dwc3_udc_schedule_setup(s);
        } else if (trbctl == DWC3_TRBCTL_CONTROL_DATA) {
            dwc3_udc_complete_control_data_out(s, ep, &trb);
        }
        return;
    }

    if (ep == 1) {
        dwc3_udc_complete_control_in(s, ep, &trb);
        return;
    }

    if ((ep & 1) && s->ep_type[ep] == DWC3_EP_TYPE_BULK &&
        (trbctl == DWC3_TRBCTL_NORMAL ||
         trbctl == DWC3_TRBCTL_NORMAL_ZLP)) {
        dwc3_udc_complete_bulk_in(s, ep, &trb);
    } else if ((ep & 1) &&
               s->ep_type[ep] == DWC3_EP_TYPE_INTERRUPT &&
               trbctl == DWC3_TRBCTL_NORMAL) {
        dwc3_udc_complete_trb(s, ep, &trb, 0);
    }
}

static void dwc3_udc_endpoint_command(DWC3UDCState *s, unsigned int ep,
                                      uint32_t command)
{
    uint32_t command_number = command & 0xf;
    uint32_t result = command & ~(DWC3_DEPCMD_CMDACT | (0xf << 12));

    s->depcmd_write_ns[ep] = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    DWC3_UDC_DBG(s, "DEPCMD ep=%u cmd=%u command=0x%x\n",
                 ep, command_number, command);
    switch (command_number) {
    case DWC3_DEPCMD_DEPCFG:
        s->ep_type[ep] =
            (dwc3_udc_reg(s, DWC3_DEPCMDPAR0(ep)) >>
             DWC3_DEPCFG_EPTYPE_SHIFT) & DWC3_DEPCFG_EPTYPE_MASK;
        break;
    case DWC3_DEPCMD_DEPSTRTXFER:
        s->ep_trb[ep] =
            ((uint64_t)dwc3_udc_reg(s, DWC3_DEPCMDPAR0(ep)) << 32) |
            dwc3_udc_reg(s, DWC3_DEPCMDPAR1(ep));
        s->ep_pending[ep] = true;
        result |= DWC3_DEPCMD_RSCIDX;
        break;
    case DWC3_DEPCMD_DEPENDXFER:
        s->ep_pending[ep] = false;
        break;
    case DWC3_DEPCMD_DEPSETSTALL:
        s->ep_stalled[ep] = true;
        break;
    case DWC3_DEPCMD_DEPCLEARSTALL:
        s->ep_stalled[ep] = false;
        break;
    case DWC3_DEPCMD_DEPXFERCFG:
        if ((dwc3_udc_reg(s, DWC3_DEPCMDPAR0(ep)) & 0xffff) != 1) {
            result |= BIT(12);
        }
        break;
    case DWC3_DEPCMD_DEPSTARTCFG:
        for (unsigned i = (command >> 16) & 0x7f; i < DWC3_MAX_ENDPOINTS; i++) {
            s->ep_pending[i] = false;
            s->ep_stalled[i] = false;
        }
        break;
    default:
        dwc3_udc_log_unimp_command(s, ep, command_number);
        result |= BIT(12);
        break;
    }

    dwc3_udc_set_reg(s, DWC3_DEPCMD(ep), result);
    if (command & BIT(8)) {
        dwc3_udc_push_event(s, (ep << 1) | (7 << 6) |
                            (result & (0xf << 12)) | (command_number << 24));
    }
    if (command_number == DWC3_DEPCMD_DEPSTRTXFER) {
        dwc3_udc_process_ep(s, ep);
    }
}

static uint64_t dwc3_udc_read(void *opaque, hwaddr addr, unsigned int size)
{
    DWC3UDCState *s = opaque;

    if (addr >= DWC3_REG_BANK_SIZE) {
        dwc3_udc_log_unimp_window(s, addr, false);
        return 0;
    }
    if (addr & 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": invalid unaligned read at "
                      "0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }

    if (addr >= DWC3_DEPCMD(0) &&
        addr <= DWC3_DEPCMD(DWC3_MAX_ENDPOINTS - 1) &&
        (addr & 0xf) == 0xc) {
        unsigned int ep = (addr - DWC3_DEPCMD(0)) / 16;
        int64_t gap = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                      s->depcmd_write_ns[ep];

        DWC3_UDC_DBG(s, "DEPCMD ep=%u poll gap=%" PRId64 " us reg=0x%x\n",
                     ep, gap / 1000, dwc3_udc_reg(s, addr));
    }

    switch (addr) {
    case DWC3_GEVNTCOUNT:
        return s->event_count;
    default:
        return dwc3_udc_reg(s, addr);
    }
}

static void dwc3_udc_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned int size)
{
    DWC3UDCState *s = opaque;
    uint32_t old_value;
    uint32_t new_value = value;

    if (addr >= DWC3_REG_BANK_SIZE) {
        dwc3_udc_log_unimp_window(s, addr, true);
        return;
    }
    if (addr & 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": invalid unaligned write at "
                      "0x%" HWADDR_PRIx "\n", addr);
        return;
    }

    old_value = dwc3_udc_reg(s, addr);
    switch (addr) {
    case DWC3_GEVNTADR_LO:
        dwc3_udc_set_reg(s, addr, new_value);
        s->event_addr = (s->event_addr & 0xffffffff00000000ULL) | new_value;
        break;
    case DWC3_GEVNTADR_HI:
        dwc3_udc_set_reg(s, addr, new_value);
        s->event_addr = (s->event_addr & UINT32_MAX) |
                        ((uint64_t)new_value << 32);
        break;
    case DWC3_GEVNTSIZ:
        dwc3_udc_set_reg(s, addr, new_value);
        s->event_size = new_value & 0xffff;
        if (s->event_prod >= s->event_size) {
            s->event_prod = 0;
        }
        if (s->runstop_retry &&
            s->event_size >= 2 * sizeof(uint32_t) &&
            (dwc3_udc_reg(s, DWC3_DCTL) & DWC3_DCTL_RUNSTOP)) {
            /*
             * Run/Stop was enabled before the event ring was armed; deliver
             * the deferred bus-reset/connect-done handshake now so that
             * enumeration cannot be missed.
             */
            s->runstop_retry = false;
            DWC3_UDC_DBG(s, "deferred runstop handshake delivered "
                         "(ring size=%u)\n", s->event_size);
            dwc3_udc_push_event(s, DWC3_DEVT_USBRST);
            dwc3_udc_push_event(s, DWC3_DEVT_CONNECTDONE);
        }
        dwc3_udc_update_irq(s);
        break;
    case DWC3_GEVNTCOUNT: {
        uint32_t consumed = MIN(new_value & 0xffff, s->event_count);

        s->event_count -= consumed;
        dwc3_udc_update_irq(s);
        break;
    }
    case DWC3_DCTL:
        DWC3_UDC_DBG(s, "DCTL write 0x%08x (old 0x%08x)\n",
                     new_value, old_value);
        if (new_value & DWC3_DCTL_CSFTRST) {
            /*
             * Device controller soft reset: clears the register bank and all
             * endpoint/event ring state.  CSFTRST is self-clearing and is not
             * stored.
             */
            dwc3_udc_reset_state(s);
            break;
        }
        new_value &= ~DWC3_DCTL_CSFTRST;
        dwc3_udc_set_reg(s, addr, new_value);
        dwc3_udc_set_reg(s, DWC3_DSTS,
                        (new_value & DWC3_DCTL_RUNSTOP ? 0 :
                         DWC3_DSTS_DEVCTRLHLT) |
                        (s->host_connected ? 0 : DWC3_DSTS_USBLNKST_RX_DET));
        if (!(old_value & DWC3_DCTL_RUNSTOP) &&
            (new_value & DWC3_DCTL_RUNSTOP) && s->host_connected) {
            /*
             * Run/Stop 0->1 is the deterministic point at which the emulated
             * host starts a session: queue a bus reset and a connect-done so
             * the firmware stack performs its (asynchronous) reset and speed
             * negotiation.
             */
            DWC3_UDC_DBG(s, "runstop start (event ring addr=0x%" PRIx64
                         " size=%u)\n", s->event_addr, s->event_size);
            if (s->event_size < 2 * sizeof(uint32_t)) {
                /*
                 * The firmware enabled the controller before arming the event
                 * ring (unexpected, but a lost handshake would silently leave
                 * the console unenumerated forever): defer it until the ring
                 * is configured.
                 */
                s->runstop_retry = true;
            } else {
                dwc3_udc_push_event(s, DWC3_DEVT_USBRST);
                dwc3_udc_push_event(s, DWC3_DEVT_CONNECTDONE);
            }
        } else if ((old_value & DWC3_DCTL_RUNSTOP) &&
                   !(new_value & DWC3_DCTL_RUNSTOP)) {
            /* Controller stopped before a deferred handshake was delivered. */
            s->runstop_retry = false;
        }
        break;
    case DWC3_DGCMD:
        new_value &= ~(BIT(10) | (0xf << 12));
        switch (new_value & 0xff) {
        case 0x09:
            if (!(dwc3_udc_reg(s, DWC3_DGCMDPAR) & BIT(5))) {
                break;
            }
            /* fall through */
        case 0x0a:
            /* Device FIFO flush discards bytes not consumed by the host. */
            s->tx_head = s->tx_tail = s->tx_count = 0;
            timer_del(s->tx_timer);
            break;
        default:
            new_value |= BIT(12);
            break;
        }
        dwc3_udc_set_reg(s, addr, new_value);
        break;
    case DWC3_GCOREID:
    case DWC3_GHWPARAMS0 ... DWC3_GHWPARAMS7:
    case DWC3_DSTS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_DWC3_UDC ": write to read-only register at "
                      "0x%" HWADDR_PRIx "\n", addr);
        break;
    default:
        if (addr >= DWC3_DEPCMD(0) &&
            addr <= DWC3_DEPCMD(DWC3_MAX_ENDPOINTS - 1) &&
            (addr & 0xf) == 0xc) {
            unsigned int ep = (addr - DWC3_DEPCMD(0)) / 16;

            dwc3_udc_endpoint_command(s, ep, new_value);
        } else {
            dwc3_udc_set_reg(s, addr, new_value);
        }
        break;
    }
}

static const MemoryRegionOps dwc3_udc_ops = {
    .read = dwc3_udc_read,
    .write = dwc3_udc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static int dwc3_udc_can_receive(void *opaque)
{
    DWC3UDCState *s = opaque;

    return DWC3_INPUT_FIFO_SIZE - s->input_count;
}

static void dwc3_udc_receive(void *opaque, const uint8_t *buffer, int size)
{
    DWC3UDCState *s = opaque;
    int received = size;

    DWC3_UDC_DBG(s, "chardev input %d bytes (fifo count=%u)\n",
                 received, s->input_count);
    while (size-- > 0 && s->input_count < DWC3_INPUT_FIFO_SIZE) {
        s->input[s->input_tail] = *buffer++;
        s->input_tail = (s->input_tail + 1) % DWC3_INPUT_FIFO_SIZE;
        s->input_count++;
    }

    dwc3_udc_schedule_setup(s);
}

static void dwc3_udc_reset_state(DWC3UDCState *s)
{
    DWC3_UDC_DBG(s, "reset_state (runstop=%d stage=%u step=%u)\n",
                 !!(dwc3_udc_reg(s, DWC3_DCTL) & DWC3_DCTL_RUNSTOP),
                 s->control_stage, s->enumeration_step);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ep_type, 0, sizeof(s->ep_type));
    memset(s->ep_trb, 0, sizeof(s->ep_trb));
    memset(s->ep_pending, 0, sizeof(s->ep_pending));
    memset(s->ep_stalled, 0, sizeof(s->ep_stalled));

    /* Documented reset values of the modeled DWC3 IP. */
    dwc3_udc_set_reg(s, DWC3_GSBUSCFG0, DWC3_GSBUSCFG0_RESET);
    dwc3_udc_set_reg(s, DWC3_GCTL, DWC3_GCTL_RESET);
    dwc3_udc_set_reg(s, DWC3_GCOREID, DWC3_GCOREID_RESET);
    /* 64-bit AXI master, 32-bit APB slave, one event interrupt, HS UTMI. */
    dwc3_udc_set_reg(s, DWC3_GHWPARAMS0, 0x40204048);
    dwc3_udc_set_reg(s, DWC3_GHWPARAMS1, BIT(15));
    dwc3_udc_set_reg(s, DWC3_GHWPARAMS3,
                     (16 << 18) | (DWC3_MAX_ENDPOINTS << 12) | BIT(2));
    dwc3_udc_set_reg(s, DWC3_GHWPARAMS7, (8192 << 16) | 1024);
    dwc3_udc_set_reg(s, DWC3_GRXFIFOSIZ, 1024);
    for (unsigned i = 0; i < DWC3_MAX_ENDPOINTS / 2; i++) {
        dwc3_udc_set_reg(s, DWC3_GTXFIFOSIZ(i), (i * 512 << 16) | 512);
    }
    dwc3_udc_set_reg(s, DWC3_GUSB2PHYCFG, DWC3_GUSB2PHYCFG_RESET);
    dwc3_udc_set_reg(s, DWC3_GUSB3PIPECTL, DWC3_GUSB3PIPECTL_RESET);
    dwc3_udc_set_reg(s, DWC3_DSTS, DWC3_DSTS_RESET);

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
    dwc3_udc_tx_flush(s);
    timer_del(s->tx_timer);
    s->tx_head = 0;
    s->tx_tail = 0;
    s->tx_count = 0;
    s->runstop_retry = false;

    qemu_set_irq(s->irq, 0);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static void dwc3_udc_reset(DeviceState *dev)
{
    DWC3UDCState *s = DWC3_UDC(dev);

    dwc3_udc_reset_state(s);
}

static void dwc3_udc_realize(DeviceState *dev, Error **errp)
{
    DWC3UDCState *s = DWC3_UDC(dev);

    DWC3_UDC_DBG(s, "realize backend_connected=%d\n",
                 qemu_chr_fe_backend_connected(&s->chr));
    /*
     * Install handlers unconditionally: qemu_chr_fe_set_handlers() is a no-op
     * when no backend is associated, and with server=on,wait=off socket
     * backends the client may connect after realize.  The CHR_EVENT_OPENED
     * handler then flushes retained console output and resumes input.
     */
    qemu_chr_fe_set_handlers(&s->chr, dwc3_udc_can_receive,
                             dwc3_udc_receive, dwc3_udc_chr_event, NULL,
                             s, NULL, true);
}

static bool dwc3_udc_migration_valid(DWC3UDCState *s)
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

static int dwc3_udc_post_load(void *opaque, int version_id)
{
    DWC3UDCState *s = opaque;

    if (!dwc3_udc_migration_valid(s)) {
        return -EINVAL;
    }

    /* TX bytes are host-side transient data and are not migrated. */
    timer_del(s->tx_timer);
    s->tx_head = 0;
    s->tx_tail = 0;
    s->tx_count = 0;

    dwc3_udc_update_irq(s);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_accept_input(&s->chr);
    }
    return 0;
}

static const VMStateDescription vmstate_dwc3_udc = {
    .name = TYPE_DWC3_UDC,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = dwc3_udc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, DWC3UDCState, DWC3_REG_BANK_WORDS),
        VMSTATE_UINT8_ARRAY(ep_type, DWC3UDCState, DWC3_MAX_ENDPOINTS),
        VMSTATE_UINT64_ARRAY(ep_trb, DWC3UDCState, DWC3_MAX_ENDPOINTS),
        VMSTATE_BOOL_ARRAY(ep_pending, DWC3UDCState, DWC3_MAX_ENDPOINTS),
        VMSTATE_BOOL_ARRAY_V(ep_stalled, DWC3UDCState, DWC3_MAX_ENDPOINTS, 2),
        VMSTATE_UINT64(event_addr, DWC3UDCState),
        VMSTATE_UINT32(event_size, DWC3UDCState),
        VMSTATE_UINT32(event_count, DWC3UDCState),
        VMSTATE_UINT32(event_prod, DWC3UDCState),
        VMSTATE_UINT8_ARRAY(input, DWC3UDCState, DWC3_INPUT_FIFO_SIZE),
        VMSTATE_UINT16(input_head, DWC3UDCState),
        VMSTATE_UINT16(input_tail, DWC3UDCState),
        VMSTATE_UINT16(input_count, DWC3UDCState),
        VMSTATE_UINT8(input_sequence, DWC3UDCState),
        VMSTATE_UINT8(current_input, DWC3UDCState),
        VMSTATE_UINT8(enumeration_step, DWC3UDCState),
        VMSTATE_UINT32(control_stage, DWC3UDCState),
        VMSTATE_BOOL(control_input_request, DWC3UDCState),
        VMSTATE_TIMER_PTR(setup_timer, DWC3UDCState),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property dwc3_udc_properties[] = {
    DEFINE_PROP_CHR("chardev", DWC3UDCState, chr),
    DEFINE_PROP_BOOL("host-connected", DWC3UDCState, host_connected, true),
    /* Diagnostic tracing for reliability triage; off by default. */
    DEFINE_PROP_BOOL("debug", DWC3UDCState, debug, false),
};

static void dwc3_udc_init(Object *obj)
{
    DWC3UDCState *s = DWC3_UDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dwc3_udc_ops, s,
                          TYPE_DWC3_UDC, DWC3_UDC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->setup_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  dwc3_udc_setup_timer, s);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_REALTIME, dwc3_udc_tx_timer, s);
}

static void dwc3_udc_finalize(Object *obj)
{
    DWC3UDCState *s = DWC3_UDC(obj);

    timer_free(s->setup_timer);
    timer_free(s->tx_timer);
}

static void dwc3_udc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dwc3_udc_realize;
    dc->vmsd = &vmstate_dwc3_udc;
    dc->desc = "Synopsys DWC3 USB device controller (CDC ACM bridge)";
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, dwc3_udc_reset);
    device_class_set_props(dc, dwc3_udc_properties);
}

static const TypeInfo dwc3_udc_info = {
    .name = TYPE_DWC3_UDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWC3UDCState),
    .instance_init = dwc3_udc_init,
    .instance_finalize = dwc3_udc_finalize,
    .class_init = dwc3_udc_class_init,
};

static void dwc3_udc_register_types(void)
{
    type_register_static(&dwc3_udc_info);
}

type_init(dwc3_udc_register_types);
