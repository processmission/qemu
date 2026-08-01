/*
 * STC8G1K08A machine tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "hw/core/clock.h"
#include "qemu/bitops.h"
#include "qemu/sockets.h"
#include "qobject/qdict.h"
#include "libqtest.h"

#define MACHINE "-M stc8g1k08a"
#define SOC "/machine/soc"
#define CPU SOC "/cpu"
#define ADC SOC "/adc"
#define GPIO SOC "/gpio"
#define I2C SOC "/i2c"
#define IAP SOC "/iap"
#define INTC SOC "/intc"
#define LVD SOC "/lvd"
#define MDU SOC "/mdu"
#define PCA SOC "/pca"
#define SPI SOC "/spi"
#define SYSCTRL SOC "/sysctrl"
#define WDT SOC "/wdt"

#define FLASH_BASE 0x00000000
#define FLASH_SIZE (8 * 1024)
#define EEPROM_BASE (FLASH_BASE + FLASH_SIZE)
#define EEPROM_SIZE (4 * 1024)
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

static char *quote_firmware_path(const char *path)
{
#ifdef _WIN32
    return g_strdup_printf("\"%s\"", path);
#else
    return g_shell_quote(path);
#endif
}

enum {
    IRQ_INT0,
    IRQ_TIMER0,
    IRQ_INT1,
    IRQ_TIMER1,
    IRQ_UART1,
    IRQ_ADC,
    IRQ_LVD,
    IRQ_PCA,
    IRQ_SPI,
    IRQ_INT2,
    IRQ_INT3,
    IRQ_INT4,
    IRQ_I2C,
};

enum {
    INTC_ADC,
    INTC_LVD,
    INTC_PCA,
    INTC_SPI,
    INTC_INT2,
    INTC_INT3,
    INTC_INT4,
    INTC_I2C,
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

static uint64_t qom_get_uint(QTestState *qts, const char *path,
                             const char *property)
{
    QDict *response;
    uint64_t value;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-get', 'arguments': {"
                         "  'path': %s, 'property': %s } }",
                         path, property);
    g_assert_false(qdict_haskey(response, "error"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static void assert_clock_hz(QTestState *qts, const char *path, uint64_t hz)
{
    g_assert_cmphex(qom_get_uint(qts, path, "qtest-clock-period"), ==,
                    hz ? CLOCK_PERIOD_1SEC / hz : 0);
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
    g_assert_cmphex(qtest_readb(qts, SFR(0xa0)), ==, 0xff);
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

    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0xff);
    qtest_writeb(qts, FLASH_BASE, 0x00);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + FLASH_SIZE - 1),
                    ==, 0xff);

    /* Sparse GPIO and UART containers must not hide CPU-owned SFRs. */
    qtest_writeb(qts, SFR(0xa8), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa8)), ==, 0xff);
    qtest_writeb(qts, SFR(0xb8), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb8)), ==, 0xff);
    qtest_writeb(qts, SFR(0xa0), 0x03);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa0)), ==, 0x03);
    qtest_writeb(qts, SFR(0x81), 0x55);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0x81)), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa0)), ==, 0xff);

    qtest_quit(qts);
}

static void test_raw_firmware_loading(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *quoted = NULL;
    const uint8_t image[] = { 0x74, 0x5a, 0x80, 0xfe };
    QTestState *qts;

    directory = g_dir_make_tmp("stc8g-raw-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.bin", NULL);
    g_assert_true(g_file_set_contents(filename, (const char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);

    quoted = quote_firmware_path(filename);
    qts = qtest_initf(MACHINE " -bios %s", quoted);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, image[0]);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 3), ==, image[3]);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 4), ==, 0xff);
    qtest_quit(qts);

    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_hex_firmware_loading(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *quoted = NULL;
    const char hex[] = ":04000000745A80FEB0\n:00000001FF\n";
    QTestState *qts;

    directory = g_dir_make_tmp("stc8g-hex-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.hex", NULL);
    g_assert_true(g_file_set_contents(filename, hex, -1, &error));
    g_assert_no_error(error);

    quoted = quote_firmware_path(filename);
    qts = qtest_initf(MACHINE " -bios %s", quoted);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0x74);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 1), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 2), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 3), ==, 0xfe);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + 4), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + FLASH_SIZE - 1),
                    ==, 0xff);
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
        quoted = quote_firmware_path(child_filename);
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
    quoted = quote_firmware_path(filename);
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

static void test_gpio_input_reset(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, SFR(0xb1), 0x00);
    qtest_writeb(qts, SFR(0xb2), 0x01);
    qtest_writeb(qts, SFR(0xb0), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 0, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb1)), ==, 0x0c);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb2)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb0)) & 0x01, ==, 0x00);

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

        /* ITx=0 requests while the input is held low. */
        qtest_writeb(qts, SFR(0x88), 0x00);
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, flag);
        g_assert_true(qtest_get_irq(qts, irq));
        qtest_writeb(qts, SFR(0x88), 0x00);
        g_assert_true(qtest_get_irq(qts, irq));
        qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, 0);
        g_assert_false(qtest_get_irq(qts, irq));

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

static void test_interrupt_controller(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned source;

    qtest_irq_intercept_in(qts, CPU);
    g_assert_cmphex(qtest_readb(qts, SFR(0xaf)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb5)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb6)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xef)), ==, 0x00);

    qtest_writeb(qts, SFR(0xaf), 0xff);
    qtest_writeb(qts, SFR(0xb5), 0xff);
    qtest_writeb(qts, SFR(0xb6), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xaf)), ==, 0x02);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb5)), ==, 0x52);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb6)), ==, 0x52);

    for (source = INTC_ADC; source <= INTC_I2C; source++) {
        qtest_set_irq_in(qts, INTC, "irq-in", source, 1);
        g_assert_true(qtest_get_irq(qts, IRQ_ADC + source));
        if (source >= INTC_INT2 && source <= INTC_INT4) {
            g_assert_cmphex(qtest_readb(qts, SFR(0xef)), ==,
                            BIT(source));
            qtest_writeb(qts, SFR(0xef), BIT(source));
            g_assert_false(qtest_get_irq(qts, IRQ_ADC + source));
        }
        qtest_set_irq_in(qts, INTC, "irq-in", source, 0);
        g_assert_false(qtest_get_irq(qts, IRQ_ADC + source));
    }

    qtest_writeb(qts, SFR(0xef), 0x70);
    qtest_set_irq_in(qts, INTC, "irq-in", INTC_INT2, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xef)), ==, 0x10);
    qtest_set_irq_in(qts, INTC, "irq-in", INTC_INT2, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xef)), ==, 0x10);
    g_assert_true(qtest_get_irq(qts, IRQ_INT2));
    qtest_writeb(qts, SFR(0xef), 0x10);
    g_assert_cmphex(qtest_readb(qts, SFR(0xef)), ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT2));

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0xaf)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb5)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb6)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xef)), ==, 0x00);
    qtest_quit(qts);
}

static void test_intclko_irq_enable(void)
{
    static const uint8_t reset[] = { 0x02, 0x01, 0x00 };
    static const uint8_t int2_vector[] = { 0x02, 0x02, 0x00 };
    static const uint8_t main[] = { 0x75, 0xa8, 0x80, 0x80, 0xfe };
    static const uint8_t handler[] = { 0x05, 0x20, 0x75, 0xef, 0x10, 0x32 };
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *quoted = NULL;
    g_autofree uint8_t *image = g_malloc0(FLASH_SIZE);
    QTestState *qts;
    gint64 deadline;

    memset(image, 0xff, FLASH_SIZE);
    memcpy(image, reset, sizeof(reset));
    memcpy(image + 0x0053, int2_vector, sizeof(int2_vector));
    memcpy(image + 0x0100, main, sizeof(main));
    memcpy(image + 0x0200, handler, sizeof(handler));

    directory = g_dir_make_tmp("stc8g-intclko-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.bin", NULL);
    g_assert_true(g_file_set_contents(filename, (char *)image,
                                      FLASH_SIZE, &error));
    g_assert_no_error(error);
    quoted = quote_firmware_path(filename);

    qts = qtest_initf(MACHINE " -accel tcg -bios %s", quoted);
    qtest_set_irq_in(qts, INTC, "irq-in", INTC_INT2, 1);
    g_usleep(1000);
    g_assert_cmphex(qtest_readb(qts, IDATA_BASE + 0x20), ==, 0);

    qtest_writeb(qts, SFR(0x8f), BIT(4));
    deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (qtest_readb(qts, IDATA_BASE + 0x20) == 0 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, IDATA_BASE + 0x20), ==, 1);

    qtest_quit(qts);
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_level_interrupt_reasserts(void)
{
    static const uint8_t reset[] = { 0x02, 0x01, 0x00 };
    static const uint8_t int0_vector[] = { 0x02, 0x02, 0x00 };
    static const uint8_t main[] = { 0x75, 0xa8, 0x81, 0x80, 0xfe };
    static const uint8_t handler[] = {
        0x05, 0x20, 0xe5, 0x20, 0xb4, 0x02, 0x03, 0x75, 0xa8, 0x00,
        0x32,
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *quoted = NULL;
    g_autofree uint8_t *image = g_malloc0(FLASH_SIZE);
    QTestState *qts;
    gint64 deadline;

    memset(image, 0xff, FLASH_SIZE);
    memcpy(image, reset, sizeof(reset));
    memcpy(image + 0x0003, int0_vector, sizeof(int0_vector));
    memcpy(image + 0x0100, main, sizeof(main));
    memcpy(image + 0x0200, handler, sizeof(handler));

    directory = g_dir_make_tmp("stc8g-level-int-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.bin", NULL);
    g_assert_true(g_file_set_contents(filename, (char *)image,
                                      FLASH_SIZE, &error));
    g_assert_no_error(error);
    quoted = quote_firmware_path(filename);

    qts = qtest_initf(MACHINE " -accel tcg -bios %s", quoted);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 2, 0);
    deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (qtest_readb(qts, IDATA_BASE + 0x20) != 2 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, IDATA_BASE + 0x20), ==, 2);

    qtest_quit(qts);
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_adc(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_irq_intercept_in(qts, CPU);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbd)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbe)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xde)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfea8)), ==, 0x2a);

    qtest_set_irq_in(qts, ADC, "adc-in", 0, 0x155);
    qtest_writeb(qts, SFR(0xbc), 0xc0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xc0);
    qtest_clock_step(qts, 1010000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xa0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbd)), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbe)), ==, 0x40);
    g_assert_true(qtest_get_irq(qts, IRQ_ADC));

    qtest_writeb(qts, SFR(0xbc), 0x80);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0x80);
    g_assert_false(qtest_get_irq(qts, IRQ_ADC));

    qtest_set_irq_in(qts, ADC, "adc-in", 1, 0x2ab);
    qtest_writeb(qts, SFR(0xde), 0x20);
    qtest_writeb(qts, SFR(0xbc), 0xc1);
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbd)), ==, 0x02);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbe)), ==, 0xab);

    /* In-flight conversions retain their cycle count across clock changes. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xbc), 0x80);
    qtest_clock_step(qts, 1000000);
    qtest_writeb(qts, SFR(0xbc), 0xc0);
    qtest_clock_step(qts, 1000);
    qtest_writeb(qts, XFR(0xfe01), 0x02);
    qtest_clock_step(qts, 1999);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xc0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xa0);

    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xbc), 0x80);
    qtest_clock_step(qts, 1000000);
    qtest_writeb(qts, SFR(0xbc), 0xc0);
    qtest_clock_step(qts, 1000);
    qtest_writeb(qts, XFR(0xfe00), 0x03);
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xc0);
    qtest_writeb(qts, XFR(0xfe00), 0x00);
    qtest_clock_step(qts, 999);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xc0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xa0);

    /* START set with a stopped clock resumes when sysclk restarts. */
    qtest_system_reset(qts);
    qtest_writeb(qts, XFR(0xfe00), 0x03);
    qtest_writeb(qts, SFR(0xbc), 0xc0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xc0);
    qtest_writeb(qts, XFR(0xfe00), 0x00);
    qtest_clock_step(qts, 1999);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xc0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0xa0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0xbc)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfea8)), ==, 0x2a);
    qtest_quit(qts);
}

static void test_sysctrl(void)
{
    static const char * const sysclk_consumers[] = {
        ADC "/sysclk", I2C "/sysclk", MDU "/sysclk", PCA "/sysclk",
        SPI "/sysclk", SOC "/timer/sysclk",
    };
    QTestState *qts = qtest_init(MACHINE);
    unsigned index;

    g_assert_cmphex(qtest_readb(qts, XFR(0xfe00)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe01)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe02)), ==, 0x81);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe03)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe04)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe05)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe06)), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, SFR(0x9d)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x9e)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x9f)), ==, 0x80);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 24000000);
    assert_clock_hz(qts, SYSCTRL "/mclko", 0);

    qtest_writeb(qts, XFR(0xfe05), 0x82);
    assert_clock_hz(qts, SYSCTRL "/mclko", 12000000);
    qtest_writeb(qts, XFR(0xfe01), 0x08);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 3000000);
    assert_clock_hz(qts, SYSCTRL "/mclko", 1500000);
    for (index = 0; index < ARRAY_SIZE(sysclk_consumers); index++) {
        assert_clock_hz(qts, sysclk_consumers[index], 3000000);
    }
    qtest_writeb(qts, SFR(0x9f), 0x81);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 3007200);
    qtest_writeb(qts, SFR(0x9d), 0x01);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 4134900);
    qtest_writeb(qts, SFR(0x9d), 0x00);
    qtest_writeb(qts, SFR(0x9f), 0x80);

    qtest_writeb(qts, XFR(0xfe03), 0x80);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe03)), ==, 0x81);
    qtest_writeb(qts, XFR(0xfe00), 0x01);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 3000000);
    qtest_writeb(qts, XFR(0xfe00), 0x03);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 0);
    qtest_writeb(qts, XFR(0xfe04), 0x80);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe04)), ==, 0x81);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 4096);
    qtest_writeb(qts, XFR(0xfe01), 0xff);
    g_assert_cmpuint(qom_get_uint(qts, SYSCTRL "/sysclk",
                                  "qtest-clock-period"),
                     >, CLOCK_PERIOD_1SEC / 129);
    g_assert_cmpuint(qom_get_uint(qts, SYSCTRL "/sysclk",
                                  "qtest-clock-period"),
                     <, CLOCK_PERIOD_1SEC / 128);
    for (index = 0; index < ARRAY_SIZE(sysclk_consumers); index++) {
        g_assert_cmphex(qom_get_uint(qts, sysclk_consumers[index],
                                     "qtest-clock-period"),
                        ==, qom_get_uint(qts, SYSCTRL "/sysclk",
                                          "qtest-clock-period"));
    }

    qtest_system_reset(qts);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 24000000);
    assert_clock_hz(qts, SYSCTRL "/mclko", 0);
    qtest_quit(qts);
}

static void test_power_modes(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, SFR(0x87), 0x31);
    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0x31);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 24000000);

    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0x87), 0x32);
    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0x32);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 0);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0x30);
    assert_clock_hz(qts, SYSCTRL "/sysclk", 24000000);
    qtest_quit(qts);
}

static void test_spi(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_irq_intercept_in(qts, CPU);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xce)), ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcf)), ==, 0x00);

    qtest_writeb(qts, SFR(0xce), 0xd0);
    qtest_writeb(qts, SFR(0xcf), 0x5a);
    qtest_writeb(qts, SFR(0xcf), 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcf)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x40);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x40);
    qtest_clock_step(qts, 500);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcf)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0xc0);
    g_assert_true(qtest_get_irq(qts, IRQ_SPI));
    qtest_writeb(qts, SFR(0xcd), 0xc0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_SPI));

    qtest_writeb(qts, SFR(0xce), 0x50);
    qtest_set_irq_in(qts, SPI, "ss-in", 0, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xce)), ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x80);
    qtest_writeb(qts, SFR(0xcd), 0x80);
    qtest_set_irq_in(qts, SPI, "slave-data", 0, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcf)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x80);
    g_assert_true(qtest_get_irq(qts, IRQ_SPI));

    /* Transfers retain cycles across a sysclk frequency change or stop. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xce), 0xd0);
    qtest_writeb(qts, SFR(0xcf), 0x5a);
    qtest_clock_step(qts, 500);
    qtest_writeb(qts, XFR(0xfe01), 0x02);
    qtest_clock_step(qts, 1666);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x00);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x80);

    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xce), 0xd0);
    qtest_writeb(qts, SFR(0xcf), 0xa5);
    qtest_clock_step(qts, 500);
    qtest_writeb(qts, XFR(0xfe00), 0x03);
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x00);
    qtest_writeb(qts, XFR(0xfe00), 0x00);
    qtest_clock_step(qts, 833);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x00);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x80);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0xcd)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xce)), ==, 0x04);
    qtest_quit(qts);
}

static void mdu_write32(QTestState *qts, uint32_t value)
{
    qtest_writeb(qts, XFR(0xfcf0), value >> 24);
    qtest_writeb(qts, XFR(0xfcf1), value >> 16);
    qtest_writeb(qts, XFR(0xfcf2), value >> 8);
    qtest_writeb(qts, XFR(0xfcf3), value);
}

static uint32_t mdu_read32(QTestState *qts)
{
    return qtest_readb(qts, XFR(0xfcf0)) << 24 |
           qtest_readb(qts, XFR(0xfcf1)) << 16 |
           qtest_readb(qts, XFR(0xfcf2)) << 8 |
           qtest_readb(qts, XFR(0xfcf3));
}

static void mdu_write16(QTestState *qts, uint16_t value)
{
    qtest_writeb(qts, XFR(0xfcf4), value >> 8);
    qtest_writeb(qts, XFR(0xfcf5), value);
}

static uint16_t mdu_read16(QTestState *qts)
{
    return qtest_readb(qts, XFR(0xfcf4)) << 8 |
           qtest_readb(qts, XFR(0xfcf5));
}

static void mdu_start(QTestState *qts, uint8_t arcon)
{
    qtest_writeb(qts, XFR(0xfcf6), arcon);
    qtest_writeb(qts, XFR(0xfcf7), 0x01);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)) & 0x01, ==, 0x01);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)) & 0x01, ==, 0x00);
}

static void test_mdu(void)
{
    QTestState *qts = qtest_init(MACHINE);

    mdu_write32(qts, 0x00001234);
    mdu_write16(qts, 0x0010);
    mdu_start(qts, 4 << 5);
    g_assert_cmphex(mdu_read32(qts), ==, 0x00012340);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)) & 0x40, ==, 0x40);

    mdu_write32(qts, 0x0000f00d);
    mdu_write16(qts, 0x0010);
    mdu_start(qts, 5 << 5);
    g_assert_cmphex(mdu_read32(qts) & 0xffff, ==, 0x0f00);
    g_assert_cmphex(mdu_read16(qts), ==, 0x000d);

    mdu_write32(qts, 0x12345678);
    mdu_write16(qts, 0x0010);
    mdu_start(qts, 6 << 5);
    g_assert_cmphex(mdu_read32(qts), ==, 0x01234567);
    g_assert_cmphex(mdu_read16(qts), ==, 0x0008);

    mdu_write32(qts, 0x00100000);
    mdu_start(qts, (2 << 5) | 4);
    g_assert_cmphex(mdu_read32(qts), ==, 0x01000000);
    mdu_write32(qts, 0x00001234);
    mdu_start(qts, 3 << 5);
    g_assert_cmphex(mdu_read32(qts), ==, 0x91a00000);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf6)) & 0x1f, ==, 19);

    mdu_write32(qts, 0x01000000);
    mdu_write16(qts, 0x0000);
    mdu_start(qts, 6 << 5);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)) & 0x40, ==, 0x40);
    qtest_writeb(qts, XFR(0xfcf7), 0x02);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf6)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)), ==, 0x00);

    /* MDU operation cycles pause and resume with the system clock. */
    mdu_write32(qts, 0x12345678);
    mdu_write16(qts, 0x0010);
    qtest_writeb(qts, XFR(0xfcf6), 6 << 5);
    qtest_writeb(qts, XFR(0xfcf7), 0x01);
    qtest_clock_step(qts, 300);
    qtest_writeb(qts, XFR(0xfe01), 0x02);
    qtest_clock_step(qts, 816);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)) & 0x01, ==, 0x01);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)) & 0x01, ==, 0x00);

    /* A non-integral oscillator/divider ratio retains its exact period. */
    qtest_system_reset(qts);
    qtest_writeb(qts, XFR(0xfe04), 0x80);
    qtest_writeb(qts, XFR(0xfe00), 0x03);
    qtest_writeb(qts, XFR(0xfe01), 0xff);
    mdu_write32(qts, 0x12345678);
    mdu_write16(qts, 0x0010);
    qtest_writeb(qts, XFR(0xfcf6), 6 << 5);
    qtest_writeb(qts, XFR(0xfcf7), 0x01);
    qtest_clock_step(qts, 132500000);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfcf7)) & 0x01, ==, 0x00);

    qtest_system_reset(qts);
    g_assert_cmphex(mdu_read32(qts), ==, 0x00000000);
    g_assert_cmphex(mdu_read16(qts), ==, 0x0000);
    qtest_quit(qts);
}

static void test_i2c(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_irq_intercept_in(qts, CPU);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe80)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe88)), ==, 0x00);
    qtest_writeb(qts, XFR(0xfe80), 0xc0);
    qtest_writeb(qts, XFR(0xfe86), 0xa0);
    qtest_writeb(qts, XFR(0xfe81), 0x89);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0x80);
    qtest_clock_step(qts, 3000);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0x80);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0xc2);
    g_assert_true(qtest_get_irq(qts, IRQ_I2C));
    qtest_writeb(qts, XFR(0xfe82), 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0x82);
    g_assert_false(qtest_get_irq(qts, IRQ_I2C));
    qtest_writeb(qts, XFR(0xfe81), 0x86);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0x42);

    qtest_writeb(qts, XFR(0xfe82), 0x00);
    qtest_writeb(qts, XFR(0xfe81), 0x81);
    qtest_clock_step(qts, 1000);
    qtest_writeb(qts, XFR(0xfe82), 0x00);
    qtest_writeb(qts, XFR(0xfe88), 0x01);
    qtest_writeb(qts, XFR(0xfe86), 0xa0);
    qtest_clock_step(qts, 4000);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0xc2);

    qtest_writeb(qts, XFR(0xfe80), 0x80);
    qtest_writeb(qts, XFR(0xfe83), 0x78);
    qtest_set_irq_in(qts, I2C, "slave-event", 0, 1);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe84)), ==, 0xc0);
    g_assert_true(qtest_get_irq(qts, IRQ_I2C));
    qtest_writeb(qts, XFR(0xfe84), 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe84)), ==, 0x80);
    qtest_set_irq_in(qts, I2C, "slave-data", 0, 0xa5);
    qtest_set_irq_in(qts, I2C, "slave-event", 0, 2);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe87)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe84)), ==, 0xa0);
    qtest_writeb(qts, XFR(0xfe84), 0x00);
    qtest_set_irq_in(qts, I2C, "slave-ack", 0, 1);
    qtest_set_irq_in(qts, I2C, "slave-event", 0, 3);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe84)), ==, 0x92);
    qtest_writeb(qts, XFR(0xfe84), 0x00);
    qtest_set_irq_in(qts, I2C, "slave-event", 0, 4);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe84)), ==, 0x0a);
    qtest_writeb(qts, XFR(0xfe83), 0x01);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe83)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe84)), ==, 0x00);

    qtest_system_reset(qts);
    qtest_writeb(qts, XFR(0xfe80), 0xc0);
    qtest_writeb(qts, XFR(0xfe81), 0x81);
    qtest_clock_step(qts, 300);
    qtest_writeb(qts, XFR(0xfe01), 0x02);
    qtest_clock_step(qts, 733);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0x80);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe82)), ==, 0xc0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, XFR(0xfe80)), ==, 0x00);
    qtest_quit(qts);
}

static void test_pca(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_irq_intercept_in(qts, CPU);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd9)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xda)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xf9)), ==, 0x00);

    /* A compare match raises CCF0 and drives the shared PCA interrupt. */
    qtest_writeb(qts, SFR(0xea), 0x02);
    qtest_writeb(qts, SFR(0xfa), 0x00);
    qtest_writeb(qts, SFR(0xda), 0x49);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x41);
    g_assert_true(qtest_get_irq(qts, IRQ_PCA));
    qtest_writeb(qts, SFR(0xd8), 0x40);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x40);
    g_assert_false(qtest_get_irq(qts, IRQ_PCA));

    /* CAPP/CAPN capture the current counter value on their selected edges. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd9), 0x06);
    qtest_writeb(qts, SFR(0xda), 0x31);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_set_irq_in(qts, PCA, "eci", 0, 1);
    qtest_set_irq_in(qts, PCA, "eci", 0, 0);
    qtest_set_irq_in(qts, PCA, "ccp-in", 0, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xea)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0xfa)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x41);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_set_irq_in(qts, PCA, "eci", 0, 1);
    qtest_set_irq_in(qts, PCA, "eci", 0, 0);
    qtest_set_irq_in(qts, PCA, "ccp-in", 0, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xea)), ==, 0x02);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x41);

    /* Preserve externally driven edge history over a system reset. */
    qtest_system_reset(qts);
    qtest_set_irq_in(qts, PCA, "eci", 0, 1);
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd9), 0x06);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_set_irq_in(qts, PCA, "eci", 0, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x01);

    qtest_system_reset(qts);
    qtest_set_irq_in(qts, PCA, "ccp-in", 0, 1);
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xda), 0x11);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_set_irq_in(qts, PCA, "ccp-in", 0, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x41);

    qtest_quit(qts);

    qts = qtest_init(MACHINE);
    qtest_irq_intercept_out_named(qts, PCA, "ccp-out");

    /* PWM reloads at period boundaries and reports configured output edges. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd9), 0x08);
    qtest_writeb(qts, SFR(0xea), 0x04);
    qtest_writeb(qts, SFR(0xfa), 0x04);
    qtest_writeb(qts, SFR(0xda), 0x73);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 167);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x41);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_clock_step(qts, 10500);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x41);

    /* CMOD.CIDL stops the PCA in idle mode; a clear bit keeps it running. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x02);
    qtest_writeb(qts, SFR(0x87), 0x01);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x04);
    qtest_writeb(qts, SFR(0x87), 0x00);
    qtest_clock_step(qts, 500);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x05);

    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd9), 0x80);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x02);
    qtest_writeb(qts, SFR(0x87), 0x01);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x02);
    qtest_writeb(qts, SFR(0x87), 0x00);
    qtest_clock_step(qts, 500);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x03);

    /* Reads retain incomplete /12 source-clock intervals. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    for (unsigned index = 0; index < 10; index++) {
        qtest_clock_step(qts, 400);
        qtest_readb(qts, SFR(0xe9));
    }
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x08);

    /* Changing CMOD.CPS discards the phase of the old divider. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_clock_step(qts, 400);
    qtest_writeb(qts, SFR(0xd9), 0x02);
    qtest_clock_step(qts, 100);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x01);

    /* CMOD.CPS=010 clocks PCA from the actual timer-0 overflow pulse. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd9), 0x04);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_writeb(qts, SFR(0x89), 0x01);
    qtest_writeb(qts, SFR(0x8a), 0xfe);
    qtest_writeb(qts, SFR(0x8c), 0xff);
    qtest_writeb(qts, SFR(0x88), 0x10);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x01);

    /* Each timer-0 overflow clocks the PCA, including a long time step. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd9), 0x04);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_writeb(qts, SFR(0x89), 0x02);
    qtest_writeb(qts, SFR(0x8a), 0xff);
    qtest_writeb(qts, SFR(0x8c), 0xff);
    qtest_writeb(qts, SFR(0x88), 0x10);
    qtest_clock_step(qts, 4000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x08);

    /* A long run of one-tick Timer 0 periods preserves all PCA pulses. */
    qtest_system_reset(qts);
    qtest_writeb(qts, SFR(0xd9), 0x04);
    qtest_writeb(qts, SFR(0xd8), 0x40);
    qtest_writeb(qts, SFR(0x89), 0x00);
    timer_set_count(qts, 0, 0xff, 0xff);
    qtest_writeb(qts, SFR(0x88), 0x10);
    timer_set_count(qts, 0, 0xff, 0xff);
    qtest_writeb(qts, SFR(0x8e), 0x81);
    qtest_clock_step(qts, 1000000000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe9)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xf9)), ==, 0x36);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd9)), ==, 0x00);
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

        qtest_set_irq_in(qts, GPIO, "gpio-in", gate_pin, 0);
        qtest_system_reset(qts);
        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
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

        qtest_system_reset(qts);
        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_set_irq_in(qts, GPIO, "gpio-in", counter_pin, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);
        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0x00);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));

        qtest_system_reset(qts);
        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_set_irq_in(qts, GPIO, "gpio-in", counter_pin, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfe);
        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);

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

static void test_wdt(void)
{
    QDict *event;
    QTestState *qts = qtest_init(MACHINE " -watchdog-action none");

    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0);
    assert_clock_hz(qts, WDT "/sysclk", 24000000);
    qtest_writeb(qts, SFR(0xc1), 0x20);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0x20);
    qtest_clock_step(qts, 32767999);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0x20);
    qtest_clock_step(qts, 1);
    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "action"),
                    ==, "none");
    qobject_unref(event);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0xa0);

    qtest_writeb(qts, SFR(0xc1), 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0x20);
    qtest_quit(qts);

    qts = qtest_init(MACHINE " -watchdog-action none");
    qtest_writeb(qts, SFR(0xc1), 0x20);
    qtest_clock_step(qts, 10000000);
    qtest_writeb(qts, SFR(0x87), 0x01);
    qtest_clock_step(qts, 32768000);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0x20);
    qtest_writeb(qts, SFR(0x87), 0x00);
    qtest_clock_step(qts, 22767999);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0x20);
    qtest_clock_step(qts, 1);
    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "action"),
                    ==, "none");
    qobject_unref(event);
    qtest_quit(qts);

    qts = qtest_init(MACHINE " -watchdog-action none");
    qtest_writeb(qts, SFR(0xc1), 0x20);
    for (unsigned i = 0; i < 10; i++) {
        qtest_clock_step(qts, 1);
        qtest_writeb(qts, SFR(0xc1), 0x20);
    }
    qtest_clock_step(qts, 32767989);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0x20);
    qtest_clock_step(qts, 1);
    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "action"),
                    ==, "none");
    qobject_unref(event);
    qtest_quit(qts);

    qts = qtest_init(MACHINE " -watchdog-action none");
    qtest_writeb(qts, XFR(0xfe01), 2);
    assert_clock_hz(qts, WDT "/sysclk", 12000000);
    qtest_writeb(qts, SFR(0xc1), 0x20);
    qtest_clock_step(qts, 65536000);
    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "action"),
                    ==, "none");
    qobject_unref(event);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0xa0);
    qtest_quit(qts);

    qts = qtest_init(MACHINE " -watchdog-action reset");
    qtest_writeb(qts, SFR(0xc1), 0x20);
    qtest_clock_step(qts, 32768000);
    event = qtest_qmp_eventwait_ref(qts, "WATCHDOG");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(event, "data"), "action"),
                    ==, "reset");
    qobject_unref(event);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readb(qts, SFR(0xc1)), ==, 0xa0);
    qtest_quit(qts);
}

static void iap_execute(QTestState *qts, uint8_t command, uint16_t address,
                        uint8_t data)
{
    qtest_writeb(qts, SFR(0xc7), 0x80);
    qtest_writeb(qts, SFR(0xf5), 24);
    qtest_writeb(qts, SFR(0xc5), command);
    qtest_writeb(qts, SFR(0xc3), address >> 8);
    qtest_writeb(qts, SFR(0xc4), address);
    qtest_writeb(qts, SFR(0xc2), data);
    qtest_writeb(qts, SFR(0xc6), 0x5a);
    qtest_writeb(qts, SFR(0xc6), 0xa5);
}

static void test_iap(void)
{
    QTestState *qts = qtest_init(MACHINE);

    g_assert_cmphex(qtest_readb(qts, SFR(0xc2)), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + EEPROM_SIZE - 1),
                    ==, 0xff);
    assert_clock_hz(qts, IAP "/sysclk", 24000000);

    iap_execute(qts, 2, 0x0234, 0x5a);
    qtest_clock_step(qts, 31000);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0234), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0200), ==, 0xff);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0234), ==, 0x5a);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc2)), ==, 0xff);

    iap_execute(qts, 2, 0x0234, 0xf0);
    qtest_clock_step(qts, 31000);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0234), ==, 0x50);

    /* PROGRAM remains on its fixed deadline across a system-clock change. */
    iap_execute(qts, 2, 0x0235, 0x5a);
    qtest_clock_step(qts, 15000);
    qtest_writeb(qts, XFR(0xfe01), 0x02);
    qtest_clock_step(qts, 15999);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0235), ==, 0xff);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0235), ==, 0x5a);
    qtest_writeb(qts, XFR(0xfe01), 0x00);

    iap_execute(qts, 1, 0x0234, 0);
    qtest_clock_step(qts, 167);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc2)), ==, 0x50);

    iap_execute(qts, 1, 0x0234, 0);
    qtest_clock_step(qts, 83);
    qtest_writeb(qts, XFR(0xfe01), 0x02);
    qtest_clock_step(qts, 167);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc2)), ==, 0x00);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc2)), ==, 0x50);
    qtest_writeb(qts, XFR(0xfe01), 0x00);

    iap_execute(qts, 2, 0x0200, 0x00);
    qtest_clock_step(qts, 31000);
    iap_execute(qts, 3, 0x0234, 0);
    qtest_clock_step(qts, 4571000);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0200), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0234), ==, 0xff);

    qtest_writeb(qts, SFR(0xc3), 0x10);
    qtest_writeb(qts, SFR(0xc4), 0x00);
    qtest_writeb(qts, SFR(0xc5), 1);
    qtest_writeb(qts, SFR(0xc6), 0x5a);
    qtest_writeb(qts, SFR(0xc6), 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc7)), ==, 0x90);
    qtest_writeb(qts, SFR(0xc7), 0x80);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc7)), ==, 0x80);

    qtest_writeb(qts, SFR(0xc7), 0xa0);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readb(qts, EEPROM_BASE + 0x0234), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xc7)), ==, 0);
    qtest_quit(qts);
}

static void test_lvd(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_irq_intercept_in(qts, CPU);
    g_assert_cmphex(qtest_readb(qts, SFR(0xff)), ==, 0);
    qtest_writeb(qts, SFR(0x87), 0x10);
    g_assert_false(qtest_get_irq(qts, IRQ_LVD));
    qtest_writeb(qts, SFR(0xff), 0x03);
    qtest_set_irq_in(qts, LVD, "vdd-millivolts", 0, 2999);
    g_assert_cmphex(qtest_readb(qts, SFR(0x87)) & 0x20, ==, 0x20);
    g_assert_true(qtest_get_irq(qts, IRQ_LVD));
    qtest_writeb(qts, SFR(0x87), 0x10);
    g_assert_false(qtest_get_irq(qts, IRQ_LVD));
    qtest_set_irq_in(qts, LVD, "vdd-millivolts", 0, 3300);
    qtest_set_irq_in(qts, LVD, "vdd-millivolts", 0, 2999);
    g_assert_true(qtest_get_irq(qts, IRQ_LVD));
    qtest_writeb(qts, SFR(0xff), 0x13);
    g_assert_cmphex(qtest_readb(qts, SFR(0xff)), ==, 0x13);
    qtest_quit(qts);

    qts = qtest_init(MACHINE);
    qtest_writeb(qts, SFR(0xff), 0x43);
    qtest_set_irq_in(qts, LVD, "vdd-millivolts", 0, 2999);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readb(qts, SFR(0xff)), ==, 0);
    qtest_quit(qts);
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
    qtest_add_func("/stc8g/gpio/input-reset", test_gpio_input_reset);
    qtest_add_func("/stc8g/gpio/modes", test_gpio_modes);
    qtest_add_func("/stc8g/gpio/pullup-and-input-enable",
                   test_gpio_pullup_and_input_enable);
    qtest_add_func("/stc8g/gpio/external-interrupts",
                   test_gpio_external_interrupts);
    qtest_add_func("/stc8g/intc/registers-and-sources",
                   test_interrupt_controller);
    qtest_add_func("/stc8g/intc/intclko-enable", test_intclko_irq_enable);
    qtest_add_func("/stc8g/intc/level-interrupt-reasserts",
                   test_level_interrupt_reasserts);
    qtest_add_func("/stc8g/adc", test_adc);
    qtest_add_func("/stc8g/sysctrl", test_sysctrl);
    qtest_add_func("/stc8g/power-modes", test_power_modes);
    qtest_add_func("/stc8g/mdu", test_mdu);
    qtest_add_func("/stc8g/i2c", test_i2c);
    qtest_add_func("/stc8g/pca", test_pca);
    qtest_add_func("/stc8g/spi", test_spi);
    qtest_add_func("/stc8g/timer/modes", test_timer_modes);
    qtest_add_func("/stc8g/timer/reload-and-gates",
                   test_timer_reload_and_gates);
    qtest_add_func("/stc8g/timer/counters-and-rates",
                   test_timer_counters_and_rates);
    qtest_add_func("/stc8g/uart1/transmit", test_uart1_transmit);
    qtest_add_func("/stc8g/uart1/receive", test_uart1_receive);
    qtest_add_func("/stc8g/wdt", test_wdt);
    qtest_add_func("/stc8g/iap", test_iap);
    qtest_add_func("/stc8g/lvd", test_lvd);

    return g_test_run();
}
