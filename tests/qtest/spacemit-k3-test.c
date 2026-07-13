/*
 * QTest testcase for the SpacemiT K3 Pico-ITX machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include <libfdt.h>
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define K3_NUM_HARTS                  8
#define K3_HARTS_PER_CLUSTER          4
#define K3_TIMEBASE_FREQ              24000000

#define K3_RESET_BASE                 UINT64_C(0xc0800000)
#define K3_RESET_SIZE                 UINT64_C(0x1000)
#define K3_UART0_BASE                 UINT64_C(0xd4017000)
#define K3_UART0_SIZE                 UINT64_C(0x100)
#define K3_S_IMSIC_BASE               UINT64_C(0xe0400000)
#define K3_S_IMSIC_HART_STRIDE        UINT64_C(0x40000)
#define K3_S_APLIC_BASE               UINT64_C(0xe0804000)
#define K3_M_IMSIC_BASE               UINT64_C(0xf1000000)
#define K3_M_IMSIC_HART_STRIDE        UINT64_C(0x1000)
#define K3_M_APLIC_BASE               UINT64_C(0xf1800000)
#define K3_M_CLINT_BASE               UINT64_C(0xf1810000)
#define K3_FIRMWARE_BASE              UINT64_C(0x100000000)
#define K3_FIRMWARE_SIZE              UINT64_C(0x2000000)
#define K3_DRAM_BASE                  UINT64_C(0x102000000)
#define K3_DRAM_SIZE                  UINT64_C(0x80000000)

#define ACLINT_MTIME                  UINT64_C(0xbff8)

#define APLIC_DOMAINCFG               0x0000
#define APLIC_DOMAINCFG_IE            (1U << 8)
#define APLIC_DOMAINCFG_DM            (1U << 2)
#define APLIC_DOMAINCFG_RDONLY        (1U << 31)
#define APLIC_SOURCECFG_BASE          0x0004
#define APLIC_SOURCECFG_D             (1U << 10)
#define APLIC_SOURCECFG_LEVEL_HIGH    0x6
#define APLIC_MMSICFGADDR             0x1bc0
#define APLIC_MMSICFGADDRH            0x1bc4
#define APLIC_SMSICFGADDR             0x1bc8
#define APLIC_SMSICFGADDRH            0x1bcc
#define APLIC_CLRIP_BASE              0x1d00
#define APLIC_SETIENUM                0x1edc
#define APLIC_TARGET_BASE             0x3004
#define APLIC_LHXS_SHIFT              20
#define APLIC_LHXW_SHIFT              12

#define K3_UART0_IRQ                  42
#define UART_RBR                      0x00
#define UART_IER                      0x04
#define UART_LSR                      0x14
#define UART_IER_RDI                  0x01
#define UART_LSR_DR                   0x01

#define K3_FDT_MAX_SIZE               (64 * KiB)

#define CSR_MIP                       0x344
#define CSR_MENVCFG                   0x30a
#define CSR_MISELECT                  0x350
#define CSR_MIREG                     0x351
#define CSR_MTOPEI                    0x35c
#define CSR_SISELECT                  0x150
#define CSR_SIREG                     0x151
#define CSR_STOPEI                    0x15c
#define CSR_STIMECMP                  0x14d
#define MIP_STIP                      (UINT64_C(1) << 5)
#define MIP_SEIP                      (UINT64_C(1) << 9)
#define MIP_MEIP                      (UINT64_C(1) << 11)
#define MENVCFG_STCE                  (UINT64_C(1) << 63)
#define ISELECT_IMSIC_EIDELIVERY      0x70
#define ISELECT_IMSIC_EIP0            0x80
#define ISELECT_IMSIC_EIE0            0xc0

static QTestState *k3_qtest_init(void)
{
    return qtest_init("-M k3-pico-itx -bios none -display none "
                      "-nodefaults");
}

static char *k3_create_fdt(const void *compatible, size_t compatible_len)
{
    g_autofree uint8_t *fdt = g_malloc0(K3_FDT_MAX_SIZE);
    const fdt32_t memory_reg[] = {
        cpu_to_fdt32((uint32_t)(K3_DRAM_BASE >> 32)),
        cpu_to_fdt32((uint32_t)K3_DRAM_BASE),
        cpu_to_fdt32((uint32_t)(K3_DRAM_SIZE >> 32)),
        cpu_to_fdt32((uint32_t)K3_DRAM_SIZE),
    };
    g_autoptr(GError) error = NULL;
    char *path = g_strdup("spacemit-k3-fdt-XXXXXX");
    int cpus;
    int memory;
    int fd;

    g_assert_cmpint(fdt_create_empty_tree(fdt, K3_FDT_MAX_SIZE), ==, 0);
    g_assert_cmpint(fdt_setprop(fdt, 0, "compatible", compatible,
                                compatible_len), ==, 0);

    cpus = fdt_add_subnode(fdt, 0, "cpus");
    g_assert_cmpint(cpus, >=, 0);
    g_assert_cmpint(fdt_setprop_u32(fdt, cpus, "timebase-frequency",
                                    K3_TIMEBASE_FREQ), ==, 0);
    for (unsigned int i = 0; i < K3_NUM_HARTS; i++) {
        g_autofree char *name = g_strdup_printf("cpu@%x", i);

        g_assert_cmpint(fdt_add_subnode(fdt, cpus, name), >=, 0);
    }

    memory = fdt_add_subnode(fdt, 0, "memory@102000000");
    g_assert_cmpint(memory, >=, 0);
    g_assert_cmpint(fdt_setprop(fdt, memory, "reg", memory_reg,
                                sizeof(memory_reg)), ==, 0);
    g_assert_cmpint(fdt_pack(fdt), ==, 0);

    fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(path, (const char *)fdt,
                                      fdt_totalsize(fdt), &error));
    g_assert_no_error(error);

    return path;
}

static void test_fdt_compatible_list(void)
{
    static const char compatible[] =
        "spacemit,k3\0spacemit,k3-pico-itx";
    g_autofree char *dtb_path = k3_create_fdt(compatible,
                                              sizeof(compatible));
    QTestState *qts = qtest_initf(
        "-M k3-pico-itx -bios none -display none -nodefaults -dtb %s",
        dtb_path);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(dtb_path), ==, 0);
}

static void test_fdt_missing_compatible(void)
{
    static const char compatible[] = "spacemit,k3";
    g_autofree char *dtb_path = k3_create_fdt(compatible,
                                              sizeof(compatible));
    g_autofree char *stderr_buf = NULL;
    g_autoptr(GError) error = NULL;
    /* Make an unexpected successful DTB validation terminate promptly. */
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-M", "k3-pico-itx",
        "-bios", "none",
        "-display", "none",
        "-nodefaults",
        "-dtb", dtb_path,
        "-device", "spacemit-k3-test-invalid",
        NULL,
    };
    int exit_status;
    bool spawned;

    spawned = g_spawn_sync(NULL, (char **)argv, NULL,
                           G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
                           &stderr_buf, &exit_status, &error);
    g_assert_true(spawned);
    g_assert_no_error(error);
    g_assert_false(g_spawn_check_exit_status(exit_status, NULL));
    g_assert_nonnull(strstr(stderr_buf,
                           "DTB requires root compatible "
                           "'spacemit,k3-pico-itx'"));
    g_assert_cmpint(g_unlink(dtb_path), ==, 0);
}

static void test_cpu_requires_vector(void)
{
    g_autofree char *stderr_buf = NULL;
    g_autoptr(GError) error = NULL;
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-M", "k3-pico-itx",
        "-cpu", "spacemit-x100,v=false",
        "-bios", "none",
        "-display", "none",
        "-nodefaults",
        "-device", "spacemit-k3-test-invalid",
        NULL,
    };
    int exit_status;
    bool spawned;

    spawned = g_spawn_sync(NULL, (char **)argv, NULL,
                           G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
                           &stderr_buf, &exit_status, &error);
    g_assert_true(spawned);
    g_assert_no_error(error);
    g_assert_false(g_spawn_check_exit_status(exit_status, NULL));
    g_assert_nonnull(strstr(stderr_buf,
                           "K3 X100 hart 0 requires the V extension"));
}

static uint64_t k3_qom_get_uint(QTestState *qts, const char *path,
                                const char *property)
{
    QDict *response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': "
        "{ 'path': %s, 'property': %s } }", path, property);
    uint64_t value;

    g_assert(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);

    return value;
}

static uint64_t k3_csr_get(QTestState *qts, unsigned int hartid, int csr)
{
    uint64_t value = 0;

    g_assert_cmpuint(qtest_csr_call(qts, "get_csr", hartid, csr, &value),
                     ==, 0);
    return value;
}

static void k3_csr_set(QTestState *qts, unsigned int hartid, int csr,
                       uint64_t value)
{
    g_assert_cmpuint(qtest_csr_call(qts, "set_csr", hartid, csr, &value),
                     ==, 0);
}

static uint64_t k3_imsic_indirect_read(QTestState *qts, unsigned int hartid,
                                      bool mmode, uint64_t selector)
{
    k3_csr_set(qts, hartid, mmode ? CSR_MISELECT : CSR_SISELECT, selector);
    return k3_csr_get(qts, hartid, mmode ? CSR_MIREG : CSR_SIREG);
}

static void k3_imsic_indirect_write(QTestState *qts, unsigned int hartid,
                                    bool mmode, uint64_t selector,
                                    uint64_t value)
{
    k3_csr_set(qts, hartid, mmode ? CSR_MISELECT : CSR_SISELECT, selector);
    k3_csr_set(qts, hartid, mmode ? CSR_MIREG : CSR_SIREG, value);
}

static void k3_imsic_enable(QTestState *qts, unsigned int hartid, bool mmode,
                            unsigned int eiid)
{
    g_assert_cmpuint(eiid, <, 64);
    k3_imsic_indirect_write(qts, hartid, mmode,
                            ISELECT_IMSIC_EIDELIVERY, 1);
    k3_imsic_indirect_write(qts, hartid, mmode, ISELECT_IMSIC_EIE0,
                            UINT64_C(1) << eiid);
}

static void k3_imsic_claim(QTestState *qts, unsigned int hartid, bool mmode,
                           unsigned int eiid)
{
    int topei_csr = mmode ? CSR_MTOPEI : CSR_STOPEI;
    uint64_t topei = k3_csr_get(qts, hartid, topei_csr);

    g_assert_cmpuint(topei >> 16, ==, eiid);
    k3_csr_set(qts, hartid, topei_csr, topei);
}

static void test_topology(void)
{
    QTestState *qts = k3_qtest_init();
    QDict *response;
    QList *cpus;
    QListEntry *entry;
    bool seen[K3_NUM_HARTS] = { false };

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, K3_NUM_HARTS);

    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, entry->value);
        unsigned int index = qdict_get_int(cpu, "cpu-index");
        unsigned int cluster = index / K3_HARTS_PER_CLUSTER;
        unsigned int hart = index % K3_HARTS_PER_CLUSTER;
        g_autofree char *expected_path = g_strdup_printf(
            "/machine/soc/cluster%u/cpus/harts[%u]", cluster, hart);

        g_assert_cmpuint(index, <, K3_NUM_HARTS);
        g_assert_false(seen[index]);
        seen[index] = true;
        g_assert_cmpstr(qdict_get_str(cpu, "qom-type"), ==,
                        "spacemit-x100-riscv-cpu");
        g_assert_cmpstr(qdict_get_str(cpu, "qom-path"), ==, expected_path);
    }
    for (unsigned int i = 0; i < K3_NUM_HARTS; i++) {
        g_assert_true(seen[i]);
    }

    g_assert_cmpuint(k3_qom_get_uint(qts, "/machine/soc/cluster0",
                                     "cluster-id"), ==, 0);
    g_assert_cmpuint(k3_qom_get_uint(qts, "/machine/soc/cluster1",
                                     "cluster-id"), ==, 1);
    g_assert_cmpuint(k3_qom_get_uint(
        qts, "/machine/soc/cluster0/cpus/harts[0]", "vlen"), ==, 256);

    qobject_unref(response);
    qtest_quit(qts);
}

static void test_address_map(void)
{
    static const uint64_t first_pattern = UINT64_C(0x0123456789abcdef);
    static const uint64_t last_pattern = UINT64_C(0xfedcba9876543210);
    QTestState *qts = k3_qtest_init();

    g_assert_cmphex(qtest_readl(qts, K3_RESET_BASE), ==, 0x00000297);
    g_assert_cmphex(qtest_readl(qts, K3_RESET_BASE + K3_RESET_SIZE), ==, 0);

    qtest_writeq(qts, K3_FIRMWARE_BASE, first_pattern);
    qtest_writeq(qts, K3_FIRMWARE_BASE + K3_FIRMWARE_SIZE - 8,
                 last_pattern);
    g_assert_cmphex(qtest_readq(qts, K3_FIRMWARE_BASE), ==, first_pattern);
    g_assert_cmphex(qtest_readq(qts,
                    K3_FIRMWARE_BASE + K3_FIRMWARE_SIZE - 8), ==,
                    last_pattern);

    qtest_writeq(qts, K3_DRAM_BASE, last_pattern);
    qtest_writeq(qts, K3_DRAM_BASE + K3_DRAM_SIZE - 8, first_pattern);
    g_assert_cmphex(qtest_readq(qts, K3_DRAM_BASE), ==, last_pattern);
    g_assert_cmphex(qtest_readq(qts, K3_DRAM_BASE + K3_DRAM_SIZE - 8), ==,
                    first_pattern);

    g_assert_cmphex(qtest_readl(qts, K3_M_APLIC_BASE + APLIC_DOMAINCFG), ==,
                    APLIC_DOMAINCFG_RDONLY | APLIC_DOMAINCFG_DM);
    g_assert_cmphex(qtest_readl(qts, K3_S_APLIC_BASE + APLIC_DOMAINCFG), ==,
                    APLIC_DOMAINCFG_RDONLY | APLIC_DOMAINCFG_DM);
    g_assert_cmphex(qtest_readl(qts, K3_M_IMSIC_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, K3_S_IMSIC_BASE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, K3_UART0_BASE + UART_LSR), ==, 0x60);
    g_assert_cmphex(qtest_readl(qts, K3_UART0_BASE + K3_UART0_SIZE - 4), ==,
                    0);

    qtest_quit(qts);
}

static void test_timer_and_sstc(void)
{
    QTestState *qts = k3_qtest_init();
    uint64_t before = qtest_readq(qts, K3_M_CLINT_BASE + ACLINT_MTIME);
    uint64_t after;

    qtest_clock_step(qts, 1000000);
    after = qtest_readq(qts, K3_M_CLINT_BASE + ACLINT_MTIME);
    g_assert_cmpuint(after - before, ==, K3_TIMEBASE_FREQ / 1000);

    k3_csr_set(qts, 0, CSR_STIMECMP, UINT64_MAX);
    k3_csr_set(qts, 0, CSR_MENVCFG, MENVCFG_STCE);
    g_assert_cmphex(k3_csr_get(qts, 0, CSR_MIP) & MIP_STIP, ==, 0);
    k3_csr_set(qts, 0, CSR_STIMECMP, after + K3_TIMEBASE_FREQ / 1000);
    qtest_clock_step(qts, 999999);
    g_assert_cmphex(k3_csr_get(qts, 0, CSR_MIP) & MIP_STIP, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(k3_csr_get(qts, 0, CSR_MIP) & MIP_STIP, ==, MIP_STIP);
    k3_csr_set(qts, 0, CSR_STIMECMP, UINT64_MAX);
    g_assert_cmphex(k3_csr_get(qts, 0, CSR_MIP) & MIP_STIP, ==, 0);

    qtest_quit(qts);
}

static void test_imsic_hart_routing(void)
{
    const unsigned int eiid = 10;
    QTestState *qts = k3_qtest_init();

    k3_imsic_enable(qts, 0, false, eiid);
    k3_imsic_enable(qts, 7, false, eiid);

    /* The unimplemented VS page in hart 0's stride must not signal S-mode. */
    qtest_writel(qts, K3_S_IMSIC_BASE + 0x1000, eiid);
    g_assert_cmphex(k3_imsic_indirect_read(qts, 0, false,
                                           ISELECT_IMSIC_EIP0), ==, 0);

    qtest_writel(qts, K3_S_IMSIC_BASE, eiid);
    g_assert_cmphex(k3_imsic_indirect_read(qts, 0, false,
                                           ISELECT_IMSIC_EIP0), ==,
                    UINT64_C(1) << eiid);
    g_assert_cmphex(k3_csr_get(qts, 0, CSR_MIP) & MIP_SEIP, ==, MIP_SEIP);
    g_assert_cmphex(k3_imsic_indirect_read(qts, 7, false,
                                           ISELECT_IMSIC_EIP0), ==, 0);
    k3_imsic_claim(qts, 0, false, eiid);

    qtest_writel(qts, K3_S_IMSIC_BASE + 7 * K3_S_IMSIC_HART_STRIDE, eiid);
    g_assert_cmphex(k3_imsic_indirect_read(qts, 7, false,
                                           ISELECT_IMSIC_EIP0), ==,
                    UINT64_C(1) << eiid);
    g_assert_cmphex(k3_imsic_indirect_read(qts, 0, false,
                                           ISELECT_IMSIC_EIP0), ==, 0);
    k3_imsic_claim(qts, 7, false, eiid);

    k3_imsic_enable(qts, 7, true, eiid);
    qtest_writel(qts, K3_M_IMSIC_BASE + 7 * K3_M_IMSIC_HART_STRIDE, eiid);
    g_assert_cmphex(k3_imsic_indirect_read(qts, 7, true,
                                           ISELECT_IMSIC_EIP0), ==,
                    UINT64_C(1) << eiid);
    g_assert_cmphex(k3_csr_get(qts, 7, CSR_MIP) & MIP_MEIP, ==, MIP_MEIP);
    k3_imsic_claim(qts, 7, true, eiid);

    qtest_quit(qts);
}

static bool k3_wait_for_uart_rx(QTestState *qts)
{
    for (unsigned int i = 0; i < 10000; i++) {
        if (qtest_readl(qts, K3_UART0_BASE + UART_LSR) & UART_LSR_DR) {
            return true;
        }
        g_usleep(100);
    }

    return false;
}

static void test_uart_aplic_imsic(void)
{
    const unsigned int eiid = K3_UART0_IRQ;
    const uint64_t sourcecfg = APLIC_SOURCECFG_BASE +
                               (K3_UART0_IRQ - 1) * 4;
    const uint64_t target = APLIC_TARGET_BASE +
                            (K3_UART0_IRQ - 1) * 4;
    const uint64_t input_word = APLIC_CLRIP_BASE +
                                (K3_UART0_IRQ / 32) * 4;
    const uint32_t input_mask = 1U << (K3_UART0_IRQ % 32);
    int sock_fd;
    QTestState *qts = qtest_init_with_serial(
        "-M k3-pico-itx -bios none -display none -nodefaults", &sock_fd);

    k3_imsic_enable(qts, 0, false, eiid);

    qtest_writel(qts, K3_M_APLIC_BASE + APLIC_MMSICFGADDR,
                 K3_M_IMSIC_BASE >> 12);
    qtest_writel(qts, K3_M_APLIC_BASE + APLIC_MMSICFGADDRH,
                 3U << APLIC_LHXW_SHIFT);
    qtest_writel(qts, K3_M_APLIC_BASE + APLIC_SMSICFGADDR,
                 K3_S_IMSIC_BASE >> 12);
    qtest_writel(qts, K3_M_APLIC_BASE + APLIC_SMSICFGADDRH,
                 6U << APLIC_LHXS_SHIFT);

    qtest_writel(qts, K3_M_APLIC_BASE + sourcecfg, APLIC_SOURCECFG_D);
    qtest_writel(qts, K3_S_APLIC_BASE + sourcecfg,
                 APLIC_SOURCECFG_LEVEL_HIGH);
    qtest_writel(qts, K3_S_APLIC_BASE + target, eiid);
    qtest_writel(qts, K3_S_APLIC_BASE + APLIC_SETIENUM, K3_UART0_IRQ);
    qtest_writel(qts, K3_S_APLIC_BASE + APLIC_DOMAINCFG,
                 APLIC_DOMAINCFG_IE);

    qtest_writel(qts, K3_UART0_BASE + UART_IER, UART_IER_RDI);
    g_assert_cmpint(send(sock_fd, "K", 1, 0), ==, 1);
    g_assert_true(k3_wait_for_uart_rx(qts));

    g_assert_cmphex(qtest_readl(qts, K3_S_APLIC_BASE + input_word) &
                    input_mask, ==, input_mask);
    g_assert_cmphex(k3_imsic_indirect_read(qts, 0, false,
                                           ISELECT_IMSIC_EIP0), ==,
                    UINT64_C(1) << eiid);
    g_assert_cmphex(k3_csr_get(qts, 0, CSR_MIP) & MIP_SEIP, ==, MIP_SEIP);

    g_assert_cmphex(qtest_readl(qts, K3_UART0_BASE + UART_RBR), ==, 'K');
    g_assert_cmphex(qtest_readl(qts, K3_S_APLIC_BASE + input_word) &
                    input_mask, ==, 0);
    k3_imsic_claim(qts, 0, false, eiid);
    g_assert_cmphex(k3_csr_get(qts, 0, CSR_MIP) & MIP_SEIP, ==, 0);

    close(sock_fd);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("k3-pico-itx")) {
        qtest_add_func("spacemit-k3/topology", test_topology);
        qtest_add_func("spacemit-k3/address-map", test_address_map);
        qtest_add_func("spacemit-k3/fdt-compatible-list",
                       test_fdt_compatible_list);
        qtest_add_func("spacemit-k3/fdt-missing-compatible",
                       test_fdt_missing_compatible);
        qtest_add_func("spacemit-k3/cpu-requires-vector",
                       test_cpu_requires_vector);
        qtest_add_func("spacemit-k3/timer-sstc", test_timer_and_sstc);
        qtest_add_func("spacemit-k3/imsic-routing", test_imsic_hart_routing);
        qtest_add_func("spacemit-k3/uart-aplic-imsic",
                       test_uart_aplic_imsic);
    }

    return g_test_run();
}
