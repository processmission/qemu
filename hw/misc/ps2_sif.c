/*
 * Sony PlayStation 2 subsystem interface (SIF) and IOP high-level emulation
 *
 * The SIF connects the Emotion Engine with the I/O processor (IOP).  The
 * EE sends command packets to the IOP over DMAC channel SIF1, and the IOP
 * answers over SIF0.  The IOP is not emulated: this device stands in for
 * its SIF command and RPC services, the way PCSX2 does high-level
 * emulation of IOP modules.  It provides:
 *
 *  - the reset and SIF initialisation handshake,
 *  - the heap server, which allocates IOP RAM,
 *  - the module loader server, which accepts IRX modules, and reads and
 *    writes IOP memory for the EE,
 *  - the interrupt relay server of the irqrelay module, which forwards IOP
 *    interrupts to the EE.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/misc/ps2_sif.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "trace.h"

enum {
    PS2_SIF_CMD_WRITE_SREG = 0x80000001,
    PS2_SIF_CMD_INIT_CMD = 0x80000002,
    PS2_SIF_CMD_RESET_CMD = 0x80000003,
    PS2_SIF_CMD_RPC_END = 0x80000008,
    PS2_SIF_CMD_RPC_BIND = 0x80000009,
    PS2_SIF_CMD_RPC_CALL = 0x8000000a,
    PS2_SIF_CMD_ATA = 0x80000013,
    PS2_SIF_CMD_IRQ_RELAY = 0x80000020,
};

enum {
    PS2_SIF_SID_HEAP = 0x80000003,
    PS2_SIF_SID_LOAD_MODULE = 0x80000006,
    PS2_SIF_SID_IRQ_RELAY = 0x80000020,
};

/* IOP modules with an emulated function. */
typedef enum PS2SIFModule {
    PS2_SIF_MODULE_IRQRELAY = 1 << 0,
    PS2_SIF_MODULE_DEV9 = 1 << 1,
    PS2_SIF_MODULE_ATA = 1 << 2,
} PS2SIFModule;

/* The operation of the ATA command is in bits 0-2 of its option. */
#define PS2_SIF_ATA_OP_MASK     7
#define PS2_SIF_ATA_BOUNCE_BUFFER 0

/* IOP addresses of the emulated IOP side. */
#define PS2_SIF_IOP_CMD_BUFFER  0x00010000
#define PS2_SIF_IOP_SERVER      0x00010100
#define PS2_SIF_IOP_SERVER_BUF  0x00011000
#define PS2_SIF_SERVER_BUF_SIZE 0x1000
#define PS2_SIF_HEAP_BASE       0x00020000
#define PS2_SIF_HEAP_END        0x001f0000
#define PS2_SIF_HEAP_ALIGN      64

/* The IOP needs a moment to reboot. */
#define PS2_SIF_RESET_DELAY_NS  (1 * SCALE_MS)

#define PS2_SIF_CMD_HEADER_SIZE 16
#define PS2_SIF_IOP_DMA_TAG_ERT (1u << 31)
#define PS2_SIF_IOP_DMA_TAG_INT (1u << 30)

/* Offsets of RPC packet fields, after the 12-byte RPC packet header. */
#define PS2_SIF_RPC_CLIENT      12
#define PS2_SIF_BIND_SERVER_ID  16
#define PS2_SIF_CALL_RPC_ID     16
#define PS2_SIF_CALL_SEND_SIZE  20
#define PS2_SIF_CALL_RECV_ADDR  24
#define PS2_SIF_CALL_RECV_SIZE  28
#define PS2_SIF_CALL_SERVER     36
#define PS2_SIF_CALL_SIZE       40
#define PS2_SIF_END_CLIENT_ID   16
#define PS2_SIF_END_SERVER      20
#define PS2_SIF_END_SERVER_BUF  24
#define PS2_SIF_END_SIZE        32

/* IOP kernel error numbers. */
#define PS2_IOP_KE_ERROR        (-1)
#define PS2_IOP_KE_NO_MEMORY    (-400)
#define PS2_IOP_KE_ILLEGAL_MEM  (-401)

/* The send data is zero padded to the size of the server buffer. */
typedef int32_t (*PS2SIFRPCFn)(PS2SIFState *s, uint32_t rpc_id,
                               const uint8_t *send, uint32_t send_size,
                               uint8_t *recv, uint32_t recv_size);

typedef struct PS2SIFServer {
    uint32_t sid;
    /* The server exists once these modules have been linked. */
    uint32_t modules;
    PS2SIFRPCFn call;
} PS2SIFServer;

static uint32_t ps2_sif_server_addr(int i)
{
    return PS2_SIF_IOP_SERVER + i * 0x40;
}

static uint32_t ps2_sif_server_buffer(int i)
{
    return PS2_SIF_IOP_SERVER_BUF + i * PS2_SIF_SERVER_BUF_SIZE;
}

static void ps2_sif_send(PS2SIFState *s, uint32_t cmd, uint32_t opt,
                         const void *payload, uint32_t len)
{
    uint8_t *pkt;

    assert(len <= PS2_SIF_CMD_PACKET_MAX - PS2_SIF_CMD_HEADER_SIZE);
    if (s->queue_len == PS2_SIF_QUEUE_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: SIF0 queue overflow, command "
                      "0x%08x dropped\n", __func__, cmd);
        return;
    }

    pkt = s->queue[(s->queue_head + s->queue_len) % PS2_SIF_QUEUE_LEN];
    memset(pkt, 0, PS2_SIF_CMD_PACKET_MAX);
    /* packet_size in bits 0-7, no data_size and data_addr. */
    stl_le_p(pkt, PS2_SIF_CMD_HEADER_SIZE + len);
    stl_le_p(pkt + 8, cmd);
    stl_le_p(pkt + 12, opt);
    memcpy(pkt + PS2_SIF_CMD_HEADER_SIZE, payload, len);
    s->queue_len++;
    trace_ps2_sif_send(cmd, opt, len);

    qemu_bh_schedule(s->sif0_bh);
}

/* Deliver one queued packet each time the EE starts SIF0. */
static void ps2_sif0_deliver(void *opaque)
{
    PS2SIFState *s = opaque;
    uint8_t *pkt;

    if (!s->queue_len || !ps2_dmac_channel_active(s->dmac, PS2_DMAC_SIF0)) {
        return;
    }

    pkt = s->queue[s->queue_head];
    if (dma_memory_write(&address_space_memory, s->sif0_addr, pkt, pkt[0],
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad SIF0 buffer 0x%08x\n",
                      __func__, s->sif0_addr);
    }
    s->queue_head = (s->queue_head + 1) % PS2_SIF_QUEUE_LEN;
    s->queue_len--;
    ps2_dmac_channel_done(s->dmac, PS2_DMAC_SIF0);
}

static void ps2_sif0_start(void *opaque)
{
    PS2SIFState *s = opaque;

    qemu_bh_schedule(s->sif0_bh);
}

static uint32_t ps2_sif_heap_alloc(PS2SIFState *s, uint32_t size)
{
    uint32_t addr = PS2_SIF_HEAP_BASE;
    uint32_t i;

    if (!size || size > PS2_SIF_HEAP_END - PS2_SIF_HEAP_BASE ||
        s->heap_count == PS2_SIF_HEAP_MAX_BLOCKS) {
        return 0;
    }
    size = ROUND_UP(size, PS2_SIF_HEAP_ALIGN);

    /* First fit in the gaps between the blocks. */
    for (i = 0; i < s->heap_count; i++) {
        if (s->heap_addr[i] - addr >= size) {
            break;
        }
        addr = s->heap_addr[i] + s->heap_size[i];
    }
    if (PS2_SIF_HEAP_END - addr < size) {
        return 0;
    }

    memmove(&s->heap_addr[i + 1], &s->heap_addr[i],
            (s->heap_count - i) * sizeof(s->heap_addr[0]));
    memmove(&s->heap_size[i + 1], &s->heap_size[i],
            (s->heap_count - i) * sizeof(s->heap_size[0]));
    s->heap_addr[i] = addr;
    s->heap_size[i] = size;
    s->heap_count++;
    return addr;
}

static int32_t ps2_sif_heap_free(PS2SIFState *s, uint32_t addr)
{
    for (uint32_t i = 0; i < s->heap_count; i++) {
        if (s->heap_addr[i] == addr) {
            s->heap_count--;
            memmove(&s->heap_addr[i], &s->heap_addr[i + 1],
                    (s->heap_count - i) * sizeof(s->heap_addr[0]));
            memmove(&s->heap_size[i], &s->heap_size[i + 1],
                    (s->heap_count - i) * sizeof(s->heap_size[0]));
            return 0;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%08x is not allocated\n",
                  __func__, addr);
    return PS2_IOP_KE_ERROR;
}

static int32_t ps2_sif_heap_call(PS2SIFState *s, uint32_t rpc_id,
                                 const uint8_t *send, uint32_t send_size,
                                 uint8_t *recv, uint32_t recv_size)
{
    uint32_t arg = send_size >= 4 ? ldl_le_p(send) : 0;
    uint32_t ret;

    switch (rpc_id) {
    case 1:
        ret = ps2_sif_heap_alloc(s, arg);
        trace_ps2_sif_heap_alloc(arg, ret);
        break;
    case 2:
        ret = ps2_sif_heap_free(s, arg);
        trace_ps2_sif_heap_free(arg);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented heap call %u\n",
                      __func__, rpc_id);
        ret = PS2_IOP_KE_ERROR;
        break;
    }
    if (recv_size >= 4) {
        stl_le_p(recv, ret);
    }
    return 0;
}

static bool ps2_sif_iop_read(PS2SIFState *s, uint32_t addr, void *buf,
                             uint32_t len)
{
    return address_space_read(&s->iop_as, addr, MEMTXATTRS_UNSPECIFIED,
                              buf, len) == MEMTX_OK;
}

/* Find the module name in the .iopmod section of an IRX in IOP memory. */
static bool ps2_sif_irx_name(PS2SIFState *s, uint32_t addr, char *name,
                             size_t size)
{
    uint8_t ehdr[52], shdr[40];
    uint32_t shoff;
    uint16_t shentsize, shnum;

    if (!ps2_sif_iop_read(s, addr, ehdr, sizeof(ehdr)) ||
        memcmp(ehdr, "\177ELF", 4)) {
        return false;
    }
    shoff = ldl_le_p(ehdr + 0x20);
    shentsize = lduw_le_p(ehdr + 0x2e);
    shnum = lduw_le_p(ehdr + 0x30);
    if (shentsize < sizeof(shdr)) {
        return false;
    }

    for (unsigned i = 0; i < shnum; i++) {
        if (!ps2_sif_iop_read(s, addr + shoff + i * shentsize, shdr,
                              sizeof(shdr))) {
            return false;
        }
        /* SHT_LOPROC + 0x80 */
        if (ldl_le_p(shdr + 4) == 0x70000080) {
            /* The name follows six words and the 16-bit version. */
            if (!ps2_sif_iop_read(s, addr + ldl_le_p(shdr + 16) + 26,
                                  name, size - 1)) {
                return false;
            }
            name[size - 1] = '\0';
            return true;
        }
    }
    return false;
}

static int32_t ps2_sif_link_module(PS2SIFState *s, uint32_t addr)
{
    static const struct {
        const char *name;
        PS2SIFModule module;
    } modules[] = {
        { "irqrelay", PS2_SIF_MODULE_IRQRELAY },
        { "dev9", PS2_SIF_MODULE_DEV9 },
        { "ata", PS2_SIF_MODULE_ATA },
    };
    char name[64];

    if (!ps2_sif_irx_name(s, addr & (PS2_IOP_RAM_SIZE - 1), name,
                          sizeof(name))) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no IRX module at 0x%08x\n",
                      __func__, addr);
        return PS2_IOP_KE_ILLEGAL_MEM;
    }

    trace_ps2_sif_link_module(name);
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (!strcmp(name, modules[i].name)) {
            s->modules |= modules[i].module;
        }
    }
    /* Other modules are linked, but have no effect. */
    return ++s->module_count;
}

static int32_t ps2_sif_iop_access(PS2SIFState *s, bool write, uint32_t addr,
                                  uint32_t type, uint32_t *value)
{
    static const unsigned sizes[] = { 1, 2, 4 };
    MemTxResult res;

    if (type >= ARRAY_SIZE(sizes)) {
        return PS2_IOP_KE_ERROR;
    }
    /* The EE gives KSEG addresses. */
    addr &= 0x1fffffff;
    if (write) {
        res = address_space_write(&s->iop_as, addr, MEMTXATTRS_UNSPECIFIED,
                                  value, sizes[type]);
    } else {
        *value = 0;
        res = address_space_read(&s->iop_as, addr, MEMTXATTRS_UNSPECIFIED,
                                 value, sizes[type]);
    }
    *value = le32_to_cpu(*value);
    trace_ps2_sif_iop_access(write, addr, sizes[type], *value);
    return res == MEMTX_OK ? 0 : PS2_IOP_KE_ILLEGAL_MEM;
}

static int32_t ps2_sif_load_module_call(PS2SIFState *s, uint32_t rpc_id,
                                        const uint8_t *send,
                                        uint32_t send_size,
                                        uint8_t *recv, uint32_t recv_size)
{
    uint8_t ret[8] = { 0 };
    uint32_t value;

    switch (rpc_id) {
    case 0:
        /* Load a ROM module.  The ROM modules are always there. */
        stl_le_p(ret, ++s->module_count);
        break;
    case 2:
        value = cpu_to_le32(ldl_le_p(send + 8));
        stl_le_p(ret, ps2_sif_iop_access(s, true, ldl_le_p(send),
                                         ldl_le_p(send + 4), &value));
        break;
    case 3:
        ps2_sif_iop_access(s, false, ldl_le_p(send), ldl_le_p(send + 4),
                           &value);
        stl_le_p(ret, value);
        break;
    case 6:
        stl_le_p(ret, ps2_sif_link_module(s, ldl_le_p(send)));
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented module call %u\n",
                      __func__, rpc_id);
        stl_le_p(ret, PS2_IOP_KE_ERROR);
        break;
    }
    memcpy(recv, ret, MIN(recv_size, sizeof(ret)));
    return 0;
}

static int32_t ps2_sif_irq_relay_call(PS2SIFState *s, uint32_t rpc_id,
                                      const uint8_t *send, uint32_t send_size,
                                      uint8_t *recv, uint32_t recv_size)
{
    uint8_t iop = send_size ? send[0] : PS2_SIF_NUM_IOP_IRQS;
    int32_t ret = 0;

    if (iop >= PS2_SIF_NUM_IOP_IRQS) {
        ret = PS2_IOP_KE_ERROR;
    } else {
        switch (rpc_id) {
        case 1:
        case 3:
            if (send_size < 3 || !send[1]) {
                ret = PS2_IOP_KE_ERROR;
            } else if (!send[2]) {
                qemu_log_mask(LOG_UNIMP, "%s: SMFLAG relays are not "
                              "implemented\n", __func__);
                ret = PS2_IOP_KE_ERROR;
            } else {
                s->relay[iop] = send[1];
            }
            break;
        case 2:
            s->relay[iop] = 0;
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented relay call %u\n",
                          __func__, rpc_id);
            ret = PS2_IOP_KE_ERROR;
            break;
        }
        trace_ps2_sif_irq_relay(rpc_id, iop, s->relay[iop]);
    }
    if (recv_size >= 4) {
        stl_le_p(recv, ret);
    }
    return 0;
}

static const PS2SIFServer ps2_sif_servers[] = {
    { PS2_SIF_SID_HEAP, 0, ps2_sif_heap_call },
    { PS2_SIF_SID_LOAD_MODULE, 0, ps2_sif_load_module_call },
    { PS2_SIF_SID_IRQ_RELAY, PS2_SIF_MODULE_IRQRELAY,
      ps2_sif_irq_relay_call },
};

static void ps2_sif_rpc_end(PS2SIFState *s, uint32_t client,
                            uint32_t client_id, uint32_t server,
                            uint32_t server_buffer)
{
    uint8_t end[PS2_SIF_END_SIZE] = { 0 };

    stl_le_p(end + PS2_SIF_RPC_CLIENT, client);
    stl_le_p(end + PS2_SIF_END_CLIENT_ID, client_id);
    stl_le_p(end + PS2_SIF_END_SERVER, server);
    stl_le_p(end + PS2_SIF_END_SERVER_BUF, server_buffer);
    ps2_sif_send(s, PS2_SIF_CMD_RPC_END, 0, end, sizeof(end));
}

static void ps2_sif_rpc_bind(PS2SIFState *s, const uint8_t *pkt,
                             uint32_t len)
{
    uint32_t client, sid;

    if (len < PS2_SIF_BIND_SERVER_ID + 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: short packet\n", __func__);
        return;
    }
    client = ldl_le_p(pkt + PS2_SIF_RPC_CLIENT);
    sid = ldl_le_p(pkt + PS2_SIF_BIND_SERVER_ID);

    for (int i = 0; i < ARRAY_SIZE(ps2_sif_servers); i++) {
        const PS2SIFServer *server = &ps2_sif_servers[i];

        if (server->sid == sid &&
            (s->modules & server->modules) == server->modules) {
            trace_ps2_sif_rpc_bind(sid, ps2_sif_server_addr(i));
            ps2_sif_rpc_end(s, client, PS2_SIF_CMD_RPC_BIND,
                            ps2_sif_server_addr(i), ps2_sif_server_buffer(i));
            return;
        }
    }

    /* A zero server tells the EE that there is no such server. */
    qemu_log_mask(LOG_UNIMP, "%s: no RPC server 0x%08x\n", __func__, sid);
    trace_ps2_sif_rpc_bind(sid, 0);
    ps2_sif_rpc_end(s, client, PS2_SIF_CMD_RPC_BIND, 0, 0);
}

static void ps2_sif_rpc_call(PS2SIFState *s, const uint8_t *pkt,
                             uint32_t len)
{
    g_autofree uint8_t *send = NULL;
    g_autofree uint8_t *recv = NULL;
    uint32_t client, rpc_id, send_size, recv_addr, recv_size, addr;
    const PS2SIFServer *server = NULL;
    int i;

    if (len < PS2_SIF_CALL_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: short packet\n", __func__);
        return;
    }
    client = ldl_le_p(pkt + PS2_SIF_RPC_CLIENT);
    rpc_id = ldl_le_p(pkt + PS2_SIF_CALL_RPC_ID);
    send_size = MIN(ldl_le_p(pkt + PS2_SIF_CALL_SEND_SIZE),
                    PS2_SIF_SERVER_BUF_SIZE);
    recv_addr = ldl_le_p(pkt + PS2_SIF_CALL_RECV_ADDR);
    recv_size = MIN(ldl_le_p(pkt + PS2_SIF_CALL_RECV_SIZE), 64 * KiB);
    addr = ldl_le_p(pkt + PS2_SIF_CALL_SERVER);

    for (i = 0; i < ARRAY_SIZE(ps2_sif_servers); i++) {
        if (ps2_sif_server_addr(i) == addr) {
            server = &ps2_sif_servers[i];
            break;
        }
    }
    trace_ps2_sif_rpc_call(server ? server->sid : 0, rpc_id, send_size,
                           recv_size);

    send = g_malloc0(PS2_SIF_SERVER_BUF_SIZE);
    recv = g_malloc0(recv_size);
    if (!server) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no RPC server at 0x%08x\n",
                      __func__, addr);
    } else {
        /* The send data came ahead of the packet, to the server buffer. */
        ps2_sif_iop_read(s, ps2_sif_server_buffer(i), send, send_size);
        server->call(s, rpc_id, send, send_size, recv, recv_size);
    }

    if (recv_size &&
        dma_memory_write(&address_space_memory, recv_addr, recv, recv_size,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad receive buffer 0x%08x\n",
                      __func__, recv_addr);
    }
    /* The EE waits for the end packet even if the call failed. */
    ps2_sif_rpc_end(s, client, PS2_SIF_CMD_RPC_CALL, addr,
                    server ? ps2_sif_server_buffer(i) : 0);
}

static void ps2_sif_reset_done(void *opaque)
{
    PS2SIFState *s = opaque;

    s->resetting = false;
    s->smflag |= PS2_SIF_STATUS_SIFINIT | PS2_SIF_STATUS_BOOTEND;
}

/* Forget everything the EE has set up on the IOP. */
static void ps2_sif_iop_clear(PS2SIFState *s)
{
    s->modules = 0;
    s->module_count = 0;
    s->heap_count = 0;
    memset(s->relay, 0, sizeof(s->relay));
    s->queue_head = 0;
    s->queue_len = 0;
}

static void ps2_sif_iop_reset(PS2SIFState *s)
{
    ps2_sif_iop_clear(s);
    s->resetting = true;
    timer_mod(s->reset_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PS2_SIF_RESET_DELAY_NS);
}

static void ps2_sif_command(PS2SIFState *s, const uint8_t *buf, uint32_t len)
{
    const uint8_t *payload = buf + PS2_SIF_CMD_HEADER_SIZE;
    uint32_t cmd, opt, plen;
    uint8_t sreg[8];

    if (len < PS2_SIF_CMD_HEADER_SIZE ||
        buf[0] < PS2_SIF_CMD_HEADER_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad command packet\n", __func__);
        return;
    }
    plen = MIN(buf[0], len) - PS2_SIF_CMD_HEADER_SIZE;
    cmd = ldl_le_p(buf + 8);
    opt = ldl_le_p(buf + 12);
    trace_ps2_sif_command(cmd, opt, plen);

    switch (cmd) {
    case PS2_SIF_CMD_INIT_CMD:
        if (opt == 0 && plen >= 4) {
            s->sif0_addr = ldl_le_p(payload);
        } else if (opt == 1) {
            /* The RPC services are up: set software register 0. */
            stl_le_p(sreg, 0);
            stl_le_p(sreg + 4, 1);
            ps2_sif_send(s, PS2_SIF_CMD_WRITE_SREG, 0, sreg, sizeof(sreg));
        }
        break;
    case PS2_SIF_CMD_RESET_CMD:
        ps2_sif_iop_reset(s);
        break;
    case PS2_SIF_CMD_RPC_END:
        /* The EE answered a bind request of ours; we never make any. */
        break;
    case PS2_SIF_CMD_RPC_BIND:
        ps2_sif_rpc_bind(s, payload, plen);
        break;
    case PS2_SIF_CMD_RPC_CALL:
        ps2_sif_rpc_call(s, payload, plen);
        break;
    case PS2_SIF_CMD_ATA:
        /*
         * Without DMA the EE drives the ATA registers itself, so its bounce
         * buffer for DMA goes unused.
         */
        if ((opt & PS2_SIF_ATA_OP_MASK) != PS2_SIF_ATA_BOUNCE_BUFFER) {
            qemu_log_mask(LOG_UNIMP, "%s: ATA DMA is not supported\n",
                          __func__);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented command 0x%08x\n",
                      __func__, cmd);
        break;
    }
}

/*
 * SIF1 carries IOP DMA tags, each followed by its data: the address in
 * IOP memory and a word count.  A tag with an interrupt marks a command
 * packet for the IOP command buffer.
 */
static void ps2_sif1_sink(void *opaque, const uint8_t *buf, uint32_t len)
{
    PS2SIFState *s = opaque;

    while (len >= 16) {
        uint32_t tag = ldl_le_p(buf);
        uint32_t addr = extract32(tag, 0, 24);
        uint32_t nbytes = MIN(ldl_le_p(buf + 4) * 4, len - 16);

        address_space_write(&s->iop_as, addr, MEMTXATTRS_UNSPECIFIED,
                            buf + 16, nbytes);
        if (tag & PS2_SIF_IOP_DMA_TAG_INT) {
            ps2_sif_command(s, buf + 16, nbytes);
        }

        nbytes = ROUND_UP(nbytes, 16);
        if (tag & PS2_SIF_IOP_DMA_TAG_ERT || nbytes >= len - 16) {
            break;
        }
        buf += 16 + nbytes;
        len -= 16 + nbytes;
    }
}

static const PS2DMACChannelOps ps2_sif0_ops = {
    .start = ps2_sif0_start,
};

static const PS2DMACChannelOps ps2_sif1_ops = {
    .sink = ps2_sif1_sink,
};

static void ps2_sif_iop_irq(void *opaque, int irq, int level)
{
    PS2SIFState *s = opaque;
    uint64_t bit = 1ull << irq;
    bool rising = level && !(s->iop_irq_level & bit);
    uint8_t relay[4];

    s->iop_irq_level = level ? s->iop_irq_level | bit
                             : s->iop_irq_level & ~bit;
    if (rising && s->relay[irq]) {
        stl_le_p(relay, s->relay[irq]);
        ps2_sif_send(s, PS2_SIF_CMD_IRQ_RELAY, 0, relay, sizeof(relay));
    }
}

static uint64_t ps2_sif_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2SIFState *s = opaque;

    switch (addr) {
    case PS2_SIF_MAINADDR:
        return s->mainaddr;
    case PS2_SIF_SUBADDR:
        return s->subaddr;
    case PS2_SIF_MSFLAG:
        return s->msflag;
    case PS2_SIF_SMFLAG:
        return s->smflag;
    case PS2_SIF_SUBCTRL:
        return s->subctrl;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void ps2_sif_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned int size)
{
    PS2SIFState *s = opaque;

    switch (addr) {
    case PS2_SIF_MAINADDR:
        s->mainaddr = value;
        s->sif0_addr = value;
        break;
    case PS2_SIF_MSFLAG:
        s->msflag |= value;
        /* The rebooted IOP sets up its command buffer when told to. */
        if (!s->resetting && (value & PS2_SIF_STATUS_CMDINIT)) {
            s->smflag |= PS2_SIF_STATUS_CMDINIT;
        }
        break;
    case PS2_SIF_SMFLAG:
        s->smflag &= ~value;
        break;
    case PS2_SIF_SUBCTRL:
        s->subctrl = value;
        break;
    case PS2_SIF_UNKNOWN60:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps ps2_sif_ops = {
    .read = ps2_sif_read,
    .write = ps2_sif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ps2_sif_reset_hold(Object *obj, ResetType type)
{
    PS2SIFState *s = PS2_SIF(obj);

    timer_del(s->reset_timer);
    ps2_sif_iop_clear(s);
    s->resetting = false;
    s->mainaddr = 0;
    s->subaddr = PS2_SIF_IOP_CMD_BUFFER;
    s->msflag = 0;
    /* The IOP has booted from ROM. */
    s->smflag = PS2_SIF_STATUS_SIFINIT | PS2_SIF_STATUS_CMDINIT |
                PS2_SIF_STATUS_BOOTEND;
    s->subctrl = 0;
    s->sif0_addr = 0;
    s->iop_irq_level = 0;
}

static void ps2_sif_init(Object *obj)
{
    PS2SIFState *s = PS2_SIF(obj);

    memory_region_init_io(&s->iomem, obj, &ps2_sif_ops, s, TYPE_PS2_SIF,
                          PS2_SIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), ps2_sif_iop_irq, "iop-irq",
                            PS2_SIF_NUM_IOP_IRQS);
}

static void ps2_sif_realize(DeviceState *dev, Error **errp)
{
    PS2SIFState *s = PS2_SIF(dev);

    if (!s->dmac || !s->iop_mr) {
        error_setg(errp, "%s: the dmac and iop-memory links must be set",
                   TYPE_PS2_SIF);
        return;
    }
    address_space_init(&s->iop_as, s->iop_mr, "ps2-iop");
    s->sif0_bh = qemu_bh_new_guarded(ps2_sif0_deliver, s,
                                     &dev->mem_reentrancy_guard);
    s->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ps2_sif_reset_done, s);
    ps2_dmac_set_channel_ops(s->dmac, PS2_DMAC_SIF0, &ps2_sif0_ops, s);
    ps2_dmac_set_channel_ops(s->dmac, PS2_DMAC_SIF1, &ps2_sif1_ops, s);
}

static const Property ps2_sif_properties[] = {
    DEFINE_PROP_LINK("dmac", PS2SIFState, dmac, TYPE_PS2_DMAC,
                     PS2DMACState *),
    DEFINE_PROP_LINK("iop-memory", PS2SIFState, iop_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static int ps2_sif_post_load(void *opaque, int version_id)
{
    PS2SIFState *s = opaque;

    if (s->queue_head >= PS2_SIF_QUEUE_LEN ||
        s->queue_len > PS2_SIF_QUEUE_LEN ||
        s->heap_count > PS2_SIF_HEAP_MAX_BLOCKS) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < s->queue_len; i++) {
        uint8_t *pkt = s->queue[(s->queue_head + i) % PS2_SIF_QUEUE_LEN];

        if (pkt[0] < PS2_SIF_CMD_HEADER_SIZE ||
            pkt[0] > PS2_SIF_CMD_PACKET_MAX) {
            return -EINVAL;
        }
    }
    qemu_bh_schedule(s->sif0_bh);
    return 0;
}

static const VMStateDescription vmstate_ps2_sif = {
    .name = TYPE_PS2_SIF,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ps2_sif_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mainaddr, PS2SIFState),
        VMSTATE_UINT32(subaddr, PS2SIFState),
        VMSTATE_UINT32(msflag, PS2SIFState),
        VMSTATE_UINT32(smflag, PS2SIFState),
        VMSTATE_UINT32(subctrl, PS2SIFState),
        VMSTATE_BOOL(resetting, PS2SIFState),
        VMSTATE_TIMER_PTR(reset_timer, PS2SIFState),
        VMSTATE_UINT32(sif0_addr, PS2SIFState),
        VMSTATE_UINT8_2DARRAY(queue, PS2SIFState, PS2_SIF_QUEUE_LEN,
                              PS2_SIF_CMD_PACKET_MAX),
        VMSTATE_UINT32(queue_head, PS2SIFState),
        VMSTATE_UINT32(queue_len, PS2SIFState),
        VMSTATE_UINT32(modules, PS2SIFState),
        VMSTATE_UINT32(module_count, PS2SIFState),
        VMSTATE_UINT32_ARRAY(heap_addr, PS2SIFState,
                             PS2_SIF_HEAP_MAX_BLOCKS),
        VMSTATE_UINT32_ARRAY(heap_size, PS2SIFState,
                             PS2_SIF_HEAP_MAX_BLOCKS),
        VMSTATE_UINT32(heap_count, PS2SIFState),
        VMSTATE_UINT8_ARRAY(relay, PS2SIFState, PS2_SIF_NUM_IOP_IRQS),
        VMSTATE_UINT64(iop_irq_level, PS2SIFState),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_sif_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_sif_reset_hold;
    dc->realize = ps2_sif_realize;
    dc->vmsd = &vmstate_ps2_sif;
    device_class_set_props(dc, ps2_sif_properties);
}

static const TypeInfo ps2_sif_types[] = {
    {
        .name = TYPE_PS2_SIF,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2SIFState),
        .instance_init = ps2_sif_init,
        .class_init = ps2_sif_class_init,
    },
};

DEFINE_TYPES(ps2_sif_types);
