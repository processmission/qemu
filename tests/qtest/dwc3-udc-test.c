/*
 * QTest for the generic DWC3 device-mode controller (dwc3-udc)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qobject.h"

#define DWC3_UDC_BASE               0xfc000000

/* DWC3 global registers. */
#define DWC3_GSBUSCFG0              0xc100
#define DWC3_GCTL                   0xc110
#define DWC3_GCOREID                0xc120
#define DWC3_GUSB2PHYCFG            0xc200
#define DWC3_GUSB3PIPECTL           0xc2c0
#define DWC3_GEVNTADR_LO            0xc400
#define DWC3_GEVNTADR_HI            0xc404
#define DWC3_GEVNTSIZ               0xc408
#define DWC3_GEVNTCOUNT             0xc40c

/* DWC3 device registers. */
#define DWC3_DCFG                   0xc700
#define DWC3_DCTL                   0xc704
#define DWC3_DEVTEN                 0xc708
#define DWC3_DSTS                   0xc70c
#define DWC3_DALEPENA               0xc720

#define DWC3_DEPCMDPAR1(ep)         (0xc804 + 16 * (ep))
#define DWC3_DEPCMDPAR0(ep)         (0xc808 + 16 * (ep))
#define DWC3_DEPCMD(ep)             (0xc80c + 16 * (ep))

#define DWC3_GEVNTSIZ_INTMASK       BIT(31)
#define DWC3_DCTL_CSFTRST           BIT(30)
#define DWC3_DCTL_RUNSTOP           BIT(31)
#define DWC3_DEPCMD_CMDACT          BIT(10)
#define DWC3_DEPCMD_DEPSTRTXFER     6

/* Reset values of the modeled IP; see evidence/usb-contract.md. */
#define DWC3_GCTL_RESET             0x30c13004
#define DWC3_GCOREID_RESET          0x5533330a
#define DWC3_GUSB2PHYCFG_RESET      0x40102410
#define DWC3_GUSB3PIPECTL_RESET     0x02000000
#define DWC3_DSTS_RESET             0x00540000

/* Event ring entry values. */
#define DWC3_DEVT_USBRST            0x101
#define DWC3_DEVT_CONNECTDONE       0x201
#define DWC3_DEPEVT_XFERCOMPLETE(ep) (((ep) << 1) | (1 << 6))

/* TRB control fields. */
#define DWC3_TRB_CTRL_HWO           BIT(0)
#define DWC3_TRB_CTRL_LST           BIT(1)
#define DWC3_TRB_TRBCTL_MASK        (0x3f << 4)
#define DWC3_TRBCTL_CONTROL_SETUP   (2 << 4)
#define DWC3_TRBCTL_CONTROL_STATUS_2 (3 << 4)

/* Guest RAM used by the tests (inside the machine's low RAM). */
#define DWC3_EVT_RING_ADDR          0x04000000ULL
#define DWC3_TRB_OUT_ADDR           0x04001000ULL
#define DWC3_TRB_IN_ADDR            0x04001010ULL
#define DWC3_SETUP_BUF_ADDR         0x04002000ULL

#define DWC3_SETUP_LEN              8
#define DWC3_EVT_SIZE               4

/* sysbus_init_irq() exports the single IRQ as the named "sysbus-irq" GPIO. */
#define DWC3_IRQ_GPIO_NAME          "sysbus-irq"

static QTestState *dwc3_udc_start(void)
{
    return qtest_initf("-machine rock-5b -m 128M -serial null");
}

/*
 * Locate the device QOM node.  The SoC instantiates it as a child of the
 * rockchip.rk3588.soc device; the child name is looked up by type so that the
 * test only depends on the SoC attachment point, not on its name.
 */
static char *dwc3_udc_qom_path(QTestState *qts)
{
    const char *parent = "/machine/soc";
    g_autofree char *expected = g_strdup_printf("child<%s>", "dwc3-udc");
    QDict *response = qtest_qmp(qts,
                                "{ 'execute': 'qom-list',"
                                "  'arguments': { 'path': %s } }", parent);
    QList *properties;
    QListEntry *entry;
    char *path = NULL;

    if (qdict_haskey(response, "error")) {
        QDict *error = qdict_get_qdict(response, "error");

        fprintf(stderr, "dwc3-udc: qom-list '%s' failed: %s\n", parent,
                qdict_get_try_str(error, "desc") ?: "unknown");
        g_assert_not_reached();
    }
    properties = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (!g_strcmp0(qdict_get_str(property, "type"), expected)) {
            path = g_strdup_printf("%s/%s", parent,
                                   qdict_get_str(property, "name"));
            break;
        }
    }
    qobject_unref(response);

    g_assert_nonnull(path);
    return path;
}

static uint32_t dwc3_udc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, DWC3_UDC_BASE + offset);
}

static void dwc3_udc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, DWC3_UDC_BASE + offset, value);
}

static void dwc3_udc_program_event_ring(QTestState *qts, uint32_t size)
{
    dwc3_udc_writel(qts, DWC3_GEVNTADR_LO, DWC3_EVT_RING_ADDR);
    dwc3_udc_writel(qts, DWC3_GEVNTADR_HI, DWC3_EVT_RING_ADDR >> 32);
    dwc3_udc_writel(qts, DWC3_GEVNTSIZ, size);
    dwc3_udc_writel(qts, DWC3_GEVNTCOUNT, 0);
}

static uint32_t dwc3_udc_event(QTestState *qts, uint32_t index)
{
    uint8_t raw[sizeof(uint32_t)];

    qtest_memread(qts, DWC3_EVT_RING_ADDR + index * sizeof(uint32_t),
                  raw, sizeof(raw));
    return ldl_le_p(raw);
}

static void dwc3_udc_write_trb(QTestState *qts, uint64_t addr,
                               uint32_t addr_lo, uint32_t addr_hi,
                               uint32_t status, uint32_t ctrl)
{
    uint32_t trb[4] = {
        cpu_to_le32(addr_lo),
        cpu_to_le32(addr_hi),
        cpu_to_le32(status),
        cpu_to_le32(ctrl),
    };

    qtest_memwrite(qts, addr, trb, sizeof(trb));
}

static void dwc3_udc_read_trb(QTestState *qts, uint64_t addr, uint32_t *trb)
{
    uint32_t raw[4];

    qtest_memread(qts, addr, raw, sizeof(raw));
    for (unsigned int i = 0; i < 4; i++) {
        trb[i] = le32_to_cpu(raw[i]);
    }
}

/* Arm a CONTROL SETUP TRB on physical endpoint 0 and start it. */
static void dwc3_udc_arm_setup(QTestState *qts)
{
    dwc3_udc_write_trb(qts, DWC3_TRB_OUT_ADDR,
                       DWC3_SETUP_BUF_ADDR, 0, DWC3_SETUP_LEN,
                       DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
                       DWC3_TRBCTL_CONTROL_SETUP);
    dwc3_udc_writel(qts, DWC3_DEPCMDPAR0(0), DWC3_TRB_OUT_ADDR >> 32);
    dwc3_udc_writel(qts, DWC3_DEPCMDPAR1(0), DWC3_TRB_OUT_ADDR);
    dwc3_udc_writel(qts, DWC3_DEPCMD(0),
                    DWC3_DEPCMD_DEPSTRTXFER | DWC3_DEPCMD_CMDACT);
}

/* Arm a CONTROL STATUS IN TRB on physical endpoint 1 and start it. */
static void dwc3_udc_arm_status_in(QTestState *qts)
{
    dwc3_udc_write_trb(qts, DWC3_TRB_IN_ADDR, 0, 0, 0,
                       DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
                       DWC3_TRBCTL_CONTROL_STATUS_2);
    dwc3_udc_writel(qts, DWC3_DEPCMDPAR0(1), DWC3_TRB_IN_ADDR >> 32);
    dwc3_udc_writel(qts, DWC3_DEPCMDPAR1(1), DWC3_TRB_IN_ADDR);
    dwc3_udc_writel(qts, DWC3_DEPCMD(1),
                    DWC3_DEPCMD_DEPSTRTXFER | DWC3_DEPCMD_CMDACT);
}

static void dwc3_udc_consume(QTestState *qts, uint32_t bytes)
{
    dwc3_udc_writel(qts, DWC3_GEVNTCOUNT, bytes);
}

static void test_reset_values(void)
{
    QTestState *qts = dwc3_udc_start();

    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GSBUSCFG0), ==, 0x00000000);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GCTL), ==, DWC3_GCTL_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GCOREID), ==,
                    DWC3_GCOREID_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GUSB2PHYCFG), ==,
                    DWC3_GUSB2PHYCFG_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GUSB3PIPECTL), ==,
                    DWC3_GUSB3PIPECTL_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DCFG), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DCTL), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DEVTEN), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DSTS), ==, DWC3_DSTS_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DALEPENA), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTSIZ), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==, 0);

    /* GCOREID and DSTS are read-only. */
    dwc3_udc_writel(qts, DWC3_GCOREID, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GCOREID), ==,
                    DWC3_GCOREID_RESET);
    dwc3_udc_writel(qts, DWC3_DSTS, 0xffffffff);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DSTS), ==, DWC3_DSTS_RESET);

    /* Unused but implemented register-bank words are plain storage. */
    dwc3_udc_writel(qts, 0xc000, 0x12345678);
    g_assert_cmphex(dwc3_udc_readl(qts, 0xc000), ==, 0x12345678);

    /* The window above the 64 KiB register bank is unimplemented. */
    g_assert_cmphex(dwc3_udc_readl(qts, 0x20000), ==, 0);
    dwc3_udc_writel(qts, 0x20000, 0xdeadbeef);
    g_assert_cmphex(dwc3_udc_readl(qts, 0x20000), ==, 0);

    qtest_quit(qts);
}

static void test_dctl_runstop(void)
{
    QTestState *qts = dwc3_udc_start();
    g_autofree char *path = dwc3_udc_qom_path(qts);

    qtest_irq_intercept_out_named(qts, path, DWC3_IRQ_GPIO_NAME);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Three events fit in the ring: two are queued by a RUNSTOP transition. */
    dwc3_udc_program_event_ring(qts, 3 * DWC3_EVT_SIZE);
    dwc3_udc_writel(qts, DWC3_DCTL, DWC3_DCTL_RUNSTOP);

    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==,
                    2 * DWC3_EVT_SIZE);
    g_assert_cmphex(dwc3_udc_event(qts, 0), ==, DWC3_DEVT_USBRST);
    g_assert_cmphex(dwc3_udc_event(qts, 1), ==, DWC3_DEVT_CONNECTDONE);
    g_assert_true(qtest_get_irq(qts, 0));

    /* GEVNTSIZ bit 31 masks the level IRQ without dropping events. */
    dwc3_udc_writel(qts, DWC3_GEVNTSIZ,
                    3 * DWC3_EVT_SIZE | DWC3_GEVNTSIZ_INTMASK);
    g_assert_false(qtest_get_irq(qts, 0));
    dwc3_udc_writel(qts, DWC3_GEVNTSIZ, 3 * DWC3_EVT_SIZE);
    g_assert_true(qtest_get_irq(qts, 0));

    /* Consuming one event clears the IRQ only when the ring becomes empty. */
    dwc3_udc_consume(qts, DWC3_EVT_SIZE);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==, DWC3_EVT_SIZE);
    g_assert_true(qtest_get_irq(qts, 0));

    /*
     * A second RUNSTOP transition appends two more events; the write pointer
     * wraps and the third slot holds the bus reset.
     */
    dwc3_udc_writel(qts, DWC3_DCTL, 0);
    dwc3_udc_writel(qts, DWC3_DCTL, DWC3_DCTL_RUNSTOP);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==,
                    3 * DWC3_EVT_SIZE);
    g_assert_cmphex(dwc3_udc_event(qts, 2), ==, DWC3_DEVT_USBRST);
    g_assert_cmphex(dwc3_udc_event(qts, 0), ==, DWC3_DEVT_CONNECTDONE);

    /* A full ring drops new events instead of corrupting the ring. */
    dwc3_udc_writel(qts, DWC3_DCTL, 0);
    dwc3_udc_writel(qts, DWC3_DCTL, DWC3_DCTL_RUNSTOP);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==,
                    3 * DWC3_EVT_SIZE);

    dwc3_udc_consume(qts, 3 * DWC3_EVT_SIZE);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_dctl_csftrst(void)
{
    QTestState *qts = dwc3_udc_start();
    g_autofree char *path = dwc3_udc_qom_path(qts);

    qtest_irq_intercept_out_named(qts, path, DWC3_IRQ_GPIO_NAME);
    dwc3_udc_program_event_ring(qts, 4 * DWC3_EVT_SIZE);
    dwc3_udc_writel(qts, DWC3_DCTL, DWC3_DCTL_RUNSTOP);
    g_assert_true(qtest_get_irq(qts, 0));

    /* Dirty some device state, including an endpoint configuration. */
    dwc3_udc_writel(qts, DWC3_DCFG, 0x00001234);
    dwc3_udc_writel(qts, DWC3_DEVTEN, 0xffffffff);
    dwc3_udc_writel(qts, DWC3_DALEPENA, 0x3);
    dwc3_udc_writel(qts, DWC3_DEPCMDPAR0(3), 2 << 1);
    dwc3_udc_writel(qts, DWC3_DEPCMD(3), 1 | DWC3_DEPCMD_CMDACT);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DEPCMD(3)), ==, 1);

    /* DCTL.CSFTRST is self-clearing and resets the whole controller state. */
    dwc3_udc_writel(qts, DWC3_DCTL, DWC3_DCTL_CSFTRST);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DCTL), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GCOREID), ==,
                    DWC3_GCOREID_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GCTL), ==, DWC3_GCTL_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GUSB2PHYCFG), ==,
                    DWC3_GUSB2PHYCFG_RESET);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DCFG), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DEVTEN), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DALEPENA), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTSIZ), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTADR_LO), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* CSFTRST combined with RUNSTOP must not latch RUNSTOP. */
    dwc3_udc_writel(qts, DWC3_DCTL, DWC3_DCTL_CSFTRST | DWC3_DCTL_RUNSTOP);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DCTL), ==, 0);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==, 0);

    qtest_quit(qts);
}

static void test_ep0_enumeration(void)
{
    static const uint8_t requests[][DWC3_SETUP_LEN] = {
        /* SET_ADDRESS, address 1 */
        { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 },
        /* SET_CONFIGURATION, configuration 1 */
        { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 },
        /* SET_CONTROL_LINE_STATE (class, interface OUT), DTR */
        { 0x21, 0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };
    QTestState *qts = dwc3_udc_start();
    g_autofree char *path = dwc3_udc_qom_path(qts);

    qtest_irq_intercept_out_named(qts, path, DWC3_IRQ_GPIO_NAME);
    dwc3_udc_program_event_ring(qts, DWC3_EVT_SIZE);

    for (unsigned int i = 0; i < ARRAY_SIZE(requests); i++) {
        uint32_t trb[4];
        uint8_t setup[DWC3_SETUP_LEN];

        /* Arm the SETUP TRB; the emulated host answers after its delay. */
        dwc3_udc_arm_setup(qts);
        g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==, 0);
        g_assert_cmphex(dwc3_udc_readl(qts, DWC3_DEPCMD(0)), ==,
                        (DWC3_DEPCMD_DEPSTRTXFER | BIT(16)));

        qtest_clock_step(qts, 2000000);
        g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==,
                        DWC3_EVT_SIZE);
        g_assert_true(qtest_get_irq(qts, 0));
        g_assert_cmphex(dwc3_udc_event(qts, 0), ==,
                        DWC3_DEPEVT_XFERCOMPLETE(0));

        qtest_memread(qts, DWC3_SETUP_BUF_ADDR, setup, sizeof(setup));
        g_assert_cmpint(memcmp(setup, requests[i], sizeof(setup)), ==, 0);

        /* The completed TRB has HWO cleared and the remaining size in status.
            */
        dwc3_udc_read_trb(qts, DWC3_TRB_OUT_ADDR, trb);
        g_assert_cmphex(trb[2], ==, 0);
        g_assert_cmphex(trb[3], ==,
                        DWC3_TRB_CTRL_LST | DWC3_TRBCTL_CONTROL_SETUP);

        dwc3_udc_consume(qts, DWC3_EVT_SIZE);
        g_assert_false(qtest_get_irq(qts, 0));

        /* The firmware stack answers with a zero-length STATUS IN stage. */
        dwc3_udc_arm_status_in(qts);
        g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==,
                        DWC3_EVT_SIZE);
        g_assert_cmphex(dwc3_udc_event(qts, 0), ==,
                        DWC3_DEPEVT_XFERCOMPLETE(1));

        dwc3_udc_consume(qts, DWC3_EVT_SIZE);
        g_assert_false(qtest_get_irq(qts, 0));
    }

    /*
     * Once the three enumeration requests have completed, the next SETUP is a
     * CDC SET_LINE_CODING that carries one byte of console input.  No console
     * byte has been queued by this test, so the request must not be emitted
     * even after the longer input pacing delay.
     */
    dwc3_udc_arm_setup(qts);
    qtest_clock_step(qts, 20000000);
    g_assert_cmphex(dwc3_udc_readl(qts, DWC3_GEVNTCOUNT), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/dwc3-udc/reset-values", test_reset_values);
    qtest_add_func("/dwc3-udc/dctl-runstop", test_dctl_runstop);
    qtest_add_func("/dwc3-udc/dctl-csftrst", test_dctl_csftrst);
    qtest_add_func("/dwc3-udc/ep0-enumeration", test_ep0_enumeration);

    return g_test_run();
}
