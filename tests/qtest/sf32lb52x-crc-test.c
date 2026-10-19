/*
 * QTest for the SiFli SF32LB52X CRC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Falsifiable coverage of the CRC register behavior (SVD "SF32LB52x v1.0"
 * CRC1 peripheral, vendor HAL bf0_hal_crc.c and Zephyr
 * drivers/crc/crc_sf32lb.c):
 *
 *  - reset values of DR/SR/CR/RSVD1/INIT/POL;
 *  - RW readback, reserved-bit masking and CR.RESET self-clearing;
 *  - CR.RESET loads the datapath from INIT;
 *  - SR.DONE handshake after a DR write and read-only SR semantics;
 *  - CRC-7/8/16/32 results with and without reflection, checked against the
 *    published check values for the byte string "123456789";
 *  - DR.DATASIZE 1..4-byte writes, mixed word/byte feeding and low-byte
 *    masking;
 *  - unimplemented offsets and unsupported access sizes are ignored.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define CRC_BASE 0x50048000

enum {
    CRC_DR    = 0x00,
    CRC_SR    = 0x04,
    CRC_CR    = 0x08,
    CRC_RSVD1 = 0x0c,
    CRC_INIT  = 0x10,
    CRC_POL   = 0x14,
};

/* CR fields (SVD CRC_CR) */
#define CR_RESET          (1U << 0)
#define CR_DATASIZE(n)    (((n) & 0x3U) << 1)  /* n = valid bytes - 1 */
#define CR_POLYSIZE(p)    (((p) & 0x3U) << 3)
#define CR_REV_IN(v)      (((v) & 0x3U) << 5)
#define CR_REV_OUT        (1U << 7)
#define CR_WRITABLE_MASK  0x000000ffU

/* SR fields (SVD CRC_SR) */
#define SR_DONE           (1U << 0)
#define SR_OVERFLOW       (1U << 1)

#define POLYSIZE_32 0U
#define POLYSIZE_16 1U
#define POLYSIZE_8  2U
#define POLYSIZE_7  3U

#define REV_IN_BYTE 1U

/* Published CRC check values are defined for this byte string. */
static const uint8_t crc_test_data[] = "123456789";

static QTestState *crc_qtest_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static uint32_t crc_read(QTestState *qts, unsigned int reg)
{
    return qtest_readl(qts, CRC_BASE + reg);
}

static void crc_write(QTestState *qts, unsigned int reg, uint32_t value)
{
    qtest_writel(qts, CRC_BASE + reg, value);
}

/*
 * Program INIT/POL/CR and reset the datapath exactly like
 * crc_sf32lb_begin(): write CR, INIT, POL, then CR|RESET followed by CR.
 * Returns the CR value to use for data writes.
 */
static uint32_t crc_begin(QTestState *qts, unsigned int polysize, bool rev_in,
                          bool rev_out, uint32_t init, uint32_t poly)
{
    uint32_t cr = CR_POLYSIZE(polysize) |
                  (rev_in ? CR_REV_IN(REV_IN_BYTE) : 0) |
                  (rev_out ? CR_REV_OUT : 0);

    crc_write(qts, CRC_INIT, init);
    crc_write(qts, CRC_POL, poly);
    crc_write(qts, CRC_CR, cr);
    crc_write(qts, CRC_CR, cr | CR_RESET);
    crc_write(qts, CRC_CR, cr);

    return cr;
}

/* Little-endian word as the CRC DR expects (driver uses sys_get_le32). */
static uint32_t crc_word(const uint8_t *data, size_t nbytes)
{
    uint32_t word = 0;
    size_t i;

    for (i = 0; i < nbytes; i++) {
        word |= (uint32_t)data[i] << (8 * i);
    }

    return word;
}

/*
 * Feed a buffer the way crc_sf32lb_update() does: full 32-bit words with
 * DATASIZE=3, then a partial word with DATASIZE=len-1. Returns DR and
 * optionally the SR value after the last write.
 */
static uint32_t crc_update(QTestState *qts, uint32_t cr,
                           const uint8_t *data, size_t len,
                           uint32_t *sr)
{
    size_t off = 0;

    while (off + 4 <= len) {
        crc_write(qts, CRC_CR, cr | CR_DATASIZE(3));
        crc_write(qts, CRC_DR, crc_word(data + off, 4));
        g_assert_cmphex(crc_read(qts, CRC_SR) & SR_DONE, ==, SR_DONE);
        off += 4;
    }

    if (off < len) {
        size_t rem = len - off;

        crc_write(qts, CRC_CR, cr | CR_DATASIZE(rem - 1));
        crc_write(qts, CRC_DR, crc_word(data + off, rem));
        g_assert_cmphex(crc_read(qts, CRC_SR) & SR_DONE, ==, SR_DONE);
    }

    if (sr) {
        *sr = crc_read(qts, CRC_SR);
    }

    return crc_read(qts, CRC_DR);
}

static void test_reset(void)
{
    QTestState *qts = crc_qtest_start();

    g_assert_cmphex(crc_read(qts, CRC_DR), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_SR), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_CR), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_RSVD1), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_INIT), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_POL), ==, 0);

    qtest_quit(qts);
}

static void test_register_access(void)
{
    QTestState *qts = crc_qtest_start();

    crc_write(qts, CRC_INIT, 0xdeadbeef);
    g_assert_cmphex(crc_read(qts, CRC_INIT), ==, 0xdeadbeef);
    crc_write(qts, CRC_POL, 0x04c11db7);
    g_assert_cmphex(crc_read(qts, CRC_POL), ==, 0x04c11db7);

    /*
     * Only CR[7:0] is defined; reserved bits are masked out and CR.RESET is
     * self-clearing by hardware.
     */
    crc_write(qts, CRC_CR, 0xffffffff);
    g_assert_cmphex(crc_read(qts, CRC_CR), ==,
                    CR_WRITABLE_MASK & ~CR_RESET);

    /* RSVD1 is reserved: writes are ignored and reads return zero. */
    crc_write(qts, CRC_RSVD1, 0xffffffff);
    g_assert_cmphex(crc_read(qts, CRC_RSVD1), ==, 0);

    /* SR reports hardware state only; writes have no effect. */
    crc_write(qts, CRC_SR, 0xffffffff);
    g_assert_cmphex(crc_read(qts, CRC_SR), ==, 0);

    qtest_quit(qts);
}

static void test_reset_loads_init(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr = CR_POLYSIZE(POLYSIZE_32);

    crc_write(qts, CRC_INIT, 0x1a2b3c4d);
    crc_write(qts, CRC_CR, cr);
    crc_write(qts, CRC_CR, cr | CR_RESET);

    g_assert_cmphex(crc_read(qts, CRC_CR) & CR_RESET, ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_DR), ==, 0x1a2b3c4d);

    qtest_quit(qts);
}

/* CRC-32/ISO-HDLC: poly 0x04c11db7, init/xorout 0xffffffff, reflected. */
static void test_crc32_ieee(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t sr;
    uint32_t dr;
    uint32_t cr;

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    dr = crc_update(qts, cr, crc_test_data, sizeof(crc_test_data) - 1, &sr);

    g_assert_cmphex(sr, ==, SR_DONE);
    g_assert_cmphex(dr ^ 0xffffffffU, ==, 0xcbf43926U);

    qtest_quit(qts);
}

/*
 * The same CRC-32 must result from byte-wise DR writes, from 2/3/4-byte
 * writes, and from the plain word path used by the driver.
 */
static void test_crc32_datasize(void)
{
    QTestState *qts = crc_qtest_start();
    const uint8_t *data = crc_test_data;
    size_t len = sizeof(crc_test_data) - 1;
    uint32_t cr;
    uint32_t dr;
    size_t i;

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    for (i = 0; i < len; i++) {
        crc_write(qts, CRC_CR, cr | CR_DATASIZE(0));
        crc_write(qts, CRC_DR, data[i]);
        g_assert_cmphex(crc_read(qts, CRC_SR) & SR_DONE, ==, SR_DONE);
    }
    dr = crc_read(qts, CRC_DR);
    g_assert_cmphex(dr ^ 0xffffffffU, ==, 0xcbf43926U);

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    crc_write(qts, CRC_CR, cr | CR_DATASIZE(1));
    crc_write(qts, CRC_DR, crc_word(data, 2));
    crc_write(qts, CRC_CR, cr | CR_DATASIZE(2));
    crc_write(qts, CRC_DR, crc_word(data + 2, 3));
    crc_write(qts, CRC_CR, cr | CR_DATASIZE(3));
    crc_write(qts, CRC_DR, crc_word(data + 5, 4));
    dr = crc_read(qts, CRC_DR);
    g_assert_cmphex(dr ^ 0xffffffffU, ==, 0xcbf43926U);

    qtest_quit(qts);
}

/* CRC-32/C (Castagnoli): reflected, init/xorout 0xffffffff. */
static void test_crc32_c(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t dr;

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x1edc6f41);
    dr = crc_update(qts, cr, crc_test_data, sizeof(crc_test_data) - 1, NULL);

    g_assert_cmphex(dr ^ 0xffffffffU, ==, 0xe3069283U);

    qtest_quit(qts);
}

/* CRC-16/ARC: poly 0x8005, init 0, reflected. */
static void test_crc16_arc(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t dr;

    cr = crc_begin(qts, POLYSIZE_16, true, true, 0x0000, 0x8005);
    dr = crc_update(qts, cr, crc_test_data, sizeof(crc_test_data) - 1, NULL);

    g_assert_cmphex(dr, ==, 0xbb3d);

    qtest_quit(qts);
}

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, not reflected. */
static void test_crc16_ccitt_false(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t dr;

    cr = crc_begin(qts, POLYSIZE_16, false, false, 0xffff, 0x1021);
    dr = crc_update(qts, cr, crc_test_data, sizeof(crc_test_data) - 1, NULL);

    g_assert_cmphex(dr, ==, 0x29b1);

    qtest_quit(qts);
}

/* CRC-8: poly 0x07, init 0, not reflected. */
static void test_crc8(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t dr;

    cr = crc_begin(qts, POLYSIZE_8, false, false, 0x00, 0x07);
    dr = crc_update(qts, cr, crc_test_data, sizeof(crc_test_data) - 1, NULL);

    g_assert_cmphex(dr, ==, 0xf4);

    qtest_quit(qts);
}

/* CRC-8/ROHC: poly 0x07, init 0xff, reflected. */
static void test_crc8_rohc(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t dr;

    cr = crc_begin(qts, POLYSIZE_8, true, true, 0xff, 0x07);
    dr = crc_update(qts, cr, crc_test_data, sizeof(crc_test_data) - 1, NULL);

    g_assert_cmphex(dr, ==, 0xd0);

    qtest_quit(qts);
}

/* CRC-7/MMC: 7-bit polynomial size, poly 0x09, init 0. */
static void test_crc7_mmc(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t dr;

    cr = crc_begin(qts, POLYSIZE_7, false, false, 0x00, 0x09);
    dr = crc_update(qts, cr, crc_test_data, sizeof(crc_test_data) - 1, NULL);

    g_assert_cmphex(dr, ==, 0x75);

    qtest_quit(qts);
}

/* Only the low DATASIZE bytes of a DR write may reach the engine. */
static void test_datasize_masking(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t a;
    uint32_t b;

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    crc_write(qts, CRC_CR, cr | CR_DATASIZE(0));
    crc_write(qts, CRC_DR, 0xabcd);
    a = crc_read(qts, CRC_DR);

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    crc_write(qts, CRC_CR, cr | CR_DATASIZE(0));
    crc_write(qts, CRC_DR, 0x000000cd);
    b = crc_read(qts, CRC_DR);
    g_assert_cmphex(a, ==, b);

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    crc_write(qts, CRC_CR, cr | CR_DATASIZE(1));
    crc_write(qts, CRC_DR, 0xffffabcd);
    a = crc_read(qts, CRC_DR);

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    crc_write(qts, CRC_CR, cr | CR_DATASIZE(1));
    crc_write(qts, CRC_DR, 0x0000abcd);
    b = crc_read(qts, CRC_DR);
    g_assert_cmphex(a, ==, b);

    qtest_quit(qts);
}

static void test_done_status(void)
{
    QTestState *qts = crc_qtest_start();
    uint32_t cr;
    uint32_t sr;

    cr = crc_begin(qts, POLYSIZE_32, true, true, 0xffffffff, 0x04c11db7);
    g_assert_cmphex(crc_read(qts, CRC_SR), ==, 0);

    crc_write(qts, CRC_CR, cr | CR_DATASIZE(3));
    crc_write(qts, CRC_DR, 0x31323334);
    sr = crc_read(qts, CRC_SR);
    g_assert_cmphex(sr & SR_DONE, ==, SR_DONE);
    g_assert_cmphex(sr & SR_OVERFLOW, ==, 0);

    /* SR is not software-writable. */
    crc_write(qts, CRC_SR, SR_DONE | SR_OVERFLOW);
    g_assert_cmphex(crc_read(qts, CRC_SR), ==, SR_DONE);

    /* A device reset returns the whole block to its documented reset state. */
    qtest_system_reset(qts);
    g_assert_cmphex(crc_read(qts, CRC_DR), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_SR), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_CR), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_INIT), ==, 0);
    g_assert_cmphex(crc_read(qts, CRC_POL), ==, 0);

    qtest_quit(qts);
}

static void test_invalid_access(void)
{
    QTestState *qts = crc_qtest_start();

    /*
     * Registers are 32-bit; narrower or unaligned writes are rejected by the
     * memory region and must not modify the register.
     */
    crc_write(qts, CRC_INIT, 0x12345678);
    qtest_writeb(qts, CRC_BASE + CRC_INIT, 0xff);
    g_assert_cmphex(crc_read(qts, CRC_INIT), ==, 0x12345678);
    qtest_writew(qts, CRC_BASE + CRC_INIT, 0xffff);
    g_assert_cmphex(crc_read(qts, CRC_INIT), ==, 0x12345678);
    qtest_writel(qts, CRC_BASE + CRC_INIT + 2, 0xffffffff);
    g_assert_cmphex(crc_read(qts, CRC_INIT), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_unimplemented_offset(void)
{
    QTestState *qts = crc_qtest_start();

    g_assert_cmphex(crc_read(qts, 0x18), ==, 0);
    g_assert_cmphex(crc_read(qts, 0xffc), ==, 0);
    crc_write(qts, 0x18, 0xdeadbeef);
    g_assert_cmphex(crc_read(qts, 0x18), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/crc/reset", test_reset);
    qtest_add_func("/sf32lb52x/crc/registers", test_register_access);
    qtest_add_func("/sf32lb52x/crc/reset-loads-init", test_reset_loads_init);
    qtest_add_func("/sf32lb52x/crc/crc32-ieee", test_crc32_ieee);
    qtest_add_func("/sf32lb52x/crc/crc32-datasize", test_crc32_datasize);
    qtest_add_func("/sf32lb52x/crc/crc32-c", test_crc32_c);
    qtest_add_func("/sf32lb52x/crc/crc16-arc", test_crc16_arc);
    qtest_add_func("/sf32lb52x/crc/crc16-ccitt-false",
                   test_crc16_ccitt_false);
    qtest_add_func("/sf32lb52x/crc/crc8", test_crc8);
    qtest_add_func("/sf32lb52x/crc/crc8-rohc", test_crc8_rohc);
    qtest_add_func("/sf32lb52x/crc/crc7-mmc", test_crc7_mmc);
    qtest_add_func("/sf32lb52x/crc/datasize-masking", test_datasize_masking);
    qtest_add_func("/sf32lb52x/crc/done-status", test_done_status);
    qtest_add_func("/sf32lb52x/crc/invalid-access", test_invalid_access);
    qtest_add_func("/sf32lb52x/crc/unimplemented-offset",
                   test_unimplemented_offset);

    return g_test_run();
}
