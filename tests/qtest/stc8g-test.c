/*
 * STC8G1K08A machine tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bitops.h"
#include "qemu/sockets.h"
#include "libqtest.h"

#define MACHINE "-M stc8g1k08a-evb"
#define SOC "/machine/soc"
#define CPU SOC "/cpu"
#define GPIO SOC "/gpio"

#define FLASH_BASE 0x00000000
#define FLASH_SIZE (8 * 1024)
#define IDATA_BASE 0x00800000
#define IDATA_SIZE 256
#define XDATA_BASE 0x00810000
#define XDATA_SIZE 1024
#define SFR_BASE 0x01000000
#define SFR(address) (SFR_BASE + (address) - 0x80)
#define XFR_BASE 0x00c00000
#define XFR(address) (XFR_BASE + (address) - 0xfa00)
#define OPCODE_SLOT_SIZE 4
#define INVALID_HEX_ENV "QTEST_STC8G_INVALID_HEX"

enum {
    IRQ_INT0,
    IRQ_TIMER0,
    IRQ_INT1,
    IRQ_TIMER1,
    IRQ_UART1,
};

typedef struct GPIOPinDef {
    unsigned pin;
    uint8_t data;
    uint8_t mode1;
    uint8_t mode0;
    uint16_t pullup;
    uint16_t ncs;
    uint16_t slew_rate;
    uint16_t drive;
    uint16_t input_enable;
    uint8_t bit;
} GPIOPinDef;

static const GPIOPinDef gpio_pins[] = {
    { 0, 0xb0, 0xb1, 0xb2, 0xfe13, 0xfe1b, 0xfe23, 0xfe2b,
      0xfe33, 0 },
    { 1, 0xb0, 0xb1, 0xb2, 0xfe13, 0xfe1b, 0xfe23, 0xfe2b,
      0xfe33, 1 },
    { 2, 0xb0, 0xb1, 0xb2, 0xfe13, 0xfe1b, 0xfe23, 0xfe2b,
      0xfe33, 2 },
    { 3, 0xb0, 0xb1, 0xb2, 0xfe13, 0xfe1b, 0xfe23, 0xfe2b,
      0xfe33, 3 },
    { 4, 0xc8, 0xc9, 0xca, 0xfe15, 0xfe1d, 0xfe25, 0xfe2d,
      0xfe35, 4 },
    { 5, 0xc8, 0xc9, 0xca, 0xfe15, 0xfe1d, 0xfe25, 0xfe2d,
      0xfe35, 5 },
};

static void set_register_bit(QTestState *qts, uint64_t address,
                             unsigned bit, bool set)
{
    uint8_t value = qtest_readb(qts, address);

    value = deposit32(value, bit, 1, set);
    qtest_writeb(qts, address, value);
}

static void gpio_set_mode(QTestState *qts, const GPIOPinDef *pin,
                          bool mode1, bool mode0)
{
    set_register_bit(qts, SFR(pin->mode1), pin->bit, mode1);
    set_register_bit(qts, SFR(pin->mode0), pin->bit, mode0);
}

static void gpio_set_latch(QTestState *qts, const GPIOPinDef *pin,
                           bool level)
{
    set_register_bit(qts, SFR(pin->data), pin->bit, level);
}

static bool gpio_read_pin(QTestState *qts, const GPIOPinDef *pin)
{
    return extract8(qtest_readb(qts, SFR(pin->data)), pin->bit, 1);
}

static uint8_t timer_run_mask(unsigned timer)
{
    return timer ? 0x40 : 0x10;
}

static uint8_t timer_flag_mask(unsigned timer)
{
    return timer ? 0x80 : 0x20;
}

static uint8_t timer_tl_address(unsigned timer)
{
    return timer ? 0x8b : 0x8a;
}

static uint8_t timer_th_address(unsigned timer)
{
    return timer ? 0x8d : 0x8c;
}

static unsigned timer_irq(unsigned timer)
{
    return timer ? IRQ_TIMER1 : IRQ_TIMER0;
}

static void timer_set_count(QTestState *qts, unsigned timer,
                            uint8_t high, uint8_t low)
{
    qtest_writeb(qts, SFR(timer_th_address(timer)), high);
    qtest_writeb(qts, SFR(timer_tl_address(timer)), low);
}

static void gpio_pulse_falling(QTestState *qts, unsigned pin)
{
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
}

static QTestState *uart_test_init(char *socket_path, int *socket_fd)
{
    QTestState *qts;
    int temporary_fd;

    temporary_fd = mkstemp(socket_path);
    g_assert_cmpint(temporary_fd, >=, 0);
    close(temporary_fd);

    qts = qtest_initf(
        MACHINE
        " -chardev socket,id=uart-socket,path=%s,server=on,wait=off"
        " -serial chardev:uart-socket",
        socket_path);
    *socket_fd = unix_connect(socket_path, NULL);
    g_assert_cmpint(*socket_fd, >=, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    return qts;
}

static void uart_test_quit(QTestState *qts, int socket_fd,
                           const char *socket_path)
{
    close(socket_fd);
    qtest_quit(qts);
    unlink(socket_path);
}

static unsigned classic_opcode_length(uint8_t opcode)
{
    unsigned group = opcode >> 3;

    if ((opcode & 0x1f) == 0x01 || (opcode & 0x1f) == 0x11) {
        return 2;
    }
    if (group == 0x0f || group == 0x11 || group == 0x15 ||
        group == 0x1b) {
        return 2;
    }
    if (group == 0x17) {
        return 3;
    }

    switch (opcode) {
    case 0x02:
    case 0x10:
    case 0x12:
    case 0x20:
    case 0x30:
    case 0x43:
    case 0x53:
    case 0x63:
    case 0x75:
    case 0x85:
    case 0x90:
    case 0xb4:
    case 0xb5:
    case 0xb6:
    case 0xb7:
    case 0xd5:
        return 3;
    case 0x05:
    case 0x15:
    case 0x24:
    case 0x25:
    case 0x34:
    case 0x35:
    case 0x40:
    case 0x42:
    case 0x44:
    case 0x45:
    case 0x50:
    case 0x52:
    case 0x54:
    case 0x55:
    case 0x60:
    case 0x62:
    case 0x64:
    case 0x65:
    case 0x70:
    case 0x72:
    case 0x74:
    case 0x76:
    case 0x77:
    case 0x80:
    case 0x82:
    case 0x86:
    case 0x87:
    case 0x92:
    case 0x94:
    case 0x95:
    case 0xa0:
    case 0xa2:
    case 0xa6:
    case 0xa7:
    case 0xb0:
    case 0xb2:
    case 0xc0:
    case 0xc2:
    case 0xc5:
    case 0xd0:
    case 0xd2:
    case 0xe5:
    case 0xf5:
        return 2;
    default:
        return 1;
    }
}

static char *disassemble_one(QTestState *qts, uint32_t address,
                             unsigned length)
{
    g_autofree char *response =
        qtest_hmp(qts, "x/2i 0x%08x", address);
    g_autofree char *first_line = NULL;
    g_autofree char *next_address = NULL;
    const char *line_end = strpbrk(response, "\r\n");
    char *separator;

    g_assert_nonnull(line_end);
    first_line = g_strndup(response, line_end - response);
    separator = strchr(first_line, ':');
    g_assert_nonnull(separator);

    next_address = g_strdup_printf("0x%08x:", address + length);
    g_assert_nonnull(strstr(line_end, next_address));
    return g_strdup(g_strstrip(separator + 1));
}

static void test_reset_and_memory(void)
{
    QTestState *qts = qtest_init(MACHINE);

    g_assert_cmphex(qtest_readb(qts, SFR(0x81)), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, SFR(0x82)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x83)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8e)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb7)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd0)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe0)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe4)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe5)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xf0)), ==, 0x00);

    qtest_writeb(qts, IDATA_BASE, 0xa5);
    qtest_writeb(qts, IDATA_BASE + IDATA_SIZE - 1, 0x5a);
    g_assert_cmphex(qtest_readb(qts, IDATA_BASE), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IDATA_BASE + IDATA_SIZE - 1),
                    ==, 0x5a);
    qtest_writeb(qts, XDATA_BASE, 0x69);
    qtest_writeb(qts, XDATA_BASE + XDATA_SIZE - 1, 0x96);
    g_assert_cmphex(qtest_readb(qts, XDATA_BASE), ==, 0x69);
    g_assert_cmphex(qtest_readb(qts, XDATA_BASE + XDATA_SIZE - 1),
                    ==, 0x96);

    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0x00);
    qtest_writeb(qts, FLASH_BASE, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + FLASH_SIZE - 1),
                    ==, 0x00);

    /* Sparse GPIO and UART containers must not hide CPU-owned SFRs. */
    qtest_writeb(qts, SFR(0xa8), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa8)), ==, 0x9f);
    qtest_writeb(qts, SFR(0xb8), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb8)), ==, 0x1f);

    qtest_quit(qts);
}

static void test_raw_firmware_loading(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    const uint8_t image[] = { 0x74, 0x5a, 0x80, 0xfe };
    QTestState *qts;

    directory = g_dir_make_tmp("stc8g-raw-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.bin", NULL);
    g_assert_true(g_file_set_contents(filename, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);

    qts = qtest_initf(MACHINE " -bios %s", filename);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, image[0]);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 3), ==, image[3]);
    qtest_quit(qts);

    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_hex_firmware_loading(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    const char hex[] = ":04000000745A80FEB0\n:00000001FF\n";
    QTestState *qts;

    directory = g_dir_make_tmp("stc8g-hex-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.hex", NULL);
    g_assert_true(g_file_set_contents(filename, hex, -1, &error));
    g_assert_no_error(error);

    qts = qtest_initf(MACHINE " -bios %s", filename);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0x74);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 1), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 2), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 3), ==, 0xfe);
    qtest_quit(qts);

    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_invalid_hex_firmware(gconstpointer opaque)
{
    const char *hex = opaque;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;

    if (g_test_subprocess()) {
        const char *child_filename = g_getenv(INVALID_HEX_ENV);
        g_autofree char *quoted = NULL;
        QTestState *qts;

        g_assert_nonnull(child_filename);
        quoted = g_shell_quote(child_filename);
        qts = qtest_initf(MACHINE " -bios %s", quoted);
        qtest_quit(qts);
        g_error("invalid Intel HEX firmware was accepted");
    }

    directory = g_dir_make_tmp("stc8g-invalid-hex-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.hex", NULL);
    g_assert_true(g_file_set_contents(filename, hex, -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_setenv(INVALID_HEX_ENV, filename, true));

    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_unsetenv(INVALID_HEX_ENV);
    g_assert_false(g_test_trap_reached_timeout());
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*Unable to load Intel HEX firmware image*");

    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_instruction_disassembly(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *quoted = NULL;
    g_autofree uint8_t *image = g_malloc0(FLASH_SIZE);
    QTestState *qts;
    unsigned opcode;

    for (opcode = 0; opcode < 256; opcode++) {
        image[opcode * OPCODE_SLOT_SIZE] = opcode;
    }

    directory = g_dir_make_tmp("stc8g-disas-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.bin", NULL);
    g_assert_true(g_file_set_contents(filename, (char *)image,
                                      FLASH_SIZE, &error));
    g_assert_no_error(error);
    quoted = g_shell_quote(filename);
    qts = qtest_initf(MACHINE " -S -bios %s", quoted);

    for (opcode = 0; opcode < 256; opcode++) {
        g_autofree char *assembly = NULL;
        uint32_t address = opcode * OPCODE_SLOT_SIZE;

        assembly = disassemble_one(qts, address,
                                   classic_opcode_length(opcode));
        if (opcode == 0x00 || opcode == 0xa5) {
            g_assert_cmpstr(assembly, ==, "nop");
        } else {
            g_assert_cmpstr(assembly, !=, "nop");
        }
    }

    qtest_quit(qts);
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_gpio_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);
    static const struct {
        uint64_t address;
        uint8_t reset;
        uint8_t mask;
    } registers[] = {
        { SFR(0xb1), 0x0c, 0x0f },
        { SFR(0xb2), 0x00, 0x0f },
        { SFR(0xc9), 0x30, 0x30 },
        { SFR(0xca), 0x00, 0x30 },
        { XFR(0xfe13), 0x00, 0x0f },
        { XFR(0xfe15), 0x00, 0x30 },
        { XFR(0xfe1b), 0x00, 0x0f },
        { XFR(0xfe1d), 0x00, 0x30 },
        { XFR(0xfe23), 0x0f, 0x0f },
        { XFR(0xfe25), 0x30, 0x30 },
        { XFR(0xfe2b), 0x0f, 0x0f },
        { XFR(0xfe2d), 0x30, 0x30 },
        { XFR(0xfe33), 0x0f, 0x0f },
        { XFR(0xfe35), 0x30, 0x30 },
    };
    unsigned i;

    g_assert_cmphex(qtest_readb(qts, SFR(0xb0)), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc8)), ==, 0xff);
    for (i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readb(qts, registers[i].address),
                        ==, registers[i].reset);
        qtest_writeb(qts, registers[i].address, 0xff);
        g_assert_cmphex(qtest_readb(qts, registers[i].address),
                        ==, registers[i].mask);
        qtest_writeb(qts, registers[i].address, 0x00);
        g_assert_cmphex(qtest_readb(qts, registers[i].address), ==, 0x00);
    }

    qtest_writeb(qts, SFR(0xb0), 0x00);
    qtest_writeb(qts, SFR(0xc8), 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb0)) & 0xf0, ==, 0xf0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc8)) & 0xcf, ==, 0xcf);

    qtest_quit(qts);
}

static void test_gpio_modes(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned i;

    qtest_irq_intercept_out_named(qts, GPIO, "gpio-out");
    for (i = 0; i < ARRAY_SIZE(gpio_pins); i++) {
        const GPIOPinDef *pin = &gpio_pins[i];

        set_register_bit(qts, XFR(pin->input_enable), pin->bit, true);

        /* Quasi-bidirectional. */
        gpio_set_mode(qts, pin, false, false);
        gpio_set_latch(qts, pin, true);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 0);
        g_assert_false(gpio_read_pin(qts, pin));
        g_assert_false(qtest_get_irq(qts, pin->pin));
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 1);
        g_assert_true(gpio_read_pin(qts, pin));
        g_assert_true(qtest_get_irq(qts, pin->pin));
        gpio_set_latch(qts, pin, false);
        g_assert_false(qtest_get_irq(qts, pin->pin));

        /* Push-pull output ignores the external input. */
        gpio_set_mode(qts, pin, false, true);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 0);
        gpio_set_latch(qts, pin, true);
        g_assert_true(gpio_read_pin(qts, pin));
        g_assert_true(qtest_get_irq(qts, pin->pin));
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 1);
        gpio_set_latch(qts, pin, false);
        g_assert_false(gpio_read_pin(qts, pin));
        g_assert_false(qtest_get_irq(qts, pin->pin));

        /* High-impedance input follows the external level. */
        gpio_set_mode(qts, pin, true, false);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 1);
        g_assert_true(gpio_read_pin(qts, pin));
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 0);
        g_assert_false(gpio_read_pin(qts, pin));

        /* Open-drain drives zero and samples when released. */
        gpio_set_mode(qts, pin, true, true);
        gpio_set_latch(qts, pin, true);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 1);
        g_assert_true(gpio_read_pin(qts, pin));
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 0);
        g_assert_false(gpio_read_pin(qts, pin));
        gpio_set_latch(qts, pin, false);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 1);
        g_assert_false(gpio_read_pin(qts, pin));
    }

    qtest_quit(qts);
}

static void test_gpio_pullup_and_input_enable(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned i;

    qtest_irq_intercept_out_named(qts, GPIO, "gpio-out");
    for (i = 0; i < ARRAY_SIZE(gpio_pins); i++) {
        const GPIOPinDef *pin = &gpio_pins[i];

        gpio_set_mode(qts, pin, true, false);
        set_register_bit(qts, XFR(pin->input_enable), pin->bit, true);
        set_register_bit(qts, XFR(pin->pullup), pin->bit, false);
        qtest_set_irq_in(qts, GPIO, "gpio-float", pin->pin, 1);
        g_assert_false(gpio_read_pin(qts, pin));
        g_assert_false(qtest_get_irq(qts, pin->pin));

        set_register_bit(qts, XFR(pin->pullup), pin->bit, true);
        g_assert_true(gpio_read_pin(qts, pin));
        g_assert_true(qtest_get_irq(qts, pin->pin));

        /* A driven low input overrides the pull-up. */
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 0);
        g_assert_false(gpio_read_pin(qts, pin));

        /* Input disable affects port reads but not the physical pin. */
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin->pin, 1);
        set_register_bit(qts, XFR(pin->input_enable), pin->bit, false);
        g_assert_false(gpio_read_pin(qts, pin));
        g_assert_true(qtest_get_irq(qts, pin->pin));
    }

    qtest_quit(qts);
}

static void test_gpio_external_interrupts(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned interrupt;

    qtest_irq_intercept_in(qts, CPU);
    for (interrupt = 0; interrupt < 2; interrupt++) {
        unsigned pin = 2 + interrupt;
        uint8_t trigger = BIT(interrupt * 2);
        uint8_t flag = BIT(interrupt * 2 + 1);
        unsigned irq = interrupt ? IRQ_INT1 : IRQ_INT0;

        /* ITx=0 requests on either edge. */
        qtest_writeb(qts, SFR(0x88), 0x00);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, flag);
        g_assert_true(qtest_get_irq(qts, irq));
        qtest_writeb(qts, SFR(0x88), 0x00);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, flag);

        /* ITx=1 ignores rising edges and requests on falling edges. */
        qtest_writeb(qts, SFR(0x88), trigger);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, flag);
        qtest_writeb(qts, SFR(0x88), trigger);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, 0);
        g_assert_false(qtest_get_irq(qts, irq));
    }

    qtest_quit(qts);
}

static void check_timer_mode(unsigned timer, unsigned mode)
{
    QTestState *qts = qtest_init(MACHINE);
    uint8_t tmod = mode << (timer * 4);
    uint8_t flag = timer_flag_mask(timer);

    qtest_irq_intercept_in(qts, CPU);
    qtest_writeb(qts, SFR(0x89), tmod);
    if (mode == 2) {
        timer_set_count(qts, timer, 0xa5, 0xfe);
    } else {
        timer_set_count(qts, timer, 0xff, 0xfe);
    }
    qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
    qtest_clock_step(qts, 1000);

    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, flag);
    g_assert_true(qtest_get_irq(qts, timer_irq(timer)));
    if (mode == 1) {
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0x00);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0x00);
    } else if (mode == 2) {
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xa5);
    } else {
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfe);
    }

    qtest_quit(qts);
}

static void test_timer_modes(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        check_timer_mode(timer, 0);
        check_timer_mode(timer, 1);
        check_timer_mode(timer, 2);
    }
    check_timer_mode(0, 3);

    /* Timer 1 mode 3 is stopped. */
    {
        QTestState *qts = qtest_init(MACHINE);

        qtest_writeb(qts, SFR(0x89), 0x30);
        timer_set_count(qts, 1, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(1));
        qtest_clock_step(qts, 5000);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(1), ==, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(0x8b)), ==, 0xfe);
        qtest_quit(qts);
    }
}

static void test_timer_reload_and_gates(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned gate_pin = 2 + timer;
        uint8_t tmod = 0x09 << (timer * 4);

        qtest_writeb(qts, SFR(0x89), 0x00);
        timer_set_count(qts, timer, 0xff, 0xfc);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        timer_set_count(qts, timer, 0xaa, 0xbb);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfc);
        qtest_clock_step(qts, 2000);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xaa);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xbb);

        qtest_writeb(qts, SFR(0x88), 0x00);
        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_set_irq_in(qts, GPIO, "gpio-in", gate_pin, 0);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_clock_step(qts, 2000);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer), ==, 0);
        qtest_set_irq_in(qts, GPIO, "gpio-in", gate_pin, 1);
        qtest_clock_step(qts, 1000);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));

        qtest_quit(qts);
    }
}

static void test_timer_counters_and_rates(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned counter_pin = 4 + timer;
        uint8_t tmod = 0x05 << (timer * 4);

        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);
        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0x00);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));
        qtest_quit(qts);
    }

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        uint8_t x12 = timer ? BIT(6) : BIT(7);

        qtest_writeb(qts, SFR(0x8e), 0x01 | x12);
        qtest_writeb(qts, SFR(0x89), 0x01 << (timer * 4));
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_clock_step(qts, 83);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));
        qtest_quit(qts);
    }
}

static void test_uart1_transmit(void)
{
    char socket_path[] = "stc8g-uart-tx.XXXXXX";
    QTestState *qts;
    uint8_t received;
    int socket_fd;
    int ret;

    qts = uart_test_init(socket_path, &socket_fd);
    qtest_irq_intercept_in(qts, CPU);
    qtest_writeb(qts, SFR(0xa9), 0xa5);
    qtest_writeb(qts, SFR(0xb9), 0x5a);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa9)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb9)), ==, 0x5a);

    qtest_writeb(qts, SFR(0x98), 0xfc);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)), ==, 0xfc);
    qtest_writeb(qts, SFR(0x98), 0x00);
    qtest_writeb(qts, SFR(0x99), 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x02, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_UART1));
    ret = recv(socket_fd, &received, 1, 0);
    g_assert_cmpint(ret, ==, 1);
    g_assert_cmphex(received, ==, 0xa5);

    qtest_writeb(qts, SFR(0x98), 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_UART1));
    uart_test_quit(qts, socket_fd, socket_path);
}

static void test_uart1_receive(void)
{
    char socket_path[] = "stc8g-uart-rx.XXXXXX";
    QTestState *qts;
    int socket_fd;
    int ret;

    qts = uart_test_init(socket_path, &socket_fd);
    qtest_irq_intercept_in(qts, CPU);

    ret = send(socket_fd, "A", 1, 0);
    g_assert_cmpint(ret, ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 0);
    qtest_writeb(qts, SFR(0x98), 0x10);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'A');
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 1);
    g_assert_true(qtest_get_irq(qts, IRQ_UART1));

    ret = send(socket_fd, "B", 1, 0);
    g_assert_cmpint(ret, ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'A');
    qtest_writeb(qts, SFR(0x98), 0x10);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'B');

    qtest_writeb(qts, SFR(0x99), 0x5a);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x03, ==, 0x03);
    qtest_writeb(qts, SFR(0x98), 0x12);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x03, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_UART1));
    qtest_writeb(qts, SFR(0x98), 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_UART1));

    uart_test_quit(qts, socket_fd, socket_path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stc8g/reset-and-memory", test_reset_and_memory);
    qtest_add_func("/stc8g/firmware/raw", test_raw_firmware_loading);
    qtest_add_func("/stc8g/firmware/intel-hex",
                   test_hex_firmware_loading);
    qtest_add_data_func("/stc8g/firmware/intel-hex-out-of-range",
                        ":0120000000DF\n:00000001FF\n",
                        test_invalid_hex_firmware);
    qtest_add_data_func("/stc8g/firmware/intel-hex-bad-checksum",
                        ":04000000745A80FEB1\n:00000001FF\n",
                        test_invalid_hex_firmware);
    qtest_add_func("/stc8g/cpu/instruction-disassembly",
                   test_instruction_disassembly);
    qtest_add_func("/stc8g/gpio/registers", test_gpio_registers);
    qtest_add_func("/stc8g/gpio/modes", test_gpio_modes);
    qtest_add_func("/stc8g/gpio/pullup-and-input-enable",
                   test_gpio_pullup_and_input_enable);
    qtest_add_func("/stc8g/gpio/external-interrupts",
                   test_gpio_external_interrupts);
    qtest_add_func("/stc8g/timer/modes", test_timer_modes);
    qtest_add_func("/stc8g/timer/reload-and-gates",
                   test_timer_reload_and_gates);
    qtest_add_func("/stc8g/timer/counters-and-rates",
                   test_timer_counters_and_rates);
    qtest_add_func("/stc8g/uart1/transmit", test_uart1_transmit);
    qtest_add_func("/stc8g/uart1/receive", test_uart1_receive);

    return g_test_run();
}
