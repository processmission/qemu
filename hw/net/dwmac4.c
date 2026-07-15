/*
 * Synopsys DesignWare Ethernet MAC dwmac-4.20a (GMAC4).
 *
 * Copyright (c) 2026 Chao Liu
 *
 * RK3588 wires this for its GMAC node (snps,dwmac-4.20a /
 * rockchip,rk3588-gmac). The model is driven by the GMAC4 register map
 * (dwmac4.h / dwmac4_dma.h / dwmac4_descs.h in the Linux stmmac driver):
 *
 *   - MAC register bank 0x000..0x3ff (MAC_CONFIG, PACKET_FILTER, ADDR slots,
 *     MDIO, HW_FEATURE0..3, and board-selectable MAC_VERSION @ 0x110.
 *   - DMA register bank 0x1000..0x11ff (DMA_BUS_MODE, per-channel block at
 *     0x1100 + chan*0x80: TX/RX_CONTROL, TX/RX_BASE_ADDR, RING_LEN,
 *     INTR_ENA, CHAN_STATUS as W1C with 4.10-layout NIS/AIS).
 *   - dwmac4 4-word descriptor format: OWN/FIRST/LAST in TDES3/RDES3, buffer
 *     addr in DES0, sizes+IOC in TDES2, RX write-back length in RDES3.
 *
 * The whole 64 KiB MMIO window is wrapped by a custom dispatcher that RAZ/WIs
 * any offset the banks do not model - guest writes to "unassigned" offsets
 * (MTL, PTP, GRF mirror, etc.) must NOT abort the AArch64 CPU (lesson D-15).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/net/dwmac4.h"
#include "migration/vmstate.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/dma.h"

/* ---- MAC register offsets (GMAC4 family) ----------------------------- */

REG32(GMAC_CONFIG,                0x000)
    FIELD(GMAC_CONFIG, RE,         0, 1)
    FIELD(GMAC_CONFIG, TE,         1, 1)
REG32(GMAC_EXT_CONFIG,            0x004)
REG32(GMAC_PACKET_FILTER,         0x008)
REG32(GMAC_HASH_TAB0,             0x010)
REG32(GMAC_HASH_TAB1,             0x014)
REG32(GMAC_Q0_TX_FLOW_CTL,        0x070)
REG32(GMAC_RX_FLOW_CTRL,          0x090)
REG32(GMAC_TXQ_PRTY_MAP0,         0x098)
REG32(GMAC_TXQ_PRTY_MAP1,         0x09c)
REG32(GMAC_RXQ_CTRL0,             0x0a0)
REG32(GMAC_RXQ_CTRL1,             0x0a4)
REG32(GMAC_RXQ_CTRL2,             0x0a8)
REG32(GMAC_RXQ_CTRL3,             0x0ac)
REG32(GMAC_INT_STATUS,            0x0b0)
REG32(GMAC_INT_EN,                0x0b4)
REG32(GMAC_PMT,                   0x0c0)
REG32(GMAC4_LPI_CTRL_STATUS,      0x0d0)
REG32(GMAC4_LPI_TIMER_CTRL,       0x0d4)
REG32(GMAC_PHYIF_CONTROL_STATUS,  0x0f8)
    FIELD(GMAC_PHYIF_CONTROL_STATUS, TC,    0, 1)
    FIELD(GMAC_PHYIF_CONTROL_STATUS, LUD,   1, 1)
    FIELD(GMAC_PHYIF_CONTROL_STATUS, RGS,   2, 1)
REG32(GMAC4_VERSION,              0x110)
REG32(GMAC_DEBUG,                 0x114)
REG32(GMAC_HW_FEATURE0,           0x11c)
    FIELD(GMAC_HW_FEATURE0, RXCOESEL, 0, 1)   /* dev->hwts_rx_en */
REG32(GMAC_HW_FEATURE1,           0x120)
REG32(GMAC_HW_FEATURE2,           0x124)
REG32(GMAC_HW_FEATURE3,           0x128)
REG32(GMAC_MDIO_ADDR,             0x200)
REG32(GMAC_MDIO_DATA,             0x204)
REG32(GMAC_GPIO_STATUS,           0x20c)
REG32(GMAC_ARP_ADDR,              0x210)
REG32(GMAC_EXT_CFG1,              0x238)
REG32(GMAC_ADDR_HIGH0,            0x300)
REG32(GMAC_ADDR_LOW0,             0x304)

/* ---- DMA register offsets ------------------------------------------- */

REG32(DMA_BUS_MODE,               0x000)   /* +0x1000 - soft reset bit0 */
    FIELD(DMA_BUS_MODE, SFT_RESET, 0, 1)
REG32(DMA_SYS_BUS_MODE,           0x004)
REG32(DMA_STATUS,                 0x008)   /* legacy global status */
REG32(DMA_AXI_BUS_MODE,           0x028)
REG32(DMA_TBS_CTRL,               0x050)

/*
 * Per-channel DMA block: base 0x1100, stride 0x80. The contract calls out
 * chan 0 as primary. We expose chan 0 (and chan 1) at the natural offsets.
 * Below REG32 names are chan-relative (offset within the per-channel block),
 * the absolute address is DWMAC4_DMA_REG_BASE + 0x100 + chan*0x80 + off.
 */
#define DMA_CHAN_BLOCK_BASE      0x100     /* within DMA bank: 0x1000+0x100 */
REG32(DMA_CHAN_CONTROL,           0x00)
REG32(DMA_CHAN_TX_CONTROL,        0x04)
    FIELD(DMA_CHAN_TX_CONTROL, ST, 0, 1)
REG32(DMA_CHAN_RX_CONTROL,        0x08)
    FIELD(DMA_CHAN_RX_CONTROL, SR, 0, 1)
    FIELD(DMA_CHAN_RX_CONTROL, RBSZ, 1, 14)
REG32(DMA_CHAN_TX_BASE_ADDR_HI,   0x10)
REG32(DMA_CHAN_TX_BASE_ADDR,      0x14)
REG32(DMA_CHAN_RX_BASE_ADDR_HI,   0x18)
REG32(DMA_CHAN_RX_BASE_ADDR,      0x1c)
REG32(DMA_CHAN_TX_END_ADDR,       0x20)   /* TX tail pointer */
REG32(DMA_CHAN_RX_END_ADDR,       0x28)   /* RX tail pointer */
REG32(DMA_CHAN_TX_RING_LEN,       0x2c)
REG32(DMA_CHAN_RX_RING_LEN,       0x30)
REG32(DMA_CHAN_INTR_ENA,          0x34)
REG32(DMA_CHAN_RX_WATCHDOG,       0x38)
REG32(DMA_CHAN_SLOT_CTRL_STATUS,  0x3c)
REG32(DMA_CHAN_CUR_TX_DESC,       0x44)
REG32(DMA_CHAN_CUR_RX_DESC,       0x4c)
REG32(DMA_CHAN_CUR_TX_BUF_ADDR_HI, 0x50)
REG32(DMA_CHAN_CUR_TX_BUF_ADDR,   0x54)
REG32(DMA_CHAN_CUR_RX_BUF_ADDR_HI, 0x58)
REG32(DMA_CHAN_CUR_RX_BUF_ADDR,   0x5c)
REG32(DMA_CHAN_STATUS,            0x60)
    FIELD(DMA_CHAN_STATUS, TI,   0, 1)
    FIELD(DMA_CHAN_STATUS, TPS,  1, 1)
    FIELD(DMA_CHAN_STATUS, TBU,  2, 1)
    FIELD(DMA_CHAN_STATUS, RI,   6, 1)
    FIELD(DMA_CHAN_STATUS, RBU,  7, 1)
    FIELD(DMA_CHAN_STATUS, RPS,  8, 1)
    FIELD(DMA_CHAN_STATUS, RWT,  9, 1)
    FIELD(DMA_CHAN_STATUS, ETI, 10, 1)
    FIELD(DMA_CHAN_STATUS, ERI, 11, 1)
    FIELD(DMA_CHAN_STATUS, FBE, 12, 1)
    FIELD(DMA_CHAN_STATUS, CDE, 13, 1)
    /* 4.10 layout: AIS=14, NIS=15 (vs 4.00 which used 15/16). */
    FIELD(DMA_CHAN_STATUS, AIS, 14, 1)
    FIELD(DMA_CHAN_STATUS, NIS, 15, 1)

#define DMA_CHAN_STATUS_W1C_BITS  (R_DMA_CHAN_STATUS_TI_MASK   | \
                                   R_DMA_CHAN_STATUS_TPS_MASK  | \
                                   R_DMA_CHAN_STATUS_TBU_MASK  | \
                                   R_DMA_CHAN_STATUS_RI_MASK   | \
                                   R_DMA_CHAN_STATUS_RBU_MASK  | \
                                   R_DMA_CHAN_STATUS_RPS_MASK  | \
                                   R_DMA_CHAN_STATUS_RWT_MASK  | \
                                   R_DMA_CHAN_STATUS_ETI_MASK  | \
                                   R_DMA_CHAN_STATUS_ERI_MASK  | \
                                   R_DMA_CHAN_STATUS_FBE_MASK  | \
                                   R_DMA_CHAN_STATUS_CDE_MASK  | \
                                   R_DMA_CHAN_STATUS_AIS_MASK  | \
                                   R_DMA_CHAN_STATUS_NIS_MASK)

/* INTR_ENA bits - 4.10 layout (NIE=15, AIE=14). */
#define DMA_CHAN_INTR_TIE         BIT(0)
#define DMA_CHAN_INTR_RIE         BIT(6)
#define DMA_CHAN_INTR_RBUE        BIT(7)
#define DMA_CHAN_INTR_RSE         BIT(8)
#define DMA_CHAN_INTR_FBE         BIT(12)
#define DMA_CHAN_INTR_AIE         BIT(14)
#define DMA_CHAN_INTR_NIE         BIT(15)

/*
 * Default interrupt mask the stmmac driver writes for dwmac-4.10:
 * NIE|RIE|TIE|AIE|RSE|RBUE|FBE. We don't enforce it; we just AND STATUS
 * with INTR_ENA when deciding whether to assert IRQ.
 */

/* ---- dwmac4 descriptor layout (4-word / 16-byte) -------------------- */
/*
 * TDES3 / RDES3 share the OWN bit (31). FIRST=29, LAST=28. For RX
 * write-back, PACKET_SIZE lives in RDES3[14:0]. IOC for TX is TDES2[31].
 */
#define DESC_OWN                  BIT(31)
#define TX_DESC3_FIRST            BIT(29)
#define TX_DESC3_LAST             BIT(28)
#define TX_DESC3_CIC_MASK         0x00030000u
#define TX_DESC2_IOC              BIT(31)
#define TX_DESC2_B1SZ_MASK        0x00003fffu   /* BUFFER1_SIZE[13:0] */
#define RX_DESC3_FIRST            BIT(29)
#define RX_DESC3_LAST             BIT(28)
#define RX_DESC3_PKT_SIZE_MASK    0x00007fffu   /* PACKET_SIZE[14:0] */

/* ---- Forward decls -------------------------------------------------- */

static void dwmac4_update_irq(DWMAC4State *s);

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static inline uint32_t dwmac4_chan_status_addr(int chan)
{
    return DMA_CHAN_BLOCK_BASE + chan * DWMAC4_DMA_CHAN_STRIDE +
           A_DMA_CHAN_STATUS;
}

static inline uint32_t dwmac4_chan_intr_ena_addr(int chan)
{
    return DMA_CHAN_BLOCK_BASE + chan * DWMAC4_DMA_CHAN_STRIDE +
           A_DMA_CHAN_INTR_ENA;
}

static inline uint32_t dwmac4_chan_reg(int chan, hwaddr intra)
{
    return DMA_CHAN_BLOCK_BASE + chan * DWMAC4_DMA_CHAN_STRIDE + intra;
}

static int dwmac4_read_desc(hwaddr addr, uint32_t w[4])
{
    if (dma_memory_read(&address_space_memory, addr, w, 16,
                        MEMTXATTRS_UNSPECIFIED)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dwmac4: failed DMA read of descriptor @ 0x%"
                      HWADDR_PRIx "\n", addr);
        return -1;
    }
    for (int i = 0; i < 4; i++) {
        w[i] = le32_to_cpu(w[i]);
    }
    return 0;
}

static int dwmac4_write_desc(hwaddr addr, const uint32_t w[4])
{
    uint32_t le[4];
    for (int i = 0; i < 4; i++) {
        le[i] = cpu_to_le32(w[i]);
    }
    if (dma_memory_write(&address_space_memory, addr, le, 16,
                         MEMTXATTRS_UNSPECIFIED)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dwmac4: failed DMA write of descriptor @ 0x%"
                      HWADDR_PRIx "\n", addr);
        return -1;
    }
    return 0;
}

/* ===================================================================== */
/* IRQ                                                                   */
/* ===================================================================== */

static void dwmac4_update_irq(DWMAC4State *s)
{
    /*
     * macirq is asserted when any enabled status bit is set on any channel.
     * We model chan 0 (primary); also fold in chan 1 for completeness.
     */
    bool level = false;
    for (int chan = 0; chan < DWMAC4_NR_CHANNELS; chan++) {
        uint32_t status = s->dma_regs[dwmac4_chan_status_addr(chan) / 4];
        uint32_t ena = s->dma_regs[dwmac4_chan_intr_ena_addr(chan) / 4];
        if (status & ena) {
            level = true;
            break;
        }
    }
    qemu_set_irq(s->sb_irq, level);
}

/* ===================================================================== */
/* TX descriptor-ring walk                                               */
/* ===================================================================== */

static void dwmac4_try_send(DWMAC4State *s, int chan)
{
    g_autoptr(GByteArray) frame = NULL;
    bool frame_checksum = false;
    hwaddr base_addr = ((uint64_t)s->dma_regs[dwmac4_chan_reg(chan,
                          A_DMA_CHAN_TX_BASE_ADDR_HI) / 4] << 32) |
                       s->dma_regs[dwmac4_chan_reg(chan,
                          A_DMA_CHAN_TX_BASE_ADDR) / 4];
    uint32_t ring_len = s->dma_regs[dwmac4_chan_reg(chan,
                          A_DMA_CHAN_TX_RING_LEN) / 4];
    uint64_t ring_entries = (uint64_t)ring_len + 1;
    if (!base_addr) {
        return;
    }

    NetClientState *nc = qemu_get_queue(s->nic);
    hwaddr cur = s->tx_desc_cur[chan];
    if (!cur) {
        cur = base_addr;
    }

    for (uint64_t i = 0; i < ring_entries; i++) {
        uint32_t d[4];
        if (dwmac4_read_desc(cur, d)) {
            return;
        }

        /* OWN=1 -> MAC owns, can TX. Otherwise ring drained for now. */
        if (!(d[3] & DESC_OWN)) {
            break;
        }

        bool first = d[3] & TX_DESC3_FIRST;
        bool last = d[3] & TX_DESC3_LAST;
        bool ioc = d[2] & TX_DESC2_IOC;
        bool checksum_insertion = d[3] & TX_DESC3_CIC_MASK;
        uint32_t b1sz = d[2] & TX_DESC2_B1SZ_MASK;
        /*
         * dwmac4 TDES0 holds buffer-1 address (low 32). In extended 64-bit
         * descriptor mode TDES1 holds the high 32 bits; in legacy 32-bit
         * mode TDES1 holds a separate buffer-2 address. stmmac programs
         * 64-bit descriptors only when ADDR64 (HW_FEATURE1) is set; we
         * keep things simple and treat TDES0|TDES1<<32 as buf1 to cover
         * both layouts for the single-buffer frames we send.
         */
        hwaddr b1addr = ((uint64_t)d[1] << 32) | d[0];

        if (first) {
            g_clear_pointer(&frame, g_byte_array_unref);
            frame = g_byte_array_new();
            frame_checksum = checksum_insertion;
        }

        if (frame && b1sz) {
            size_t offset = frame->len;

            if (offset + b1sz > 65536) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dwmac4: TX frame exceeds 64 KiB\n");
                return;
            }
            g_byte_array_set_size(frame, offset + b1sz);
            if (dma_memory_read(&address_space_memory, b1addr,
                                frame->data + offset, b1sz,
                                MEMTXATTRS_UNSPECIFIED)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dwmac4: TX buffer read failed @ 0x%"
                              HWADDR_PRIx "\n", b1addr);
                return;
            }
        }

        if (last && frame) {
            if (frame_checksum) {
                net_checksum_calculate(frame->data, frame->len, CSUM_ALL);
            }
            qemu_send_packet(nc, frame->data, frame->len);
            g_clear_pointer(&frame, g_byte_array_unref);
        }

        /* Write-back: clear OWN. Error summary (bit15) left clear. */
        d[3] &= ~DESC_OWN;
        if (dwmac4_write_desc(cur, d)) {
            return;
        }

        cur += 16;
        if (cur >= base_addr + ring_entries * 16) {
            cur = base_addr;
        }

        if (ioc) {
            uint32_t off = dwmac4_chan_status_addr(chan) / 4;
            s->dma_regs[off] |= R_DMA_CHAN_STATUS_TI_MASK |
                                R_DMA_CHAN_STATUS_NIS_MASK;
            dwmac4_update_irq(s);
        }

    }

    s->tx_desc_cur[chan] = cur;
}

/* ===================================================================== */
/* RX                                                                    */
/* ===================================================================== */

static bool dwmac4_can_receive(NetClientState *nc)
{
    DWMAC4State *s = DWMAC4(qemu_get_nic_opaque(nc));

    /* Need MAC RX enable and DMA RX start on chan 0. */
    if (!ARRAY_FIELD_EX32(s->mac_regs, GMAC_CONFIG, RE)) {
        return false;
    }
    int chan = 0;
    uint32_t rxctl = s->dma_regs[dwmac4_chan_reg(chan, A_DMA_CHAN_RX_CONTROL) / 4];
    if (!(rxctl & R_DMA_CHAN_RX_CONTROL_SR_MASK)) {
        return false;
    }
    return true;
}

static ssize_t dwmac4_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    DWMAC4State *s = DWMAC4(qemu_get_nic_opaque(nc));
    int chan = 0;

    if (!dwmac4_can_receive(nc)) {
        return 0;
    }

    hwaddr base_addr = ((uint64_t)s->dma_regs[dwmac4_chan_reg(chan,
                          A_DMA_CHAN_RX_BASE_ADDR_HI) / 4] << 32) |
                       s->dma_regs[dwmac4_chan_reg(chan,
                          A_DMA_CHAN_RX_BASE_ADDR) / 4];
    uint32_t ring_len = s->dma_regs[dwmac4_chan_reg(chan,
                          A_DMA_CHAN_RX_RING_LEN) / 4];
    uint64_t ring_entries = (uint64_t)ring_len + 1;
    if (!base_addr) {
        return len;   /* no ring posted yet - silently drop */
    }

    hwaddr cur = s->rx_desc_cur[chan];
    if (!cur) {
        cur = base_addr;
    }

    /* Find the next descriptor the MAC owns. */
    for (uint64_t i = 0; i < ring_entries; i++) {
        uint32_t d[4];
        if (dwmac4_read_desc(cur, d)) {
            return len;
        }
        if (!(d[3] & DESC_OWN)) {
            /* Driver still owns this one - ring full, drop the frame. */
            return len;
        }

        hwaddr b1addr = ((uint64_t)d[1] << 32) | d[0];
        /* DWMAC4 keeps the receive buffer size in DMA_CHAN_RX_CONTROL. */
        uint32_t rxctl = s->dma_regs[dwmac4_chan_reg(
            chan, A_DMA_CHAN_RX_CONTROL) / 4];
        uint32_t b1sz = FIELD_EX32(rxctl, DMA_CHAN_RX_CONTROL, RBSZ);
        uint32_t to_write = MIN((uint32_t)len, b1sz);

        if (to_write && dma_memory_write(&address_space_memory, b1addr, buf,
                                         to_write, MEMTXATTRS_UNSPECIFIED)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "dwmac4: RX buffer write failed @ 0x%"
                          HWADDR_PRIx "\n", b1addr);
            return len;
        }

        /*
         * Write-back: clear OWN, set FIRST+LAST, set packet length in
         * RDES3[14:0]. We treat each frame as a single descriptor.
         */
        d[3] &= ~DESC_OWN;
        d[3] &= ~RX_DESC3_PKT_SIZE_MASK;
        /*
         * GMAC4 reports a packet size that includes the four-byte Ethernet
         * FCS, although the DMA buffer does not contain the FCS.  stmmac
         * accounts for that contract by subtracting ETH_FCS_LEN before it
         * hands the skb to the network stack.
         */
        d[3] |= RX_DESC3_FIRST | RX_DESC3_LAST |
                (((uint32_t)len + ETH_FCS_LEN) & RX_DESC3_PKT_SIZE_MASK);
        if (dwmac4_write_desc(cur, d)) {
            return len;
        }

        cur += 16;
        if (cur >= base_addr + ring_entries * 16) {
            cur = base_addr;
        }
        s->rx_desc_cur[chan] = cur;

        /* Raise RI + NIS. */
        uint32_t off = dwmac4_chan_status_addr(chan) / 4;
        s->dma_regs[off] |= R_DMA_CHAN_STATUS_RI_MASK |
                            R_DMA_CHAN_STATUS_NIS_MASK;
        dwmac4_update_irq(s);
        return len;
    }

    return len;
}

static void dwmac4_cleanup(NetClientState *nc)
{
}

static void dwmac4_link_status_changed(NetClientState *nc)
{
    /* Nothing to model - link state is reflected only via PHY MDIO reads. */
}

static NetClientInfo net_dwmac4_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = dwmac4_can_receive,
    .receive = dwmac4_receive,
    .cleanup = dwmac4_cleanup,
    .link_status_changed = dwmac4_link_status_changed,
};

/* ===================================================================== */
/* MDIO (clause-22, minimal PHY)                                         */
/* ===================================================================== */

#define GMAC4_MDIO_BUSY       BIT(0)
#define GMAC4_MDIO_GOC(v)     extract32((v), 2, 2)
#define GMAC4_MDIO_GOC_WRITE  1
#define GMAC4_MDIO_GOC_READ   3
#define GMAC4_MDIO_RDA(v)     extract32((v), 16, 5)
#define GMAC4_MDIO_PA(v)      extract32((v), 21, 5)

static void dwmac4_mdio(DWMAC4State *s, uint32_t v)
{
    bool busy = v & GMAC4_MDIO_BUSY;
    uint8_t goc = GMAC4_MDIO_GOC(v);
    bool is_gmac4 = goc == GMAC4_MDIO_GOC_READ ||
                    goc == GMAC4_MDIO_GOC_WRITE;
    bool write = is_gmac4 ? goc == GMAC4_MDIO_GOC_WRITE : v & BIT(1);
    uint8_t pa = is_gmac4 ? GMAC4_MDIO_PA(v) : (v >> 11) & 0x1f;
    uint8_t gr = is_gmac4 ? GMAC4_MDIO_RDA(v) : (v >> 6) & 0x1f;
    uint16_t data;

    if (!busy) {
        s->mac_regs[R_GMAC_MDIO_ADDR] = v;
        return;
    }
    if (pa != s->phy_addr) {
        if (!write) {
            s->mac_regs[R_GMAC_MDIO_DATA] = 0xffff;
        }
        s->mac_regs[R_GMAC_MDIO_ADDR] = v & ~GMAC4_MDIO_BUSY;
        return;
    }

    if (write) {
        data = s->mac_regs[R_GMAC_MDIO_DATA] & 0xffff;
        if (gr == 31) {
            s->phy_page = data;
        } else if (s->phy_page == 0) {
            /* BMCR reset and autoneg restart are self-clearing commands. */
            s->phy_regs[gr] = gr == 0 ? data & ~(BIT(15) | BIT(9)) : data;
        }
    } else {
        if (gr == 31) {
            s->mac_regs[R_GMAC_MDIO_DATA] = s->phy_page;
        } else {
            /* Vendor pages are accepted as RAZ/WI; page 0 is clause-22. */
            s->mac_regs[R_GMAC_MDIO_DATA] =
                s->phy_page == 0 ? s->phy_regs[gr] : 0;
        }
    }

    /* BUSY is self-clearing in hardware; model that. */
    s->mac_regs[R_GMAC_MDIO_ADDR] = v & ~GMAC4_MDIO_BUSY;
}

/* ===================================================================== */
/* Register side-effects (registerinfo callbacks)                        */
/* ===================================================================== */

static void gmac_config_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    if (val & R_GMAC_CONFIG_RE_MASK) {
        /* RX was just enabled - flush any queued packets from the netdev. */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void gmac_addr_high0_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    s->conf.macaddr.a[0] = (val >> 8) & 0xff;
    s->conf.macaddr.a[1] = val & 0xff;
}

static void gmac_addr_low0_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    s->conf.macaddr.a[2] = (val >> 24) & 0xff;
    s->conf.macaddr.a[3] = (val >> 16) & 0xff;
    s->conf.macaddr.a[4] = (val >> 8) & 0xff;
    s->conf.macaddr.a[5] = val & 0xff;
}

static void gmac_mdio_addr_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    dwmac4_mdio(s, val);
}

static void dma_bus_mode_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    if (val & R_DMA_BUS_MODE_SFT_RESET_MASK) {
        /*
         * Soft reset: clear the per-channel state and the SFT_RESET bit,
         * matching dwmac4_dma_reset() in the driver. We do NOT touch the
         * MAC bank (driver separates MAC reset from DMA reset).
         */
        for (int chan = 0; chan < DWMAC4_NR_CHANNELS; chan++) {
            s->tx_desc_cur[chan] = 0;
            s->rx_desc_cur[chan] = 0;
        }
        s->dma_regs[R_DMA_BUS_MODE] &= ~R_DMA_BUS_MODE_SFT_RESET_MASK;
    }
}

/*
 * Map a DMA-bank-relative offset to a channel index, or -1 if it's not a
 * per-channel register. The DMA bank starts at 0x1000; per-channel block
 * starts at +0x100 within the bank.
 */
static int dwmac4_chan_of_dma(hwaddr off_in_bank)
{
    if (off_in_bank < DMA_CHAN_BLOCK_BASE) {
        return -1;
    }
    hwaddr rel = off_in_bank - DMA_CHAN_BLOCK_BASE;
    if (rel >= DWMAC4_NR_CHANNELS * DWMAC4_DMA_CHAN_STRIDE) {
        return -1;
    }
    if ((rel % DWMAC4_DMA_CHAN_STRIDE) >= 0x80) {
        return -1;
    }
    return rel / DWMAC4_DMA_CHAN_STRIDE;
}

static void dma_chan_tx_tail_postw(RegisterInfo *reg, uint64_t val)
{
    /* The TX_END_ADDR register is the tail pointer; writing it kicks TX. */
    DWMAC4State *s = DWMAC4(reg->opaque);
    int chan = dwmac4_chan_of_dma(reg->access->addr);
    if (chan < 0) {
        return;
    }
    uint32_t txctl = s->dma_regs[dwmac4_chan_reg(chan,
                                                A_DMA_CHAN_TX_CONTROL) / 4];
    if (txctl & R_DMA_CHAN_TX_CONTROL_ST_MASK) {
        dwmac4_try_send(s, chan);
    }
}

static void dma_chan_tx_base_postw(RegisterInfo *reg, uint64_t val)
{
    /* Ring base reprogrammed - reset the walk cursor so we start fresh. */
    DWMAC4State *s = DWMAC4(reg->opaque);
    int chan = dwmac4_chan_of_dma(reg->access->addr);
    if (chan >= 0) {
        s->tx_desc_cur[chan] = 0;
    }
}

static void dma_chan_rx_base_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    int chan = dwmac4_chan_of_dma(reg->access->addr);
    if (chan >= 0) {
        s->rx_desc_cur[chan] = 0;
    }
}

static void dma_chan_tx_control_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    int chan = dwmac4_chan_of_dma(reg->access->addr);
    if (chan < 0) {
        return;
    }
    if (val & R_DMA_CHAN_TX_CONTROL_ST_MASK) {
        dwmac4_try_send(s, chan);
    }
}

static void dma_chan_rx_control_postw(RegisterInfo *reg, uint64_t val)
{
    DWMAC4State *s = DWMAC4(reg->opaque);
    if (val & R_DMA_CHAN_RX_CONTROL_SR_MASK) {
        /* SR=1 - RX DMA started; tell the netdev we may now receive. */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void dma_chan_rx_tail_postw(RegisterInfo *reg, uint64_t val)
{
    /* RX_END_ADDR/tail pointer write - driver kicks RX after refilling. */
    DWMAC4State *s = DWMAC4(reg->opaque);
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static void dma_chan_status_postw(RegisterInfo *reg, uint64_t val)
{
    /* W1C handled by registerinfo (.w1c=...); recompute the IRQ line. */
    DWMAC4State *s = DWMAC4(reg->opaque);
    dwmac4_update_irq(s);
}

/* ===================================================================== */
/* Register definitions                                                  */
/* ===================================================================== */

static const RegisterAccessInfo mac_regs_info[] = {
    { .name = "GMAC_CONFIG",               .addr = A_GMAC_CONFIG,
      .post_write = gmac_config_postw,
    },
    { .name = "GMAC_EXT_CONFIG",           .addr = A_GMAC_EXT_CONFIG,
    },
    { .name = "GMAC_PACKET_FILTER",        .addr = A_GMAC_PACKET_FILTER,
    },
    { .name = "GMAC_HASH_TAB0",            .addr = A_GMAC_HASH_TAB0,
    },
    { .name = "GMAC_HASH_TAB1",            .addr = A_GMAC_HASH_TAB1,
    },
    { .name = "GMAC_Q0_TX_FLOW_CTL",       .addr = A_GMAC_Q0_TX_FLOW_CTL,
    },
    { .name = "GMAC_RX_FLOW_CTRL",         .addr = A_GMAC_RX_FLOW_CTRL,
    },
    { .name = "GMAC_TXQ_PRTY_MAP0",        .addr = A_GMAC_TXQ_PRTY_MAP0,
    },
    { .name = "GMAC_TXQ_PRTY_MAP1",        .addr = A_GMAC_TXQ_PRTY_MAP1,
    },
    { .name = "GMAC_RXQ_CTRL0",            .addr = A_GMAC_RXQ_CTRL0,
    },
    { .name = "GMAC_RXQ_CTRL1",            .addr = A_GMAC_RXQ_CTRL1,
    },
    { .name = "GMAC_RXQ_CTRL2",            .addr = A_GMAC_RXQ_CTRL2,
    },
    { .name = "GMAC_RXQ_CTRL3",            .addr = A_GMAC_RXQ_CTRL3,
    },
    { .name = "GMAC_INT_STATUS",           .addr = A_GMAC_INT_STATUS,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "GMAC_INT_EN",               .addr = A_GMAC_INT_EN,
    },
    { .name = "GMAC_PMT",                  .addr = A_GMAC_PMT,
    },
    { .name = "GMAC4_LPI_CTRL_STATUS",     .addr = A_GMAC4_LPI_CTRL_STATUS,
    },
    { .name = "GMAC4_LPI_TIMER_CTRL",      .addr = A_GMAC4_LPI_TIMER_CTRL,
    },
    { .name = "GMAC_PHYIF_CONTROL_STATUS", .addr = A_GMAC_PHYIF_CONTROL_STATUS,
      /* Link is always up; return LNKSTS-style bit so probe sees carrier. */
      .reset = BIT(19),
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "GMAC4_VERSION",             .addr = A_GMAC4_VERSION,
      .reset = DWMAC4_DEFAULT_SNPS_VERSION,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "GMAC_DEBUG",                .addr = A_GMAC_DEBUG,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    /*
     * HW_FEATURE0..3: report the minimum feature set needed for Linux
     * stmmac probe: RXCOESEL/TXCOSEL/MMCSEL/MGKSEL/TSSEL. Bit layout from
     * dwmac4.h: RXCOESEL=16, TXCOSEL=14, TSSEL=12, MMCSEL=8, MGKSEL=7.
     */
    { .name = "GMAC_HW_FEATURE0",          .addr = A_GMAC_HW_FEATURE0,
      .reset = BIT(16) | BIT(14) | BIT(12) | BIT(8) | BIT(7) | BIT(6) |
               BIT(5) | BIT(1) | BIT(0),
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "GMAC_HW_FEATURE1",          .addr = A_GMAC_HW_FEATURE1,
      /*
       * ADDR64(bits[15:14])=0 -> 32-bit descriptors.
       * TXFIFOSIZE(bits[10:6])=5 -> 4KB. RXFIFOSIZE(bits[4:0])=5 -> 4KB.
       */
      .reset = (0x5 << 6) | 0x5,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "GMAC_HW_FEATURE2",          .addr = A_GMAC_HW_FEATURE2,
      /*
       * All queue/channel count fields zero => 1 TX chan / 1 RX chan /
       * 1 TX queue / 1 RX queue (driver adds 1).
       */
      .reset = 0,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "GMAC_HW_FEATURE3",          .addr = A_GMAC_HW_FEATURE3,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "GMAC_MDIO_ADDR",            .addr = A_GMAC_MDIO_ADDR,
      .post_write = gmac_mdio_addr_postw,
    },
    { .name = "GMAC_MDIO_DATA",            .addr = A_GMAC_MDIO_DATA,
    },
    { .name = "GMAC_GPIO_STATUS",          .addr = A_GMAC_GPIO_STATUS,
    },
    { .name = "GMAC_ARP_ADDR",             .addr = A_GMAC_ARP_ADDR,
    },
    { .name = "GMAC_EXT_CFG1",             .addr = A_GMAC_EXT_CFG1,
    },
    { .name = "GMAC_ADDR_HIGH0",           .addr = A_GMAC_ADDR_HIGH0,
      .reset = 0x80000000,
      .post_write = gmac_addr_high0_postw,
    },
    { .name = "GMAC_ADDR_LOW0",            .addr = A_GMAC_ADDR_LOW0,
      .reset = 0xffffffff,
      .post_write = gmac_addr_low0_postw,
    },
};

static const RegisterAccessInfo dma_regs_info[] = {
    { .name = "DMA_BUS_MODE",              .addr = A_DMA_BUS_MODE,
      .post_write = dma_bus_mode_postw,
    },
    { .name = "DMA_SYS_BUS_MODE",          .addr = A_DMA_SYS_BUS_MODE,
    },
    { .name = "DMA_STATUS",                .addr = A_DMA_STATUS,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "DMA_AXI_BUS_MODE",          .addr = A_DMA_AXI_BUS_MODE,
    },
    { .name = "DMA_TBS_CTRL",              .addr = A_DMA_TBS_CTRL,
    },

/*
 * Per-channel DMA block: base 0x1100, stride 0x80. We model channels 0
 * and 1 explicitly. Each entry's .addr is the absolute offset within the
 * DMA bank (DMA_CHAN_BLOCK_BASE + chan*stride + intra).
 */
#define CHAN_ADDR(chan, name) \
    (DMA_CHAN_BLOCK_BASE + (chan) * DWMAC4_DMA_CHAN_STRIDE + A_DMA_CHAN_##name)

    /* Channel 0 (primary) */
    { .name = "DMA_CHAN_CONTROL0",          .addr = CHAN_ADDR(0, CONTROL), },
    { .name = "DMA_CHAN_TX_CONTROL0",       .addr = CHAN_ADDR(0, TX_CONTROL),
      .post_write = dma_chan_tx_control_postw,
    },
    { .name = "DMA_CHAN_RX_CONTROL0",       .addr = CHAN_ADDR(0, RX_CONTROL),
      .post_write = dma_chan_rx_control_postw,
    },
    { .name = "DMA_CHAN_TX_BASE_ADDR_HI0",  .addr = CHAN_ADDR(0, TX_BASE_ADDR_HI), },
    { .name = "DMA_CHAN_TX_BASE_ADDR0",     .addr = CHAN_ADDR(0, TX_BASE_ADDR),
      .post_write = dma_chan_tx_base_postw,
    },
    { .name = "DMA_CHAN_RX_BASE_ADDR_HI0",  .addr = CHAN_ADDR(0, RX_BASE_ADDR_HI), },
    { .name = "DMA_CHAN_RX_BASE_ADDR0",     .addr = CHAN_ADDR(0, RX_BASE_ADDR),
      .post_write = dma_chan_rx_base_postw,
    },
    { .name = "DMA_CHAN_TX_END_ADDR0",      .addr = CHAN_ADDR(0, TX_END_ADDR),
      .post_write = dma_chan_tx_tail_postw,
    },
    { .name = "DMA_CHAN_RX_END_ADDR0",      .addr = CHAN_ADDR(0, RX_END_ADDR),
      .post_write = dma_chan_rx_tail_postw,
    },
    { .name = "DMA_CHAN_TX_RING_LEN0",      .addr = CHAN_ADDR(0, TX_RING_LEN), },
    { .name = "DMA_CHAN_RX_RING_LEN0",      .addr = CHAN_ADDR(0, RX_RING_LEN), },
    { .name = "DMA_CHAN_INTR_ENA0",         .addr = CHAN_ADDR(0, INTR_ENA),
      .post_write = dma_chan_status_postw,
    },
    { .name = "DMA_CHAN_RX_WATCHDOG0",      .addr = CHAN_ADDR(0, RX_WATCHDOG), },
    { .name = "DMA_CHAN_SLOT_CTRL_STATUS0", .addr = CHAN_ADDR(0, SLOT_CTRL_STATUS), },
    { .name = "DMA_CHAN_CUR_TX_DESC0",      .addr = CHAN_ADDR(0, CUR_TX_DESC), },
    { .name = "DMA_CHAN_CUR_RX_DESC0",      .addr = CHAN_ADDR(0, CUR_RX_DESC), },
    { .name = "DMA_CHAN_CUR_TX_BUF_ADDR_HI0", .addr = CHAN_ADDR(0, CUR_TX_BUF_ADDR_HI), },
    { .name = "DMA_CHAN_CUR_TX_BUF_ADDR0",  .addr = CHAN_ADDR(0, CUR_TX_BUF_ADDR), },
    { .name = "DMA_CHAN_CUR_RX_BUF_ADDR_HI0", .addr = CHAN_ADDR(0, CUR_RX_BUF_ADDR_HI), },
    { .name = "DMA_CHAN_CUR_RX_BUF_ADDR0",  .addr = CHAN_ADDR(0, CUR_RX_BUF_ADDR), },
    { .name = "DMA_CHAN_STATUS0",           .addr = CHAN_ADDR(0, STATUS),
      .w1c = DMA_CHAN_STATUS_W1C_BITS,
      .post_write = dma_chan_status_postw,
    },

    /* Channel 1 (mirror; same callbacks). */
    { .name = "DMA_CHAN_CONTROL1",          .addr = CHAN_ADDR(1, CONTROL), },
    { .name = "DMA_CHAN_TX_CONTROL1",       .addr = CHAN_ADDR(1, TX_CONTROL),
      .post_write = dma_chan_tx_control_postw,
    },
    { .name = "DMA_CHAN_RX_CONTROL1",       .addr = CHAN_ADDR(1, RX_CONTROL),
      .post_write = dma_chan_rx_control_postw,
    },
    { .name = "DMA_CHAN_TX_BASE_ADDR_HI1",  .addr = CHAN_ADDR(1, TX_BASE_ADDR_HI), },
    { .name = "DMA_CHAN_TX_BASE_ADDR1",     .addr = CHAN_ADDR(1, TX_BASE_ADDR),
      .post_write = dma_chan_tx_base_postw,
    },
    { .name = "DMA_CHAN_RX_BASE_ADDR_HI1",  .addr = CHAN_ADDR(1, RX_BASE_ADDR_HI), },
    { .name = "DMA_CHAN_RX_BASE_ADDR1",     .addr = CHAN_ADDR(1, RX_BASE_ADDR),
      .post_write = dma_chan_rx_base_postw,
    },
    { .name = "DMA_CHAN_TX_END_ADDR1",      .addr = CHAN_ADDR(1, TX_END_ADDR),
      .post_write = dma_chan_tx_tail_postw,
    },
    { .name = "DMA_CHAN_RX_END_ADDR1",      .addr = CHAN_ADDR(1, RX_END_ADDR),
      .post_write = dma_chan_rx_tail_postw,
    },
    { .name = "DMA_CHAN_TX_RING_LEN1",      .addr = CHAN_ADDR(1, TX_RING_LEN), },
    { .name = "DMA_CHAN_RX_RING_LEN1",      .addr = CHAN_ADDR(1, RX_RING_LEN), },
    { .name = "DMA_CHAN_INTR_ENA1",         .addr = CHAN_ADDR(1, INTR_ENA),
      .post_write = dma_chan_status_postw,
    },
    { .name = "DMA_CHAN_RX_WATCHDOG1",      .addr = CHAN_ADDR(1, RX_WATCHDOG), },
    { .name = "DMA_CHAN_SLOT_CTRL_STATUS1", .addr = CHAN_ADDR(1, SLOT_CTRL_STATUS), },
    { .name = "DMA_CHAN_CUR_TX_DESC1",      .addr = CHAN_ADDR(1, CUR_TX_DESC), },
    { .name = "DMA_CHAN_CUR_RX_DESC1",      .addr = CHAN_ADDR(1, CUR_RX_DESC), },
    { .name = "DMA_CHAN_CUR_TX_BUF_ADDR_HI1", .addr = CHAN_ADDR(1, CUR_TX_BUF_ADDR_HI), },
    { .name = "DMA_CHAN_CUR_TX_BUF_ADDR1",  .addr = CHAN_ADDR(1, CUR_TX_BUF_ADDR), },
    { .name = "DMA_CHAN_CUR_RX_BUF_ADDR_HI1", .addr = CHAN_ADDR(1, CUR_RX_BUF_ADDR_HI), },
    { .name = "DMA_CHAN_CUR_RX_BUF_ADDR1",  .addr = CHAN_ADDR(1, CUR_RX_BUF_ADDR), },
    { .name = "DMA_CHAN_STATUS1",           .addr = CHAN_ADDR(1, STATUS),
      .w1c = DMA_CHAN_STATUS_W1C_BITS,
      .post_write = dma_chan_status_postw,
    },
#undef CHAN_ADDR
};

/* ===================================================================== */
/* Top-level MMIO dispatcher                                            */
/* ===================================================================== */
/*
 * Single 64 KiB I/O region. Reads/writes inside the MAC bank (0x0..0x3ff)
 * and the DMA bank (0x1000..0x11ff) are dispatched to the registerinfo
 * memory ops; everything else is RAZ/WI so that guest accesses to the
 * many unmodeled windows (MTL @ 0xc00, PTP, MAC addr slots 1..127,
 * L3/L4 filters @ 0x900+, vendor ranges) do NOT raise an AArch64
 * synchronous external abort (lesson D-15: qtest returns 0 silently but
 * a real guest boot would otherwise panic).
 */

static uint64_t dwmac4_read(void *opaque, hwaddr offset, unsigned size)
{
    DWMAC4State *s = DWMAC4(opaque);

    if (offset < DWMAC4_MAC_REG_SIZE) {
        return register_read_memory(s->mac_reg_array, offset, size);
    }
    if (offset >= DWMAC4_DMA_REG_BASE &&
        offset < DWMAC4_DMA_REG_BASE + DWMAC4_DMA_REG_SIZE) {
        return register_read_memory(s->dma_reg_array,
                                    offset - DWMAC4_DMA_REG_BASE, size);
    }
    /* RAZ everywhere else - no abort. */
    return 0;
}

static void dwmac4_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    DWMAC4State *s = DWMAC4(opaque);

    if (offset < DWMAC4_MAC_REG_SIZE) {
        register_write_memory(s->mac_reg_array, offset, value, size);
        return;
    }
    if (offset >= DWMAC4_DMA_REG_BASE &&
        offset < DWMAC4_DMA_REG_BASE + DWMAC4_DMA_REG_SIZE) {
        register_write_memory(s->dma_reg_array, offset - DWMAC4_DMA_REG_BASE,
                              value, size);
        return;
    }
    /* WI everywhere else - no abort. */
}

static const MemoryRegionOps dwmac4_ops = {
    .read = dwmac4_read,
    .write = dwmac4_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

/* ===================================================================== */
/* PHY reset                                                             */
/* ===================================================================== */

static const uint16_t phy_reg_init[32] = {
    /*
     * Minimal clause-22 PHY advertising 1000BASE-T full-duplex with link
     * up. Driver reads BMSR for link/AN-complete and CTRL1000/STAT1000 for
     * gigabit capability.
     */
    [1]  = 0x796d,                       /* MII_BMSR: LINK|ANEG|EXTCAP|... */
    [2]  = 0x001c,                       /* PHYIDR1: Realtek-style C22 PHY */
    [3]  = 0xc916,                       /* PHYIDR2 */
    [4]  = 0x01e1,                       /* MII_ADVERTISE: 100FX+100TX+10TX FD/HD */
    [5]  = 0x45e1,                       /* MII_LPA: partner capability */
    [9]  = 0x0300,                       /* MII_CTRL1000: 1000FD adv */
    [10] = 0x0800,                       /* MII_STAT1000: 1000FD link */
    [15] = 0x0004,                       /* idle */
};

/* ===================================================================== */
/* Device lifecycle                                                      */
/* ===================================================================== */

static void dwmac4_reset(DeviceState *dev)
{
    DWMAC4State *s = DWMAC4(dev);

    for (int i = 0; i < DWMAC4_MAC_NR_REGS; i++) {
        register_reset(&s->mac_regs_info[i]);
    }
    for (int i = 0; i < DWMAC4_DMA_NR_REGS; i++) {
        register_reset(&s->dma_regs_info[i]);
    }
    for (int chan = 0; chan < DWMAC4_NR_CHANNELS; chan++) {
        s->tx_desc_cur[chan] = 0;
        s->rx_desc_cur[chan] = 0;
    }
    memcpy(s->phy_regs, phy_reg_init, sizeof(s->phy_regs));
    s->phy_page = 0;
    s->phy_regs[2] = s->phy_id1;
    s->phy_regs[3] = s->phy_id2;

    s->mac_regs[R_GMAC4_VERSION] =
        ((uint32_t)s->user_version << 8) | s->snps_version;
    s->mac_regs[R_GMAC_HW_FEATURE1] =
        ((s->dma_width == 40 ? 1 : s->dma_width == 48 ? 2 : 0) << 14) |
        ((ctz32(s->tx_fifo_size) - 7) << 6) |
        (ctz32(s->rx_fifo_size) - 7) |
        (s->tso ? BIT(18) : 0);

    /* Reflect the configured MAC address into MAC_ADDR_HIGH0/LOW0. */
    s->mac_regs[R_GMAC_ADDR_HIGH0] = 0x80000000 |
        ((s->conf.macaddr.a[0] << 8) | s->conf.macaddr.a[1]);
    s->mac_regs[R_GMAC_ADDR_LOW0] =
        (s->conf.macaddr.a[2] << 24) |
        (s->conf.macaddr.a[3] << 16) |
        (s->conf.macaddr.a[4] << 8) |
        s->conf.macaddr.a[5];
}

static void dwmac4_realize(DeviceState *dev, Error **errp)
{
    DWMAC4State *s = DWMAC4(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->dma_width != 32 && s->dma_width != 40 && s->dma_width != 48) {
        error_setg(errp, "dwmac4: dma-width must be 32, 40, or 48");
        return;
    }
    if (!is_power_of_2(s->tx_fifo_size) || s->tx_fifo_size < 128 ||
        s->tx_fifo_size > 4 * MiB) {
        error_setg(errp, "dwmac4: tx-fifo-size must be a power of two "
                   "between 128 bytes and 4 MiB");
        return;
    }
    if (!is_power_of_2(s->rx_fifo_size) || s->rx_fifo_size < 128 ||
        s->rx_fifo_size > 4 * MiB) {
        error_setg(errp, "dwmac4: rx-fifo-size must be a power of two "
                   "between 128 bytes and 4 MiB");
        return;
    }
    if (s->phy_addr > 31) {
        error_setg(errp, "dwmac4: phy-addr must be in the range 0..31");
        return;
    }

    /*
     * Two register blocks, each giving us a MemoryRegion + RegisterInfo
     * array. We keep references to the arrays so our top-level dispatcher
     * can call register_{read,write}_memory on them.
     */
    s->mac_reg_array = register_init_block32(dev, mac_regs_info,
                                             ARRAY_SIZE(mac_regs_info),
                                             s->mac_regs_info, s->mac_regs,
                                             &dwmac4_ops, false,
                                             DWMAC4_MAC_REG_SIZE);
    s->dma_reg_array = register_init_block32(dev, dma_regs_info,
                                             ARRAY_SIZE(dma_regs_info),
                                             s->dma_regs_info, s->dma_regs,
                                             &dwmac4_ops, false,
                                             DWMAC4_DMA_REG_SIZE);

    memory_region_init_io(&s->iomem, OBJECT(s), &dwmac4_ops, s, TYPE_DWMAC4,
                          DWMAC4_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->sb_irq);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);

    s->nic = qemu_new_nic(&net_dwmac4_info, &s->conf, TYPE_DWMAC4, dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void dwmac4_unrealize(DeviceState *dev)
{
    DWMAC4State *s = DWMAC4(dev);
    qemu_del_nic(s->nic);
}

static const VMStateDescription vmstate_dwmac4 = {
    .name = TYPE_DWMAC4,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(mac_regs, DWMAC4State, DWMAC4_MAC_NR_REGS),
        VMSTATE_UINT32_ARRAY(dma_regs, DWMAC4State, DWMAC4_DMA_NR_REGS),
        VMSTATE_UINT64_ARRAY(tx_desc_cur, DWMAC4State, DWMAC4_NR_CHANNELS),
        VMSTATE_UINT64_ARRAY(rx_desc_cur, DWMAC4State, DWMAC4_NR_CHANNELS),
        VMSTATE_UINT16_ARRAY(phy_regs, DWMAC4State, 32),
        VMSTATE_UINT16(phy_page, DWMAC4State),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property dwmac4_properties[] = {
    DEFINE_NIC_PROPERTIES(DWMAC4State, conf),
    DEFINE_PROP_UINT8("snps-version", DWMAC4State, snps_version,
                      DWMAC4_DEFAULT_SNPS_VERSION),
    DEFINE_PROP_UINT8("user-version", DWMAC4State, user_version, 0),
    DEFINE_PROP_UINT8("dma-width", DWMAC4State, dma_width,
                      DWMAC4_DEFAULT_DMA_WIDTH),
    DEFINE_PROP_UINT8("phy-addr", DWMAC4State, phy_addr, 1),
    DEFINE_PROP_UINT16("phy-id1", DWMAC4State, phy_id1, 0x001c),
    DEFINE_PROP_UINT16("phy-id2", DWMAC4State, phy_id2, 0xc916),
    DEFINE_PROP_UINT32("tx-fifo-size", DWMAC4State, tx_fifo_size,
                       DWMAC4_DEFAULT_FIFO_SIZE),
    DEFINE_PROP_UINT32("rx-fifo-size", DWMAC4State, rx_fifo_size,
                       DWMAC4_DEFAULT_FIFO_SIZE),
    DEFINE_PROP_BOOL("tso", DWMAC4State, tso, false),
};

static void dwmac4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "Synopsys DesignWare MAC (GMAC4 / dwmac-4.20a)";
    dc->realize = dwmac4_realize;
    dc->unrealize = dwmac4_unrealize;
    device_class_set_legacy_reset(dc, dwmac4_reset);
    dc->vmsd = &vmstate_dwmac4;
    device_class_set_props(dc, dwmac4_properties);
}

static const TypeInfo dwmac4_types[] = {
    {
        .name = TYPE_DWMAC4,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(DWMAC4State),
        .class_init = dwmac4_class_init,
    },
};
DEFINE_TYPES(dwmac4_types)
