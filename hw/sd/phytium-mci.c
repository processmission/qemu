/*
 * Local-only Phytium Multimedia Card Interface controller model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sd/phytium-mci.h"
#include "exec/cpu-common.h"
#include "system/physmem.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"

REG32(CNTRL, 0x000)
REG32(PWREN, 0x004)
REG32(CLKDIV, 0x008)
REG32(CLKENA, 0x010)
REG32(TMOUT, 0x014)
REG32(CTYPE, 0x018)
REG32(BLKSIZ, 0x01c)
REG32(BYTCNT, 0x020)
REG32(INT_MASK, 0x024)
REG32(CMDARG, 0x028)
REG32(CMD, 0x02c)
REG32(RESP0, 0x030)
REG32(RESP1, 0x034)
REG32(RESP2, 0x038)
REG32(RESP3, 0x03c)
REG32(MASKED_INTS, 0x040)
REG32(RAW_INTS, 0x044)
REG32(STATUS, 0x048)
REG32(FIFOTH, 0x04c)
REG32(CARD_DETECT, 0x050)
REG32(CARD_WRTPRT, 0x054)
REG32(CCLK_RDY, 0x058)
REG32(TRAN_CARD_CNT, 0x05c)
REG32(TRAN_FIFO_CNT, 0x060)
REG32(DEBNCE, 0x064)
REG32(UID, 0x068)
REG32(VID, 0x06c)
REG32(HWCONF, 0x070)
REG32(UHS_REG, 0x074)
REG32(CARD_RESET, 0x078)
REG32(BUS_MODE, 0x080)
REG32(DESC_LIST_ADDRL, 0x088)
REG32(DESC_LIST_ADDRH, 0x08c)
REG32(DMAC_STATUS, 0x090)
REG32(DMAC_INT_ENA, 0x094)
REG32(CUR_DESC_ADDRL, 0x098)
REG32(CUR_DESC_ADDRH, 0x09c)
REG32(CUR_BUF_ADDRL, 0x0a0)
REG32(CUR_BUF_ADDRH, 0x0a4)
REG32(CARD_THRCTL, 0x100)
REG32(UHS_REG_EXT, 0x108)
REG32(EMMC_DDR_REG, 0x10c)
REG32(ENABLE_SHIFT, 0x110)
REG32(DATA, 0x200)
REG32(IRQ_ACK, 0xfd0)

#define CNTRL_CONTROLLER_RESET      BIT(0)
#define CNTRL_FIFO_RESET            BIT(1)
#define CNTRL_DMA_RESET             BIT(2)
#define CNTRL_INT_ENABLE            BIT(4)
#define CNTRL_DMA_ENABLE            BIT(5)
#define CNTRL_USE_INTERNAL_DMAC     BIT(25)
#define CNTRL_RESET_MASK            (CNTRL_CONTROLLER_RESET | \
                                     CNTRL_FIFO_RESET | \
                                     CNTRL_DMA_RESET)

#define CMD_START                   BIT(31)
#define CMD_UPD_CLK                 BIT(21)
#define CMD_SEND_STOP               BIT(12)
#define CMD_DAT_WR                  BIT(10)
#define CMD_DAT_EXP                 BIT(9)
#define CMD_RESP_LONG               BIT(7)
#define CMD_RESP_EXP                BIT(6)
#define CMD_INDEX_MASK              0x3f

#define INT_CD                      BIT(0)
#define INT_RE                      BIT(1)
#define INT_CMD                     BIT(2)
#define INT_DTO                     BIT(3)
#define INT_TXDR                    BIT(4)
#define INT_RXDR                    BIT(5)
#define INT_RCRC                    BIT(6)
#define INT_DCRC                    BIT(7)
#define INT_RTO                     BIT(8)
#define INT_DRTO                    BIT(9)
#define INT_HTO                     BIT(10)
#define INT_FRUN                    BIT(11)
#define INT_HLE                     BIT(12)
#define INT_SBE_BCI                 BIT(13)
#define INT_ACD                     BIT(14)
#define INT_EBE                     BIT(15)
#define INT_SDIO                    BIT(16)
#define INT_ALL                     (INT_CD | INT_RE | INT_CMD | INT_DTO | \
                                     INT_TXDR | INT_RXDR | INT_RCRC | \
                                     INT_DCRC | INT_RTO | INT_DRTO | \
                                     INT_HTO | INT_FRUN | INT_HLE | \
                                     INT_SBE_BCI | INT_ACD | INT_EBE | \
                                     INT_SDIO)

#define STATUS_FIFO_RX              BIT(0)
#define STATUS_FIFO_TX              BIT(1)
#define STATUS_FIFO_EMPTY           BIT(2)
#define STATUS_FIFO_FULL            BIT(3)
#define STATUS_DATA_BUSY            BIT(10)
#define STATUS_RESPONSE_INDEX_SHIFT 11

#define BUS_MODE_SWR                BIT(0)
#define BUS_MODE_DE                 BIT(7)

#define DMAC_STATUS_TI              BIT(0)
#define DMAC_STATUS_RI              BIT(1)
#define DMAC_STATUS_FBE             BIT(2)
#define DMAC_STATUS_DU              BIT(4)
#define DMAC_STATUS_NIS             BIT(8)
#define DMAC_STATUS_AIS             BIT(9)
#define DMAC_STATUS_ALL             (DMAC_STATUS_TI | DMAC_STATUS_RI | \
                                     DMAC_STATUS_FBE | DMAC_STATUS_DU | \
                                     DMAC_STATUS_NIS | DMAC_STATUS_AIS)

#define SD_SCR_SIZE                 8

static const uint8_t phytium_mci_scr[SD_SCR_SIZE] = {
    0x02, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define ADMA_ATTR_DIC               BIT(1)
#define ADMA_ATTR_LD                BIT(2)
#define ADMA_ATTR_FD                BIT(3)
#define ADMA_ATTR_CH                BIT(4)
#define ADMA_ATTR_ER                BIT(5)
#define ADMA_ATTR_CES               BIT(30)
#define ADMA_ATTR_OWN               BIT(31)
#define ADMA_DESC_SIZE              32

static void phytium_mci_update_irq(PhytiumMciState *s)
{
    bool raw_irq = (s->regs[R_RAW_INTS] & s->regs[R_INT_MASK]) != 0;
    bool dmac_irq = (s->regs[R_DMAC_STATUS] & s->regs[R_DMAC_INT_ENA]) != 0;
    bool raise = (s->regs[R_CNTRL] & CNTRL_INT_ENABLE) &&
                 (raw_irq || dmac_irq);

    qemu_set_irq(s->irq, raise);
}

static void phytium_mci_fifo_reset(PhytiumMciState *s)
{
    s->fifo_len = 0;
    s->fifo_pos = 0;
    memset(s->fifo, 0, sizeof(s->fifo));
}

static uint32_t phytium_mci_fifo_pop_le32(PhytiumMciState *s)
{
    uint32_t value;

    if (s->fifo_len < 4) {
        return 0;
    }

    value = ldl_le_p(s->fifo + s->fifo_pos);
    s->fifo_pos += 4;
    s->fifo_len -= 4;
    if (s->fifo_len == 0 || s->fifo_pos >= sizeof(s->fifo)) {
        s->fifo_pos = 0;
    }

    return value;
}

static void phytium_mci_maybe_send_stop(PhytiumMciState *s)
{
    SDRequest req = {
        .cmd = 12,
        .arg = 0,
    };
    uint8_t response[16];

    if (!s->transfer_send_stop) {
        return;
    }

    s->transfer_send_stop = false;
    sdbus_do_command(&s->sdbus, &req, response, sizeof(response));
    s->regs[R_RAW_INTS] |= INT_ACD;
}

static void phytium_mci_pio_refill(PhytiumMciState *s)
{
    uint32_t count;
    uint32_t pos;

    if (!s->transfer_active || s->transfer_is_write ||
        s->transfer_bytes_remaining == 0 || s->fifo_len != 0) {
        return;
    }

    count = MIN(s->transfer_bytes_remaining,
                (uint32_t)(sizeof(s->fifo) - s->fifo_len));
    if (count == 0) {
        return;
    }

    memset(s->fifo + s->fifo_len, 0, count);
    if (s->transfer_synthetic_scr) {
        pos = SD_SCR_SIZE - s->transfer_bytes_remaining;
        memcpy(s->fifo + s->fifo_len, phytium_mci_scr + pos, count);
    } else {
        sdbus_read_data(&s->sdbus, s->fifo + s->fifo_len, count);
    }
    s->fifo_len += count;
    s->transfer_bytes_remaining -= count;
    s->regs[R_RAW_INTS] |= INT_RXDR;

    if (s->transfer_bytes_remaining == 0) {
        s->transfer_synthetic_scr = false;
        phytium_mci_maybe_send_stop(s);
        s->regs[R_RAW_INTS] |= INT_DTO;
    }

    phytium_mci_update_irq(s);
}

static uint32_t phytium_mci_read_data_port(PhytiumMciState *s)
{
    uint32_t value = 0;

    if (s->transfer_active && !s->transfer_is_write && s->fifo_len >= 4) {
        value = phytium_mci_fifo_pop_le32(s);
        if (s->fifo_len == 0) {
            if (s->transfer_bytes_remaining != 0) {
                phytium_mci_pio_refill(s);
            } else {
                phytium_mci_maybe_send_stop(s);
                s->regs[R_RAW_INTS] |= INT_DTO;
                s->transfer_active = false;
                phytium_mci_update_irq(s);
            }
        }
    }

    return value;
}

static void phytium_mci_write_data_port(PhytiumMciState *s, uint32_t value)
{
    uint8_t buf[4];

    if (!s->transfer_active || !s->transfer_is_write) {
        return;
    }

    stl_le_p(buf, value);
    sdbus_write_data(&s->sdbus, buf, sizeof(buf));

    if (s->transfer_bytes_remaining > sizeof(buf)) {
        s->transfer_bytes_remaining -= sizeof(buf);
        s->regs[R_RAW_INTS] |= INT_TXDR;
    } else {
        s->transfer_bytes_remaining = 0;
        s->transfer_active = false;
        phytium_mci_maybe_send_stop(s);
        s->regs[R_RAW_INTS] |= INT_DTO;
    }

    phytium_mci_update_irq(s);
}

static bool phytium_mci_idmac_enabled(PhytiumMciState *s)
{
    return (s->regs[R_CNTRL] & CNTRL_USE_INTERNAL_DMAC) &&
           (s->regs[R_BUS_MODE] & BUS_MODE_DE);
}

static void phytium_mci_adma_kick(PhytiumMciState *s)
{
    uint64_t desc_addr = deposit64(s->regs[R_DESC_LIST_ADDRL], 32, 32,
                                   s->regs[R_DESC_LIST_ADDRH]) & ~3ULL;
    bool is_write = s->transfer_is_write;
    uint32_t processed = 0;

    while (desc_addr && processed++ < PHYTIUM_MCI_ADMA_MAX_DESCS) {
        uint8_t desc[ADMA_DESC_SIZE];
        uint32_t attr;
        uint32_t len;
        uint64_t buf_addr;
        uint64_t next_addr;
        uint32_t count;

        physical_memory_read(desc_addr, desc, sizeof(desc));
        attr = ldl_le_p(desc + 0);
        len = ldl_le_p(desc + 8);
        buf_addr = deposit64(ldl_le_p(desc + 16), 32, 32,
                             ldl_le_p(desc + 20));
        next_addr = deposit64(ldl_le_p(desc + 24), 32, 32,
                              ldl_le_p(desc + 28)) & ~3ULL;

        if (!(attr & ADMA_ATTR_OWN)) {
            break;
        }
        if (s->transfer_bytes_remaining == 0) {
            break;
        }

        count = len ? len : s->transfer_bytes_remaining;
        count = MIN(count, s->transfer_bytes_remaining);

        s->regs[R_CUR_DESC_ADDRL] = (uint32_t)desc_addr;
        s->regs[R_CUR_DESC_ADDRH] = (uint32_t)(desc_addr >> 32);
        s->regs[R_CUR_BUF_ADDRL] = (uint32_t)buf_addr;
        s->regs[R_CUR_BUF_ADDRH] = (uint32_t)(buf_addr >> 32);

        if (count && buf_addr) {
            g_autofree uint8_t *buf = g_malloc0(count);

            if (is_write) {
                physical_memory_read(buf_addr, buf, count);
                sdbus_write_data(&s->sdbus, buf, count);
            } else if (s->transfer_synthetic_scr) {
                uint32_t pos = SD_SCR_SIZE - s->transfer_bytes_remaining;

                memcpy(buf, phytium_mci_scr + pos, count);
                physical_memory_write(buf_addr, buf, count);
            } else {
                sdbus_read_data(&s->sdbus, buf, count);
                physical_memory_write(buf_addr, buf, count);
            }
            s->transfer_bytes_remaining -= count;
        }

        attr &= ~ADMA_ATTR_OWN;
        stl_le_p(desc + 0, attr);
        physical_memory_write(desc_addr, desc, sizeof(desc));

        if ((attr & ADMA_ATTR_LD) || s->transfer_bytes_remaining == 0) {
            break;
        }
        if ((attr & ADMA_ATTR_CH) && next_addr) {
            desc_addr = next_addr;
        } else {
            desc_addr += ADMA_DESC_SIZE;
        }
    }

    s->regs[R_DMAC_STATUS] |= is_write ? DMAC_STATUS_TI : DMAC_STATUS_RI;
    s->regs[R_DMAC_STATUS] |= DMAC_STATUS_NIS;
    s->regs[R_RAW_INTS] |= INT_DTO;
    s->transfer_active = false;
    s->transfer_bytes_remaining = 0;
    s->transfer_synthetic_scr = false;
    phytium_mci_maybe_send_stop(s);
    phytium_mci_update_irq(s);
}

static uint32_t phytium_mci_data_length(PhytiumMciState *s, uint8_t cmd,
                                        bool is_write)
{
    uint32_t length = s->regs[R_BYTCNT];

    if (!is_write && cmd == 51) {
        length = MIN(length, (uint32_t)SD_SCR_SIZE);
    }

    return length;
}

static void phytium_mci_issue_command(PhytiumMciState *s)
{
    SDRequest req;
    uint8_t response[16] = {};
    uint32_t cmd = s->regs[R_CMD];
    bool expect_resp = cmd & CMD_RESP_EXP;
    bool long_resp = cmd & CMD_RESP_LONG;
    bool data_expected = cmd & CMD_DAT_EXP;
    bool data_write = cmd & CMD_DAT_WR;
    bool cmd_ok = true;
    bool synthetic_scr = false;
    uint8_t prev_cmd = s->last_cmd;
    size_t rlen = 0;

    if (cmd & CMD_UPD_CLK) {
        s->regs[R_RAW_INTS] |= INT_CMD;
        s->regs[R_CMD] &= ~CMD_START;
        phytium_mci_update_irq(s);
        return;
    }

    req.cmd = cmd & CMD_INDEX_MASK;
    req.arg = s->regs[R_CMDARG];
    s->last_cmd = req.cmd;

    if (data_expected && expect_resp && req.cmd == 51 && prev_cmd != 55) {
        stl_be_p(response, 0);
        rlen = sizeof(uint32_t);
        synthetic_scr = true;
    } else {
        rlen = sdbus_do_command(&s->sdbus, &req, response, sizeof(response));
    }
    if (rlen == 0 && data_expected && expect_resp &&
        (req.cmd == 18 || req.cmd == 25)) {
        rlen = sdbus_do_command(&s->sdbus, &req, response, sizeof(response));
    }
    if (rlen == 0 && data_expected && expect_resp && req.cmd == 51) {
        stl_be_p(response, 0);
        rlen = sizeof(uint32_t);
        synthetic_scr = true;
    }

    if (expect_resp) {
        if (rlen == 0) {
            s->regs[R_RAW_INTS] |= INT_RTO;
            cmd_ok = false;
        } else if (long_resp && rlen == 16) {
            s->regs[R_RESP0] = ldl_be_p(response + 12) & ~1u;
            s->regs[R_RESP1] = ldl_be_p(response + 8);
            s->regs[R_RESP2] = ldl_be_p(response + 4);
            s->regs[R_RESP3] = ldl_be_p(response + 0);
            s->regs[R_RAW_INTS] |= INT_CMD;
        } else if (rlen >= 4) {
            s->regs[R_RESP0] = ldl_be_p(response);
            s->regs[R_RESP1] = 0;
            s->regs[R_RESP2] = 0;
            s->regs[R_RESP3] = 0;
            s->regs[R_RAW_INTS] |= INT_CMD;
        } else {
            s->regs[R_RAW_INTS] |= INT_RE;
            cmd_ok = false;
        }
    } else {
        s->regs[R_RAW_INTS] |= INT_CMD;
    }

    if (cmd_ok && data_expected) {
        s->transfer_bytes_remaining =
            phytium_mci_data_length(s, req.cmd, data_write);
        s->transfer_is_write = data_write;
        s->transfer_send_stop = cmd & CMD_SEND_STOP;
        s->transfer_synthetic_scr = synthetic_scr;
        s->transfer_active = s->transfer_bytes_remaining != 0;

        if (phytium_mci_idmac_enabled(s)) {
            phytium_mci_adma_kick(s);
        } else if (data_write) {
            s->regs[R_RAW_INTS] |= INT_TXDR;
        } else {
            phytium_mci_pio_refill(s);
        }
    }

    s->regs[R_CMD] &= ~CMD_START;
    phytium_mci_update_irq(s);
}

static uint64_t phytium_mci_cntrl_pre_write(RegisterInfo *reg, uint64_t val)
{
    PhytiumMciState *s = PHYTIUM_MCI(reg->opaque);

    if (val & CNTRL_FIFO_RESET) {
        phytium_mci_fifo_reset(s);
    }
    if (val & CNTRL_DMA_RESET) {
        s->regs[R_DMAC_STATUS] = 0;
    }
    if (val & CNTRL_CONTROLLER_RESET) {
        s->transfer_active = false;
        s->transfer_bytes_remaining = 0;
        s->transfer_send_stop = false;
        s->transfer_synthetic_scr = false;
    }

    return val & ~CNTRL_RESET_MASK;
}

static void phytium_mci_irq_post_write(RegisterInfo *reg, uint64_t val)
{
    phytium_mci_update_irq(PHYTIUM_MCI(reg->opaque));
}

static void phytium_mci_cmd_post_write(RegisterInfo *reg, uint64_t val)
{
    PhytiumMciState *s = PHYTIUM_MCI(reg->opaque);

    if (val & CMD_START) {
        phytium_mci_issue_command(s);
    } else {
        phytium_mci_update_irq(s);
    }
}

static uint64_t phytium_mci_bus_mode_pre_write(RegisterInfo *reg, uint64_t val)
{
    return val & ~BUS_MODE_SWR;
}

static uint64_t phytium_mci_masked_post_read(RegisterInfo *reg, uint64_t val)
{
    PhytiumMciState *s = PHYTIUM_MCI(reg->opaque);

    return s->regs[R_RAW_INTS] & s->regs[R_INT_MASK];
}

static uint64_t phytium_mci_status_post_read(RegisterInfo *reg, uint64_t val)
{
    PhytiumMciState *s = PHYTIUM_MCI(reg->opaque);
    uint32_t status = (s->last_cmd & CMD_INDEX_MASK) <<
                      STATUS_RESPONSE_INDEX_SHIFT;

    if (s->fifo_len == 0) {
        status |= STATUS_FIFO_EMPTY | STATUS_FIFO_TX;
    }
    if (s->fifo_len == sizeof(s->fifo)) {
        status |= STATUS_FIFO_FULL;
    }
    if (s->fifo_len != 0) {
        status |= STATUS_FIFO_RX;
    }
    if (s->transfer_active) {
        status |= STATUS_DATA_BUSY;
    }

    return status;
}

static uint64_t phytium_mci_card_detect_post_read(RegisterInfo *reg,
                                                  uint64_t val)
{
    PhytiumMciState *s = PHYTIUM_MCI(reg->opaque);

    return sdbus_get_inserted(&s->sdbus) ? 0 : 1;
}

static uint64_t phytium_mci_wrtprt_post_read(RegisterInfo *reg, uint64_t val)
{
    PhytiumMciState *s = PHYTIUM_MCI(reg->opaque);

    return sdbus_get_readonly(&s->sdbus) ? 1 : 0;
}

static uint64_t phytium_mci_cclk_ready_post_read(RegisterInfo *reg,
                                                 uint64_t val)
{
    return 1;
}

static uint64_t phytium_mci_read(void *opaque, hwaddr addr, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    PhytiumMciState *s = PHYTIUM_MCI(register_array_get_owner(reg_array));

    if (addr == A_DATA) {
        return phytium_mci_read_data_port(s);
    }

    return register_read_memory(opaque, addr, size);
}

static void phytium_mci_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    PhytiumMciState *s = PHYTIUM_MCI(register_array_get_owner(reg_array));

    if (addr == A_DATA) {
        phytium_mci_write_data_port(s, value);
        return;
    }

    register_write_memory(opaque, addr, value, size);
}

static const MemoryRegionOps phytium_mci_ops = {
    .read = phytium_mci_read,
    .write = phytium_mci_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const RegisterAccessInfo phytium_mci_regs_info[] = {
    { .name = "CNTRL", .addr = A_CNTRL,
      .pre_write = phytium_mci_cntrl_pre_write,
      .post_write = phytium_mci_irq_post_write },
    { .name = "PWREN", .addr = A_PWREN },
    { .name = "CLKDIV", .addr = A_CLKDIV },
    { .name = "CLKENA", .addr = A_CLKENA },
    { .name = "TMOUT", .addr = A_TMOUT, .reset = 0xffffffff },
    { .name = "CTYPE", .addr = A_CTYPE },
    { .name = "BLKSIZ", .addr = A_BLKSIZ },
    { .name = "BYTCNT", .addr = A_BYTCNT },
    { .name = "INT_MASK", .addr = A_INT_MASK,
      .post_write = phytium_mci_irq_post_write },
    { .name = "CMDARG", .addr = A_CMDARG },
    { .name = "CMD", .addr = A_CMD, .post_write = phytium_mci_cmd_post_write },
    { .name = "RESP0", .addr = A_RESP0, .ro = UINT32_MAX },
    { .name = "RESP1", .addr = A_RESP1, .ro = UINT32_MAX },
    { .name = "RESP2", .addr = A_RESP2, .ro = UINT32_MAX },
    { .name = "RESP3", .addr = A_RESP3, .ro = UINT32_MAX },
    { .name = "MASKED_INTS", .addr = A_MASKED_INTS, .ro = UINT32_MAX,
      .post_read = phytium_mci_masked_post_read },
    { .name = "RAW_INTS", .addr = A_RAW_INTS, .w1c = INT_ALL,
      .post_write = phytium_mci_irq_post_write },
    { .name = "STATUS", .addr = A_STATUS, .ro = UINT32_MAX,
      .post_read = phytium_mci_status_post_read },
    { .name = "FIFOTH", .addr = A_FIFOTH },
    { .name = "CARD_DETECT", .addr = A_CARD_DETECT, .ro = UINT32_MAX,
      .post_read = phytium_mci_card_detect_post_read },
    { .name = "CARD_WRTPRT", .addr = A_CARD_WRTPRT, .ro = UINT32_MAX,
      .post_read = phytium_mci_wrtprt_post_read },
    { .name = "CCLK_RDY", .addr = A_CCLK_RDY, .ro = UINT32_MAX,
      .post_read = phytium_mci_cclk_ready_post_read },
    { .name = "TRAN_CARD_CNT", .addr = A_TRAN_CARD_CNT, .ro = UINT32_MAX },
    { .name = "TRAN_FIFO_CNT", .addr = A_TRAN_FIFO_CNT, .ro = UINT32_MAX },
    { .name = "DEBNCE", .addr = A_DEBNCE },
    { .name = "UID", .addr = A_UID },
    { .name = "VID", .addr = A_VID, .ro = UINT32_MAX },
    { .name = "HWCONF", .addr = A_HWCONF, .ro = UINT32_MAX },
    { .name = "UHS_REG", .addr = A_UHS_REG },
    { .name = "CARD_RESET", .addr = A_CARD_RESET, .reset = 1 },
    { .name = "BUS_MODE", .addr = A_BUS_MODE,
      .pre_write = phytium_mci_bus_mode_pre_write },
    { .name = "DESC_LIST_ADDRL", .addr = A_DESC_LIST_ADDRL },
    { .name = "DESC_LIST_ADDRH", .addr = A_DESC_LIST_ADDRH },
    { .name = "DMAC_STATUS", .addr = A_DMAC_STATUS,
      .w1c = DMAC_STATUS_ALL, .post_write = phytium_mci_irq_post_write },
    { .name = "DMAC_INT_ENA", .addr = A_DMAC_INT_ENA,
      .post_write = phytium_mci_irq_post_write },
    { .name = "CUR_DESC_ADDRL", .addr = A_CUR_DESC_ADDRL, .ro = UINT32_MAX },
    { .name = "CUR_DESC_ADDRH", .addr = A_CUR_DESC_ADDRH, .ro = UINT32_MAX },
    { .name = "CUR_BUF_ADDRL", .addr = A_CUR_BUF_ADDRL, .ro = UINT32_MAX },
    { .name = "CUR_BUF_ADDRH", .addr = A_CUR_BUF_ADDRH, .ro = UINT32_MAX },
    { .name = "CARD_THRCTL", .addr = A_CARD_THRCTL },
    { .name = "UHS_REG_EXT", .addr = A_UHS_REG_EXT },
    { .name = "EMMC_DDR_REG", .addr = A_EMMC_DDR_REG },
    { .name = "ENABLE_SHIFT", .addr = A_ENABLE_SHIFT },
    { .name = "DATA", .addr = A_DATA },
    { .name = "IRQ_ACK", .addr = A_IRQ_ACK },
};

static void phytium_mci_set_inserted(DeviceState *dev, bool inserted)
{
    PhytiumMciState *s = PHYTIUM_MCI(dev);

    if (inserted) {
        s->regs[R_RAW_INTS] &= ~INT_CD;
    } else {
        s->regs[R_RAW_INTS] |= INT_CD;
    }
    phytium_mci_update_irq(s);
}

static void phytium_mci_set_readonly(DeviceState *dev, bool readonly)
{
}

static void phytium_mci_reset(DeviceState *dev)
{
    PhytiumMciState *s = PHYTIUM_MCI(dev);

    for (unsigned int i = 0; i < PHYTIUM_MCI_REG_WORDS; i++) {
        register_reset(&s->regs_info[i]);
    }

    phytium_mci_fifo_reset(s);
    s->transfer_bytes_remaining = 0;
    s->transfer_is_write = false;
    s->transfer_send_stop = false;
    s->transfer_synthetic_scr = false;
    s->transfer_active = false;
    s->last_cmd = 0;
    phytium_mci_update_irq(s);
}

static const VMStateDescription vmstate_phytium_mci = {
    .name = TYPE_PHYTIUM_MCI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, PhytiumMciState, PHYTIUM_MCI_REG_WORDS),
        VMSTATE_UINT8_ARRAY(fifo, PhytiumMciState,
                            PHYTIUM_MCI_FIFO_DEPTH * 4),
        VMSTATE_UINT32(fifo_len, PhytiumMciState),
        VMSTATE_UINT32(fifo_pos, PhytiumMciState),
        VMSTATE_UINT32(transfer_bytes_remaining, PhytiumMciState),
        VMSTATE_BOOL(transfer_is_write, PhytiumMciState),
        VMSTATE_BOOL(transfer_send_stop, PhytiumMciState),
        VMSTATE_BOOL(transfer_synthetic_scr, PhytiumMciState),
        VMSTATE_BOOL(transfer_active, PhytiumMciState),
        VMSTATE_UINT8(last_cmd, PhytiumMciState),
        VMSTATE_END_OF_LIST()
    },
};

static void phytium_mci_init(Object *obj)
{
    PhytiumMciState *s = PHYTIUM_MCI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(DEVICE(obj), phytium_mci_regs_info,
                                         ARRAY_SIZE(phytium_mci_regs_info),
                                         s->regs_info, s->regs,
                                         &phytium_mci_ops, false,
                                         PHYTIUM_MCI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_PHYTIUM_MCI_BUS, DEVICE(s),
              "sd-bus");
}

static void phytium_mci_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, phytium_mci_reset);
    dc->vmsd = &vmstate_phytium_mci;
    dc->user_creatable = false;
}

static void phytium_mci_bus_class_init(ObjectClass *oc, const void *data)
{
    SDBusClass *sbc = SD_BUS_CLASS(oc);

    sbc->set_inserted = phytium_mci_set_inserted;
    sbc->set_readonly = phytium_mci_set_readonly;
}

static const TypeInfo phytium_mci_types[] = {
    {
        .name = TYPE_PHYTIUM_MCI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PhytiumMciState),
        .instance_init = phytium_mci_init,
        .class_init = phytium_mci_class_init,
    },
    {
        .name = TYPE_PHYTIUM_MCI_BUS,
        .parent = TYPE_SD_BUS,
        .instance_size = sizeof(SDBus),
        .class_init = phytium_mci_bus_class_init,
    },
};

DEFINE_TYPES(phytium_mci_types)
