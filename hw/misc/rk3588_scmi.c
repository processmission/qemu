/*
 * Minimal RK3588 SCMI clock responder.
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See include/hw/misc/rk3588_scmi.h for the message table.
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3588_scmi.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "exec/hwaddr.h"

/* shmem layout offsets (struct scmi_shared_mem, 24-byte fixed header). */
#define SHMEM_RESERVED          0x00
#define SHMEM_CHANNEL_STATUS    0x04
#define SHMEM_CHANNEL_FREE      BIT(0)
#define SHMEM_CHANNEL_ERROR     BIT(1)
#define SHMEM_RESERVED1         0x08
#define SHMEM_FLAGS             0x10
#define SHMEM_LENGTH            0x14
#define SHMEM_MSG_HEADER        0x18
#define SHMEM_MSG_PAYLOAD       0x1c

/* msg_header field masks (drivers/firmware/arm_scmi/common.h). */
#define MSG_ID_MASK             0xff
#define MSG_TYPE_MASK           0x300
#define MSG_XTYPE               8
#define MSG_PROTOCOL_ID_MASK    0x0003fc00
#define MSG_PROT_SHIFT          10
#define MSG_TOKEN_ID_MASK       0x0ffc0000
#define MSG_TOKEN_SHIFT         18

/* SCMI protocol ids. */
#define SCMI_PROTOCOL_BASE      0x10
#define SCMI_PROTOCOL_CLOCK     0x14
#define SCMI_PROTOCOL_RESET     0x16

/* SCMI message ids (common + clock). */
#define MSG_PROTOCOL_VERSION            0x0
#define MSG_PROTOCOL_ATTRIBUTES         0x1
#define MSG_PROTOCOL_MESSAGE_ATTRIBUTES 0x2
#define MSG_NEGOTIATE_PROTOCOL_VERSION  0x10

#define BASE_DISCOVER_VENDOR            0x3
#define BASE_DISCOVER_SUB_VENDOR        0x4
#define BASE_DISCOVER_IMPLEMENT_VERSION 0x5
#define BASE_DISCOVER_LIST_PROTOCOLS    0x6
#define BASE_DISCOVER_AGENT             0x7
#define BASE_NOTIFY_ERRORS              0x8

#define CLOCK_ATTRIBUTES        0x3
#define CLOCK_DESCRIBE_RATES    0x4
#define CLOCK_RATE_SET          0x5
#define CLOCK_RATE_GET          0x6
#define CLOCK_CONFIG_SET        0x7
#define CLOCK_CONFIG_GET        0xb

#define SCMI_BASE_VERSION       0x00020001
#define SCMI_CLOCK_VERSION      0x00030000
#define SCMI_RESET_VERSION      0x00030001
#define SCMI_NUM_PROTOCOLS      2

/* SCMI generic status codes. */
#define SCMI_SUCCESS            0
#define SCMI_ERR_SUPPORT        (-1)
#define SCMI_ERR_INVALID_PARAMS (-2)
#define SCMI_ERR_NOT_FOUND      (-4)

/* IDs in include/dt-bindings/clock/rockchip,rk3588-cru.h. */
#define SCMI_CCLK_SD    9
#define SCMI_HCLK_SD    23

/* Nominal rates (Hz) - picked to be plausible so CCF accepts them. */
#define SCMI_CCLK_SD_RATE      50000000ULL
#define SCMI_HCLK_SD_RATE      100000000ULL

static uint32_t shmem_get_status_off(void)
{
    /* First 4 bytes of the payload are the status. */
    return SHMEM_MSG_PAYLOAD;
}

static void shmem_write_u32(RK3588SCMIState *s, unsigned off, uint32_t v)
{
    stl_le_p(s->shmem_buf + off, v);
}

static uint32_t shmem_read_u32(RK3588SCMIState *s, unsigned off)
{
    return ldl_le_p(s->shmem_buf + off);
}

static void shmem_write_u64(RK3588SCMIState *s, unsigned off, uint64_t v)
{
    stq_le_p(s->shmem_buf + off, v);
}

/*
 * Resolve the per-clock nominal rate. Only IDs 9 and 23 are
 * meaningful for the SD-card driver; for everything else we return
 * a benign 24 MHz so CCF's clk-scmi does not explode during the
 * 24-clock enumeration loop in scmi_clock_protocol_init.
 */
static uint64_t rk3588_scmi_clock_rate(RK3588SCMIState *s, uint32_t clk_id)
{
    if (clk_id >= RK3588_SCMI_NUM_CLOCKS) {
        return 0;
    }
    if (s->rate[clk_id]) {
        return s->rate[clk_id];
    }
    switch (clk_id) {
    case SCMI_CCLK_SD:
        return SCMI_CCLK_SD_RATE;
    case SCMI_HCLK_SD:
        return SCMI_HCLK_SD_RATE;
    default:
        return 24000000ULL;
    }
}

static bool rk3588_scmi_base_msg_supported(uint32_t msg_id)
{
    switch (msg_id) {
    case MSG_PROTOCOL_VERSION:
    case MSG_PROTOCOL_ATTRIBUTES:
    case MSG_PROTOCOL_MESSAGE_ATTRIBUTES:
    case BASE_DISCOVER_VENDOR:
    case BASE_DISCOVER_SUB_VENDOR:
    case BASE_DISCOVER_IMPLEMENT_VERSION:
    case BASE_DISCOVER_LIST_PROTOCOLS:
    case BASE_DISCOVER_AGENT:
    case BASE_NOTIFY_ERRORS:
    case MSG_NEGOTIATE_PROTOCOL_VERSION:
        return true;
    default:
        return false;
    }
}

static bool rk3588_scmi_clock_msg_supported(uint32_t msg_id)
{
    switch (msg_id) {
    case MSG_PROTOCOL_VERSION:
    case MSG_PROTOCOL_ATTRIBUTES:
    case MSG_PROTOCOL_MESSAGE_ATTRIBUTES:
    case CLOCK_ATTRIBUTES:
    case CLOCK_RATE_SET:
    case CLOCK_RATE_GET:
    case CLOCK_CONFIG_SET:
    case CLOCK_CONFIG_GET:
    case MSG_NEGOTIATE_PROTOCOL_VERSION:
        return true;
    default:
        return false;
    }
}

static bool rk3588_scmi_reset_msg_supported(uint32_t msg_id)
{
    switch (msg_id) {
    case MSG_PROTOCOL_VERSION:
    case MSG_PROTOCOL_ATTRIBUTES:
    case MSG_PROTOCOL_MESSAGE_ATTRIBUTES:
    case MSG_NEGOTIATE_PROTOCOL_VERSION:
        return true;
    default:
        return false;
    }
}

/*
 * Write the response into the shmem buffer. Caller has already placed
 * the status word at SHMEM_MSG_PAYLOAD. We just fix up the header
 * echo, length, and CHANNEL_FREE.
 */
static void rk3588_scmi_finalize_response(RK3588SCMIState *s,
                                          uint32_t payload_len)
{
    uint32_t length = 4 /* header */ + 4 /* status */ + payload_len;

    shmem_write_u32(s, SHMEM_LENGTH, length);
    shmem_write_u32(s, SHMEM_CHANNEL_STATUS, SHMEM_CHANNEL_FREE);
}

/*
 * Read the request from shmem, write a canned response, set
 * CHANNEL_FREE. Returns true on any well-formed message (even ones
 * we don't support, which still get a NOT_SUPPORTED response); false
 * if the shmem does not look like a valid request.
 */
bool rk3588_scmi_handle_smc(RK3588SCMIState *s)
{
    uint32_t hdr, length, msg_id, msg_type, prot_id;
    uint32_t status_off;
    uint32_t status = SCMI_SUCCESS;
    uint32_t extra_payload = 0;

    length = shmem_read_u32(s, SHMEM_LENGTH);
    hdr = shmem_read_u32(s, SHMEM_MSG_HEADER);

    if (length < 4) {
        /* No header to parse - leave shmem alone; transport will see the
         * channel still busy and time out. Be lenient: still set
         * CHANNEL_FREE so the guest can recover. */
        shmem_write_u32(s, SHMEM_CHANNEL_STATUS, SHMEM_CHANNEL_FREE);
        return false;
    }

    msg_id = hdr & MSG_ID_MASK;
    msg_type = (hdr & MSG_TYPE_MASK) >> MSG_XTYPE;
    prot_id = (hdr & MSG_PROTOCOL_ID_MASK) >> MSG_PROT_SHIFT;
    (void)msg_type; /* only COMMAND (0) appears in practice */

    status_off = shmem_get_status_off();

    switch (prot_id) {
    case SCMI_PROTOCOL_BASE:
        switch (msg_id) {
        case MSG_PROTOCOL_VERSION:
            shmem_write_u32(s, status_off + 4, SCMI_BASE_VERSION);
            extra_payload = 4;
            break;
        case MSG_PROTOCOL_ATTRIBUTES:
            /* struct scmi_msg_resp_base_attributes: {num_protocols, agents}. */
            shmem_write_u32(s, status_off + 4,
                            SCMI_NUM_PROTOCOLS | (1 << 8));
            extra_payload = 4;
            break;
        case MSG_PROTOCOL_MESSAGE_ATTRIBUTES:
            if (rk3588_scmi_base_msg_supported(
                    shmem_read_u32(s, SHMEM_MSG_PAYLOAD))) {
                shmem_write_u32(s, status_off + 4, 0);
                extra_payload = 4;
            } else {
                status = SCMI_ERR_SUPPORT;
                extra_payload = 0;
            }
            break;
        case BASE_DISCOVER_VENDOR:
            memset(s->shmem_buf + status_off + 4, 0, 16);
            strcpy((char *)(s->shmem_buf + status_off + 4), "QEMU");
            extra_payload = 16;
            break;
        case BASE_DISCOVER_SUB_VENDOR:
            memset(s->shmem_buf + status_off + 4, 0, 16);
            strcpy((char *)(s->shmem_buf + status_off + 4), "RK3588");
            extra_payload = 16;
            break;
        case BASE_DISCOVER_IMPLEMENT_VERSION:
            shmem_write_u32(s, status_off + 4, 0);
            extra_payload = 4;
            break;
        case BASE_DISCOVER_LIST_PROTOCOLS: {
            uint32_t skip = shmem_read_u32(s, SHMEM_MSG_PAYLOAD);

            if (skip >= SCMI_NUM_PROTOCOLS) {
                shmem_write_u32(s, status_off + 4, 0);
                extra_payload = 4;
            } else {
                uint32_t count = SCMI_NUM_PROTOCOLS - skip;
                uint32_t protocols = 0;
                const uint8_t protocol_list[SCMI_NUM_PROTOCOLS] = {
                    SCMI_PROTOCOL_CLOCK,
                    SCMI_PROTOCOL_RESET,
                };

                for (uint32_t i = 0; i < count; i++) {
                    protocols |= protocol_list[skip + i] << (i * 8);
                }
                shmem_write_u32(s, status_off + 4, count);
                shmem_write_u32(s, status_off + 8, protocols);
                extra_payload = 8;
            }
            break;
        }
        case BASE_DISCOVER_AGENT:
            memset(s->shmem_buf + status_off + 4, 0, 16);
            strcpy((char *)(s->shmem_buf + status_off + 4), "agent0");
            extra_payload = 16;
            break;
        case BASE_NOTIFY_ERRORS:
            extra_payload = 0;
            break;
        case MSG_NEGOTIATE_PROTOCOL_VERSION:
            /* No payload; only status. */
            extra_payload = 0;
            break;
        default:
            status = SCMI_ERR_SUPPORT;
            extra_payload = 0;
            break;
        }
        break;

    case SCMI_PROTOCOL_CLOCK:
        switch (msg_id) {
        case MSG_PROTOCOL_VERSION:
            shmem_write_u32(s, status_off + 4, SCMI_CLOCK_VERSION);
            extra_payload = 4;
            break;
        case MSG_PROTOCOL_ATTRIBUTES: {
            /* num_clocks[15:0]=24, max_async_req[23:16]=0 */
            uint32_t attrs = RK3588_SCMI_NUM_CLOCKS;
            shmem_write_u32(s, status_off + 4, attrs);
            extra_payload = 4;
            break;
        }
        case MSG_PROTOCOL_MESSAGE_ATTRIBUTES:
            if (rk3588_scmi_clock_msg_supported(
                    shmem_read_u32(s, SHMEM_MSG_PAYLOAD))) {
                shmem_write_u32(s, status_off + 4, 0);
                extra_payload = 4;
            } else {
                status = SCMI_ERR_SUPPORT;
                extra_payload = 0;
            }
            break;
        case CLOCK_ATTRIBUTES: {
            uint32_t clk_id = shmem_read_u32(s, SHMEM_MSG_PAYLOAD);
            if (clk_id >= RK3588_SCMI_NUM_CLOCKS) {
                status = SCMI_ERR_NOT_FOUND;
                extra_payload = 0;
                break;
            }
            /*
             * attributes=0: no extended-name, no notifications, no parents.
             * This makes Linux skip CLOCK_NAME_GET / DESCRIBE_RATES /
             * POSSIBLE_PARENTS for this clock (clock.c:382-402).
             */
            shmem_write_u32(s, status_off + 4, 0);
            /* 16-byte ASCII name (NUL-padded). */
            memset(s->shmem_buf + status_off + 8, 0, 16);
            switch (clk_id) {
            case SCMI_CCLK_SD:
                strcpy((char *)(s->shmem_buf + status_off + 8), "cclk_sd");
                break;
            case SCMI_HCLK_SD:
                strcpy((char *)(s->shmem_buf + status_off + 8), "hclk_sd");
                break;
            default: {
                char nm[8];
                snprintf(nm, sizeof(nm), "clk%u", clk_id);
                strcpy((char *)(s->shmem_buf + status_off + 8), nm);
                break;
            }
            }
            /* clock_enable_latency=0. */
            shmem_write_u32(s, status_off + 24, 0);
            extra_payload = 4 + 16 + 4;
            break;
        }
        case CLOCK_RATE_GET: {
            uint32_t clk_id = shmem_read_u32(s, SHMEM_MSG_PAYLOAD);
            if (clk_id >= RK3588_SCMI_NUM_CLOCKS) {
                status = SCMI_ERR_NOT_FOUND;
                extra_payload = 0;
                break;
            }
            shmem_write_u64(s, status_off + 4,
                            rk3588_scmi_clock_rate(s, clk_id));
            extra_payload = 8;
            break;
        }
        case CLOCK_RATE_SET: {
            /* payload: {u32 flags; u32 clk_id; u32 rate_lo; u32 rate_hi} */
            uint32_t clk_id = shmem_read_u32(s, SHMEM_MSG_PAYLOAD + 4);
            uint32_t rate_lo = shmem_read_u32(s, SHMEM_MSG_PAYLOAD + 8);
            uint32_t rate_hi = shmem_read_u32(s, SHMEM_MSG_PAYLOAD + 12);
            if (clk_id >= RK3588_SCMI_NUM_CLOCKS) {
                status = SCMI_ERR_NOT_FOUND;
            } else {
                s->rate[clk_id] = ((uint64_t)rate_hi << 32) | rate_lo;
            }
            extra_payload = 0;
            break;
        }
        case CLOCK_CONFIG_SET: {
            /* payload: {u32 clk_id; u32 attributes} */
            uint32_t clk_id = shmem_read_u32(s, SHMEM_MSG_PAYLOAD);
            uint32_t attrs = shmem_read_u32(s, SHMEM_MSG_PAYLOAD + 4);
            if (clk_id >= RK3588_SCMI_NUM_CLOCKS) {
                status = SCMI_ERR_NOT_FOUND;
            } else {
                s->enabled[clk_id] = (attrs & 0x1) != 0;
            }
            extra_payload = 0;
            break;
        }
        case CLOCK_CONFIG_GET: {
            /* payload: {u32 clk_id; u32 flags}; v3 response has 3 u32s. */
            uint32_t clk_id = shmem_read_u32(s, SHMEM_MSG_PAYLOAD);
            if (clk_id >= RK3588_SCMI_NUM_CLOCKS) {
                status = SCMI_ERR_NOT_FOUND;
                extra_payload = 0;
                break;
            }
            shmem_write_u32(s, status_off + 4, 0);
            shmem_write_u32(s, status_off + 8, s->enabled[clk_id] ? 1 : 0);
            shmem_write_u32(s, status_off + 12, 0);
            extra_payload = 12;
            break;
        }
        case MSG_NEGOTIATE_PROTOCOL_VERSION:
            extra_payload = 0;
            break;
        default:
            status = SCMI_ERR_SUPPORT;
            extra_payload = 0;
            break;
        }
        break;

    case SCMI_PROTOCOL_RESET:
        switch (msg_id) {
        case MSG_PROTOCOL_VERSION:
            shmem_write_u32(s, status_off + 4, SCMI_RESET_VERSION);
            extra_payload = 4;
            break;
        case MSG_PROTOCOL_ATTRIBUTES:
            /* num_reset_domains[15:0]=0. */
            shmem_write_u32(s, status_off + 4, 0);
            extra_payload = 4;
            break;
        case MSG_PROTOCOL_MESSAGE_ATTRIBUTES:
            if (rk3588_scmi_reset_msg_supported(
                    shmem_read_u32(s, SHMEM_MSG_PAYLOAD))) {
                shmem_write_u32(s, status_off + 4, 0);
                extra_payload = 4;
            } else {
                status = SCMI_ERR_SUPPORT;
                extra_payload = 0;
            }
            break;
        case MSG_NEGOTIATE_PROTOCOL_VERSION:
            extra_payload = 0;
            break;
        default:
            status = SCMI_ERR_SUPPORT;
            extra_payload = 0;
            break;
        }
        break;

    default:
        /* Unknown protocol - let the framework see NOT_SUPPORTED. */
        status = SCMI_ERR_SUPPORT;
        extra_payload = 0;
        break;
    }

    shmem_write_u32(s, status_off, (uint32_t)status);
    rk3588_scmi_finalize_response(s, extra_payload);
    return true;
}

/* ------------------------------------------------------------------------- */
/* SysBusDevice plumbing: a tiny RAM-backed shmem MMIO region.                */
/* ------------------------------------------------------------------------- */

static uint64_t rk3588_scmi_shmem_read(void *opaque, hwaddr off, unsigned sz)
{
    RK3588SCMIState *s = RK3588_SCMI(opaque);
    uint64_t v = 0;

    if (off + sz <= RK3588_SCMI_SHMEM_SIZE) {
        memcpy(&v, s->shmem_buf + off, sz);
    }
    return v;
}

static void rk3588_scmi_shmem_write(void *opaque, hwaddr off, uint64_t v,
                                    unsigned sz)
{
    RK3588SCMIState *s = RK3588_SCMI(opaque);

    if (off + sz <= RK3588_SCMI_SHMEM_SIZE) {
        memcpy(s->shmem_buf + off, &v, sz);
    }
}

static const MemoryRegionOps rk3588_scmi_shmem_ops = {
    .read = rk3588_scmi_shmem_read,
    .write = rk3588_scmi_shmem_write,
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

static void rk3588_scmi_reset(DeviceState *dev)
{
    RK3588SCMIState *s = RK3588_SCMI(dev);

    memset(s->shmem_buf, 0, sizeof(s->shmem_buf));
    /* CHANNEL_FREE=1 at reset so the first tx_prepare can proceed. */
    shmem_write_u32(s, SHMEM_CHANNEL_STATUS, SHMEM_CHANNEL_FREE);
    memset(s->rate, 0, sizeof(s->rate));
    memset(s->enabled, 0, sizeof(s->enabled));
}

static void rk3588_scmi_realize(DeviceState *dev, Error **errp)
{
    RK3588SCMIState *s = RK3588_SCMI(dev);

    memory_region_init_io(&s->shmem, OBJECT(s), &rk3588_scmi_shmem_ops, s,
                          "rk3588-scmi-shmem", RK3588_SCMI_SHMEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->shmem);
}

static void rk3588_scmi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3588_scmi_realize;
    device_class_set_legacy_reset(dc, rk3588_scmi_reset);
    dc->user_creatable = false;
}

static const TypeInfo rk3588_scmi_info = {
    .name = TYPE_RK3588_SCMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588SCMIState),
    .class_init = rk3588_scmi_class_init,
};

static void rk3588_scmi_register_types(void)
{
    type_register_static(&rk3588_scmi_info);
}

type_init(rk3588_scmi_register_types)
