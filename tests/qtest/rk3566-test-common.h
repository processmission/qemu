/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Common helpers for the Rockchip RK3566 MicroDuck machine qtests.
 *
 * Copyright (c) 2026 Process Mission
 */

#ifndef TESTS_QTEST_RK3566_TEST_COMMON_H
#define TESTS_QTEST_RK3566_TEST_COMMON_H

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define RK3566_MACHINE_NAME "microduck-beta"
#define RK3566_MACHINE_TYPE "microduck-beta-machine"
#define RK3566_GIC_QOM "/machine/gic"

/*
 * Memory windows from the MicroDuck beta board map.  Only the windows and
 * register offsets used by the tests belong here; the device models keep the
 * authoritative register layout.
 */
#define RK3566_DRAM_BASE        0x00000000ULL
#define RK3566_UART0_BASE       0xfdd50000ULL
#define RK3566_UART2_BASE       0xfe660000ULL
#define RK3566_CRU_BASE         0xfdd20000ULL
#define RK3566_GICD_BASE        0xfd400000ULL
#define RK3566_TIMER0_BASE      0xfe5f0000ULL
#define RK3566_PWM_4_7_BASE     0xfe6e0000ULL
#define RK3566_PWM_8_11_BASE    0xfe6f0000ULL
#define RK3566_TSADC_BASE       0xfe710000ULL
#define RK3566_SARADC_BASE      0xfe720000ULL
#define RK3566_DPHY_BASE        0xfe870000ULL
#define RK3566_OTP_BASE         0xfe38c000ULL

/* DW8250 line status register (5 << 2). */
#define RK3566_DW8250_LSR       0x014

/* GICv3 distributor type register. */
#define RK3566_GICD_TYPER       0x004

/* RK3566 timer registers. */
#define RK3566_TIMER_LOAD       0x000
#define RK3566_TIMER_LOAD_HI    0x004
#define RK3566_TIMER_CUR        0x008
#define RK3566_TIMER_CUR_HI     0x00c
#define RK3566_TIMER_CONTROL    0x010
#define RK3566_TIMER_STATUS     0x018

/* RK3566 OTP registers; writes below 0x120 carry a 16-bit write mask. */
#define RK3566_OTP_CTRL         0x100
#define RK3566_OTP_ADDR         0x104
#define RK3566_OTP_TRIGGER      0x108
#define RK3566_OTP_DOUT         0x124
#define RK3566_OTP_STATUS       0x304
#define RK3566_OTP_STATUS_READ_DONE (1u << 2)
#define RK3566_OTP_FUSE_BYTE_ADDR 2

/*
 * Several RK3566 models take the write mask in the upper half-word of the
 * value written below 0x120: the lower half carries the data.
 */
#define RK3566_REG_WRITE_MASK(value) \
    ((uint32_t)(value) | ((uint32_t)(value) << 16))

/* Clear the selected bits through the same half-word write mask. */
#define RK3566_REG_WRITE_MASK_CLEAR(value) \
    ((uint32_t)(value) << 16)

/* CRU clock gate registers; bit per functional clock. */
#define RK3566_CRU_CLKGATE_CON(n) (0x300 + 4 * (n))
#define RK3566_CRU_CLKGATE_UART2  RK3566_CRU_CLKGATE_CON(28)

/*
 * Start the MicroDuck beta machine without a guest payload.  The machine
 * accepts a qtest-only boot, so no kernel image is needed.
 */
static inline QTestState *rk3566_qtest_start(void)
{
    return qtest_initf("-machine " RK3566_MACHINE_NAME " -m 512M");
}

/* MMIO accessors used by the register tests. */
static inline uint32_t rk3566_mmio_read32(QTestState *qts, uint64_t address)
{
    return qtest_readl(qts, address);
}

static inline void rk3566_mmio_write32(QTestState *qts, uint64_t address,
                                       uint32_t value)
{
    qtest_writel(qts, address, value);
}

static inline uint32_t rk3566_mmio_read32_at(QTestState *qts, uint64_t base,
                                             uint32_t offset)
{
    return rk3566_mmio_read32(qts, base + offset);
}

static inline void rk3566_mmio_write32_at(QTestState *qts, uint64_t base,
                                          uint32_t offset, uint32_t value)
{
    rk3566_mmio_write32(qts, base + offset, value);
}

/* Issue qom-list for @path, or return NULL when QEMU reports an error. */
static inline QDict *rk3566_qom_list(QTestState *qts, const char *path)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                                "  'arguments': { 'path': %s } }", path);

    if (qdict_haskey(response, "error")) {
        qobject_unref(response);
        return NULL;
    }
    return response;
}

/* Check that @path exists in the QOM tree. */
static inline bool rk3566_qom_object_exists(QTestState *qts, const char *path)
{
    QDict *response = rk3566_qom_list(qts, path);

    if (!response) {
        return false;
    }
    qobject_unref(response);
    return true;
}

/*
 * qom-list reports child objects as "child<type>"; strip the decoration so
 * callers can compare against the plain QOM type name.
 */
static inline char *rk3566_qom_strip_child_type(const char *qom_type)
{
    const char *prefix = "child<";
    size_t prefix_len = strlen(prefix);

    if (g_str_has_prefix(qom_type, prefix) &&
        g_str_has_suffix(qom_type, ">")) {
        return g_strndup(qom_type + prefix_len,
                         strlen(qom_type) - prefix_len - 1);
    }
    return g_strdup(qom_type);
}

/* Return the object type of child @name, or NULL when it does not exist. */
static inline char *rk3566_qom_child_object_type(QTestState *qts,
                                                 const char *path,
                                                 const char *name)
{
    QDict *response = rk3566_qom_list(qts, path);
    QList *children;
    const QListEntry *entry;
    char *type = NULL;

    if (!response) {
        return NULL;
    }
    children = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(child, "name"), name)) {
            type = rk3566_qom_strip_child_type(qdict_get_str(child, "type"));
            break;
        }
    }
    qobject_unref(response);
    return type;
}

/* Check that a child object of QOM type @object_type exists under @path. */
static inline bool rk3566_qom_child_type_exists(QTestState *qts,
                                                const char *path,
                                                const char *object_type)
{
    QDict *response = rk3566_qom_list(qts, path);
    QList *children;
    const QListEntry *entry;
    bool found = false;

    if (!response) {
        return false;
    }
    children = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        g_autofree char *type =
            rk3566_qom_strip_child_type(qdict_get_str(child, "type"));

        if (g_str_equal(type, object_type)) {
            found = true;
            break;
        }
    }
    qobject_unref(response);
    return found;
}

/* Read a string property through qom-get. */
static inline char *rk3566_qom_get_str(QTestState *qts, const char *path,
                                       const char *property)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                                "  'arguments': { 'path': %s,"
                                "                 'property': %s } }",
                                path, property);
    char *value;

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_true(qdict_haskey(response, "return"));
    value = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);
    return value;
}

/* Read an unsigned integer property through qom-get. */
static inline uint64_t rk3566_qom_get_uint64(QTestState *qts, const char *path,
                                             const char *property)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                                "  'arguments': { 'path': %s,"
                                "                 'property': %s } }",
                                path, property);
    uint64_t value;

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_true(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

/*
 * The camera, ISP, RGA2 and VENC models are only built when libavcodec,
 * libavutil, libswscale and x264 are available.  Tests that need them must
 * call g_test_skip() when this helper returns false.
 */
static inline bool rk3566_qtest_media_present(QTestState *qts)
{
    return rk3566_qom_child_type_exists(qts, "/machine", "rk3566-isp") &&
           rk3566_qom_child_type_exists(qts, "/machine", "rk3566-csi-dphy") &&
           rk3566_qom_child_type_exists(qts, "/machine", "rk3566-rga2") &&
           rk3566_qom_child_type_exists(qts, "/machine", "rk3566-rkvenc");
}

#endif /* TESTS_QTEST_RK3566_TEST_COMMON_H */
