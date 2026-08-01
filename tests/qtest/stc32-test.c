/*
 * STC32G144K246 machine tests
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

#define MACHINE "-M stc32g144k246"
#define SOC "/machine/soc"
#define CPU SOC "/cpu"
#define GPIO SOC "/gpio"

#define FLASH_BASE 0x00fc2800
#define FLASH_SIZE (246 * 1024)
#define RESET_PC 0x00ff0000
#define SOURCE_OPCODE_BASE RESET_PC
#define BINARY_OPCODE_BASE (RESET_PC + 0x1000)
#define OPERAND_VECTOR_BASE (RESET_PC + 0x2000)
#define OPCODE_SLOT_SIZE 8
#define INVALID_HEX_ENV "QTEST_STC32_INVALID_HEX"

#define SFR_BASE 0x01000000
#define SFR(address) (SFR_BASE + (address) - 0x80)

#define DPUST_Z BIT(7)
#define DPUST_DBZ BIT(6)
#define DPUST_SC_MASK MAKE_64BIT_MASK(0, 6)

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
};

static const uint8_t gpio_data_address[] = {
    0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xc8, 0xe8, 0xf8,
};

static const uint8_t gpio_mode1_address[] = {
    0x93, 0x91, 0x95, 0xb1, 0xb3, 0xc9, 0xcb, 0xe1,
};

static const uint8_t gpio_mode0_address[] = {
    0x94, 0x92, 0x96, 0xb2, 0xb4, 0xca, 0xcc, 0xe2,
};

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

enum DSPRegister {
    DSP_EBX,
    DSP_EAX,
    DSP_EDX,
    DSP_ECX,
};

typedef struct DSPAccumulateVector {
    uint8_t command;
    enum DSPRegister target;
    uint32_t input;
    uint32_t expected;
    bool zero;
} DSPAccumulateVector;

static const uint8_t dsp_register_base[] = {
    [DSP_EBX] = 8,
    [DSP_EAX] = 12,
    [DSP_EDX] = 0,
    [DSP_ECX] = 4,
};

static void dsp_write_data32(QTestState *qts, unsigned base,
                             uint32_t value)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        qtest_writeb(qts, base + i, value >> ((3 - i) * 8));
    }
}

static uint32_t dsp_read_data32(QTestState *qts, unsigned base)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < 4; i++) {
        value = (value << 8) | qtest_readb(qts, base + i);
    }
    return value;
}

static void dsp_write32(QTestState *qts, enum DSPRegister reg,
                        uint32_t value)
{
    dsp_write_data32(qts, dsp_register_base[reg], value);
}

static uint32_t dsp_read32(QTestState *qts, enum DSPRegister reg)
{
    return dsp_read_data32(qts, dsp_register_base[reg]);
}

static uint16_t dsp_read16(QTestState *qts, enum DSPRegister reg)
{
    return dsp_read32(qts, reg);
}

static void dsp_command(QTestState *qts, uint8_t command)
{
    qtest_writeb(qts, SFR(0xd8), command);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, command);
}

static QTestState *dsp_test_init(void)
{
    QTestState *qts = qtest_init(MACHINE);

    /*
     * EAX/EBX use current DATA bank 1. Select CDRS=0 so EDX/ECX use
     * fixed DATA bank 0 and do not alias EAX/EBX.
     */
    qtest_writeb(qts, SFR(0xd0), 0x08);
    qtest_writeb(qts, SFR(0xe0), 0x00);
    dsp_command(qts, 0x80);
    return qts;
}

static void dsp_set_flags(QTestState *qts, bool carry)
{
    qtest_writeb(qts, SFR(0xd0), 0x08 | (carry ? 0x80 : 0));
}

static void dsp_assert_flags(QTestState *qts, bool carry, bool overflow,
                             bool zero)
{
    uint8_t psw = qtest_readb(qts, SFR(0xd0));
    uint8_t dpust = qtest_readb(qts, SFR(0x86));

    g_assert_cmpint(!!(psw & 0x80), ==, carry);
    g_assert_cmpint(!!(psw & 0x04), ==, overflow);
    g_assert_cmpint(!!(dpust & DPUST_Z), ==, zero);
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

static void test_reset_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned port;

    g_assert_cmphex(qtest_readb(qts, SFR(0x81)), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, SFR(0x82)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x83)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x84)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x85)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x86)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x89)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8a)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8b)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8c)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8d)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8e)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8f)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x97)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb7)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xba)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd0)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd1)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe0)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xea)), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, SFR(0xeb)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0xed)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xf0)), ==, 0x00);

    for (port = 0; port < G_N_ELEMENTS(gpio_data_address); port++) {
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_data_address[port])),
                        ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode1_address[port])),
                        ==, port == 3 ? 0xfc : 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode0_address[port])),
                        ==, 0x00);
    }
    g_assert_cmphex(qtest_readb(qts, 0x7efea0), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, 0x7efea1), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, 0x7efe93), ==, 0x00);

    qtest_writeb(qts, SFR(0x81), 0x55);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0x81)), ==, 0x07);

    qtest_quit(qts);
}

static void test_memory_regions(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, 0x000123, 0x12);
    qtest_writeb(qts, 0x010123, 0x34);
    g_assert_cmphex(qtest_readb(qts, 0x000123), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, 0x010123), ==, 0x34);

    qtest_writeb(qts, 0x030123, 0x56);
    g_assert_cmphex(qtest_readb(qts, 0x800123), ==, 0x56);
    g_assert_cmphex(qtest_readb(qts, 0x030123), ==, 0x56);

    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE + FLASH_SIZE - 1),
                    ==, 0xff);

    qtest_quit(qts);
}

static void test_hex_firmware_loading(void)
{
    static const char hex[] =
        ":0200000400FFFB\n"
        ":0400000001020304F2\n"
        ":00000001FF\n";
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *quoted = NULL;
    QTestState *qts;

    directory = g_dir_make_tmp("stc32-hex-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.hex", NULL);
    g_file_set_contents(filename, hex, -1, &error);
    g_assert_no_error(error);
    quoted = quote_firmware_path(filename);

    qts = qtest_initf(MACHINE " -S -bios %s", quoted);
    g_assert_cmphex(qtest_readb(qts, 0xff0000), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, 0xff0001), ==, 0x02);
    g_assert_cmphex(qtest_readb(qts, 0xff0002), ==, 0x03);
    g_assert_cmphex(qtest_readb(qts, 0xff0003), ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, 0xff0004), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, FLASH_BASE), ==, 0xff);
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
        qts = qtest_initf(MACHINE " -S -bios %s", quoted);
        qtest_quit(qts);
        g_error("invalid Intel HEX firmware was accepted");
    }

    directory = g_dir_make_tmp("stc32-invalid-hex-XXXXXX", &error);
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

static bool source_opcode_is_defined(uint8_t opcode)
{
    if ((opcode & 0x0f) <= 5) {
        return true;
    }

    switch (opcode) {
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x0e:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x1e:
    case 0x28:
    case 0x29:
    case 0x2c:
    case 0x2d:
    case 0x2e:
    case 0x2f:
    case 0x38:
    case 0x39:
    case 0x3e:
    case 0x48:
    case 0x49:
    case 0x4c:
    case 0x4d:
    case 0x4e:
    case 0x58:
    case 0x59:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x68:
    case 0x69:
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x78:
    case 0x79:
    case 0x7a:
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f:
    case 0x89:
    case 0x8a:
    case 0x8c:
    case 0x8d:
    case 0x99:
    case 0x9a:
    case 0x9c:
    case 0x9d:
    case 0x9e:
    case 0x9f:
    case 0xa9:
    case 0xaa:
    case 0xac:
    case 0xad:
    case 0xb9:
    case 0xbc:
    case 0xbd:
    case 0xbe:
    case 0xbf:
    case 0xca:
    case 0xda:
        return true;
    default:
        return false;
    }
}

static unsigned source_primary_length(uint8_t opcode)
{
    if (opcode == 0xa5) {
        return 2;
    }
    if ((opcode & 0x0f) <= 5) {
        return classic_opcode_length(opcode);
    }

    switch (opcode) {
    case 0x08:
    case 0x18:
    case 0x28:
    case 0x38:
    case 0x48:
    case 0x58:
    case 0x68:
    case 0x78:
    case 0x0a:
    case 0x1a:
    case 0x0b:
    case 0x1b:
    case 0x0e:
    case 0x1e:
    case 0x3e:
    case 0x2c:
    case 0x2d:
    case 0x2f:
    case 0x4c:
    case 0x4d:
    case 0x5c:
    case 0x5d:
    case 0x6c:
    case 0x6d:
    case 0x7c:
    case 0x7d:
    case 0x7f:
    case 0x89:
    case 0x8c:
    case 0x8d:
    case 0x99:
    case 0x9c:
    case 0x9d:
    case 0x9f:
    case 0xac:
    case 0xad:
    case 0xbc:
    case 0xbd:
    case 0xbf:
    case 0xca:
    case 0xda:
        return 2;
    case 0x09:
    case 0x19:
    case 0x29:
    case 0x39:
    case 0x49:
    case 0x59:
    case 0x69:
    case 0x79:
    case 0x8a:
    case 0x9a:
        return 4;
    case 0x2e:
    case 0x4e:
    case 0x5e:
    case 0x6e:
    case 0x7e:
    case 0x9e:
    case 0xbe:
    case 0xa9:
        return 3;
    case 0x7a:
        return 3;
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

typedef struct MCS251DisasVector {
    bool binary_mode;
    uint8_t length;
    uint8_t bytes[8];
    const char *expected;
} MCS251DisasVector;

static const MCS251DisasVector disas_vectors[] = {
    { true, 3, { 0x43, 0x20, 0x55 }, "orl 0x20,#0x55" },
    { true, 3, { 0x85, 0x20, 0x21 }, "mov 0x21,0x20" },
    { true, 3, { 0xb4, 0x12, 0x00 }, "cjne a,#0x12," },
    { false, 4, { 0x09, 0x12, 0x00, 0x10 },
      "mov r1,@wr4+0x0010" },
    { false, 4, { 0x79, 0x1e, 0x00, 0x10 },
      "mov @dr56+0x0010,wr2" },
    { false, 2, { 0x0a, 0x12 }, "movz wr2,r2" },
    { false, 2, { 0x1a, 0x12 }, "movs wr2,r2" },
    { false, 2, { 0x0b, 0x02 }, "inc r0,#4" },
    { false, 2, { 0x0b, 0x15 }, "inc wr2,#2" },
    { false, 2, { 0x0b, 0xed }, "inc dr56,#2" },
    { false, 3, { 0x0b, 0x18, 0x20 }, "mov wr4,@wr2" },
    { false, 3, { 0x0b, 0xea, 0x20 }, "mov wr4,@dr56" },
    { false, 3, { 0x1b, 0x18, 0x20 }, "mov @wr2,wr4" },
    { false, 3, { 0x1b, 0xea, 0x20 }, "mov @dr56,wr4" },
    { false, 2, { 0x0e, 0x10 }, "sra r1" },
    { false, 2, { 0x1e, 0x14 }, "srl wr2" },
    { false, 2, { 0x3e, 0x10 }, "sll r1" },
    { false, 3, { 0x2e, 0x10, 0x7f }, "add r1,#0x7f" },
    { false, 3, { 0x2e, 0x11, 0x40 }, "add r1,0x40" },
    { false, 4, { 0x2e, 0x13, 0x12, 0x34 },
      "add r1,0x1234" },
    { false, 4, { 0x2e, 0x14, 0x12, 0x34 },
      "add wr2,#0x1234" },
    { false, 3, { 0x2e, 0x15, 0x40 }, "add wr2,0x40" },
    { false, 4, { 0x2e, 0x17, 0x12, 0x34 },
      "add wr2,0x1234" },
    { false, 4, { 0x2e, 0x18, 0x12, 0x34 },
      "add dr4,#0x1234" },
    { false, 3, { 0x2e, 0x19, 0x20 }, "add r2,@wr2" },
    { false, 3, { 0x2e, 0x1b, 0x20 }, "add r2,@dr4" },
    { false, 4, { 0xbe, 0xec, 0x12, 0x34 },
      "cmp dr56,#0x1234" },
    { false, 4, { 0x7e, 0xec, 0x12, 0x34 },
      "mov dr56,#0x1234" },
    { false, 3, { 0x7e, 0xed, 0x40 }, "mov dr56,0x40" },
    { false, 4, { 0x7e, 0xef, 0x12, 0x34 },
      "mov dr56,0x1234" },
    { false, 3, { 0x7a, 0x11, 0x40 }, "mov 0x40,r1" },
    { false, 4, { 0x7a, 0x13, 0x12, 0x34 },
      "mov 0x1234,r1" },
    { false, 3, { 0x7a, 0x15, 0x40 }, "mov 0x40,wr2" },
    { false, 4, { 0x7a, 0x17, 0x12, 0x34 },
      "mov 0x1234,wr2" },
    { false, 3, { 0x7a, 0xed, 0x40 }, "mov 0x40,dr56" },
    { false, 4, { 0x7a, 0xef, 0x12, 0x34 },
      "mov 0x1234,dr56" },
    { false, 3, { 0x7a, 0x19, 0x20 }, "mov @wr2,r2" },
    { false, 3, { 0x7a, 0xeb, 0x20 }, "mov @dr56,r2" },
    { false, 4, { 0x7a, 0xec, 0x12, 0x34 },
      "movh dr56,#0x1234" },
    { false, 2, { 0x89, 0x14 }, "ljmp @wr2" },
    { false, 2, { 0x89, 0xe8 }, "ejmp @dr56" },
    { false, 2, { 0x99, 0x14 }, "lcall @wr2" },
    { false, 2, { 0x99, 0xe8 }, "ecall @dr56" },
    { false, 2, { 0x8c, 0x12 }, "div r1,r2" },
    { false, 2, { 0x8d, 0x12 }, "div wr2,wr4" },
    { false, 2, { 0xac, 0x12 }, "mul r1,r2" },
    { false, 2, { 0xad, 0x12 }, "mul wr2,wr4" },
    { false, 4, { 0xa9, 0x13, 0x20, 0x00 }, "jbc 0x20.3," },
    { false, 4, { 0xa9, 0x23, 0x20, 0x00 }, "jb 0x20.3," },
    { false, 4, { 0xa9, 0x33, 0x20, 0x00 }, "jnb 0x20.3," },
    { false, 3, { 0xa9, 0x73, 0x20 }, "orl c,0x20.3" },
    { false, 3, { 0xa9, 0x83, 0x20 }, "anl c,0x20.3" },
    { false, 3, { 0xa9, 0x93, 0x20 }, "mov 0x20.3,c" },
    { false, 3, { 0xa9, 0xa3, 0x20 }, "mov c,0x20.3" },
    { false, 3, { 0xa9, 0xb3, 0x20 }, "cpl 0x20.3" },
    { false, 3, { 0xa9, 0xc3, 0x20 }, "clr 0x20.3" },
    { false, 3, { 0xa9, 0xd3, 0x20 }, "setb 0x20.3" },
    { false, 3, { 0xa9, 0xe3, 0x20 }, "orl c,/0x20.3" },
    { false, 3, { 0xa9, 0xf3, 0x20 }, "anl c,/0x20.3" },
    { false, 3, { 0xca, 0x02, 0x55 }, "push #0x55" },
    { false, 4, { 0xca, 0x06, 0x12, 0x34 }, "push #0x1234" },
    { false, 2, { 0xca, 0x18 }, "push r1" },
    { false, 2, { 0xca, 0x19 }, "push wr2" },
    { false, 2, { 0xca, 0xeb }, "push dr56" },
    { false, 2, { 0xda, 0x18 }, "pop r1" },
    { false, 2, { 0xda, 0x19 }, "pop wr2" },
    { false, 2, { 0xda, 0xeb }, "pop dr56" },
    { false, 2, { 0x2e, 0x02 }, "nop" },
    { false, 2, { 0xa5, 0x28 }, "esc add a,r0" },
    { true, 3, { 0xa5, 0x2c, 0x01 }, "esc add r0,r1" },
    { false, 4, { 0xa5, 0xa5, 0x74, 0x12 },
      "esc esc mov a,#0x12" },
};

static void test_instruction_disassembly(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *filename = NULL;
    g_autofree char *quoted = NULL;
    g_autofree uint8_t *image = g_malloc0(FLASH_SIZE);
    QTestState *qts;
    uint32_t address;
    unsigned opcode;
    unsigned i;

    for (opcode = 0; opcode < 256; opcode++) {
        address = SOURCE_OPCODE_BASE + opcode * OPCODE_SLOT_SIZE;
        image[address - FLASH_BASE] = opcode;
        if (opcode == 0x7a) {
            image[address - FLASH_BASE + 1] = 0x01;
        } else if (opcode == 0x89 || opcode == 0x99) {
            image[address - FLASH_BASE + 1] = 0x04;
        } else if (opcode == 0xa9) {
            image[address - FLASH_BASE + 1] = 0x70;
            image[address - FLASH_BASE + 2] = 0x20;
        } else if (opcode == 0xca || opcode == 0xda) {
            image[address - FLASH_BASE + 1] = 0x08;
        }
        address = BINARY_OPCODE_BASE + opcode * OPCODE_SLOT_SIZE;
        image[address - FLASH_BASE] = opcode;
    }
    address = OPERAND_VECTOR_BASE;
    for (i = 0; i < G_N_ELEMENTS(disas_vectors); i++) {
        memcpy(image + address - FLASH_BASE, disas_vectors[i].bytes,
               disas_vectors[i].length);
        address += OPCODE_SLOT_SIZE;
    }

    directory = g_dir_make_tmp("stc32-disas-XXXXXX", &error);
    g_assert_no_error(error);
    filename = g_build_filename(directory, "firmware.bin", NULL);
    g_file_set_contents(filename, (char *)image, FLASH_SIZE, &error);
    g_assert_no_error(error);
    quoted = quote_firmware_path(filename);
    qts = qtest_initf(MACHINE " -S -bios %s", quoted);

    qtest_writeb(qts, SFR(0x97), 0x00);
    for (opcode = 0; opcode < 256; opcode++) {
        g_autofree char *assembly = NULL;
        bool defined = source_opcode_is_defined(opcode);

        address = SOURCE_OPCODE_BASE + opcode * OPCODE_SLOT_SIZE;
        assembly = disassemble_one(qts, address,
                                   source_primary_length(opcode));
        if (!defined || opcode == 0x00) {
            g_assert_cmpstr(assembly, ==, "nop");
        } else if (opcode == 0xa5) {
            g_assert_cmpstr(assembly, ==, "esc nop");
        } else {
            g_assert_cmpstr(assembly, !=, "nop");
        }
    }

    qtest_writeb(qts, SFR(0x97), 0x40);
    for (opcode = 0; opcode < 256; opcode++) {
        g_autofree char *assembly = NULL;
        unsigned length = opcode == 0xa5 ?
                          2 : classic_opcode_length(opcode);

        address = BINARY_OPCODE_BASE + opcode * OPCODE_SLOT_SIZE;
        assembly = disassemble_one(qts, address, length);
        if (opcode == 0x00) {
            g_assert_cmpstr(assembly, ==, "nop");
        } else if (opcode == 0xa5) {
            g_assert_cmpstr(assembly, ==, "esc nop");
        } else {
            g_assert_cmpstr(assembly, !=, "nop");
        }
    }

    address = OPERAND_VECTOR_BASE;
    for (i = 0; i < G_N_ELEMENTS(disas_vectors); i++) {
        const MCS251DisasVector *vector = &disas_vectors[i];
        g_autofree char *assembly = NULL;

        qtest_writeb(qts, SFR(0x97), vector->binary_mode ? 0x40 : 0x00);
        assembly = disassemble_one(qts, address, vector->length);
        g_assert_nonnull(strstr(assembly, vector->expected));
        address += OPCODE_SLOT_SIZE;
    }

    qtest_quit(qts);
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_cpu_control_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, SFR(0x87), 0xa5);
    qtest_writeb(qts, SFR(0x8e), 0xe7);
    qtest_writeb(qts, SFR(0x8f), 0xaa);
    qtest_writeb(qts, SFR(0x97), 0xff);
    qtest_writeb(qts, SFR(0xa8), 0xff);
    qtest_writeb(qts, SFR(0xb7), 0xff);
    qtest_writeb(qts, SFR(0xb8), 0xff);
    qtest_writeb(qts, SFR(0xba), 0xff);
    qtest_writeb(qts, SFR(0xea), 0x55);
    qtest_writeb(qts, SFR(0xeb), 0xaa);

    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8e)), ==, 0xe7);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8f)), ==, 0xaa);
    g_assert_cmphex(qtest_readb(qts, SFR(0x97)), ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa8)), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb7)), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb8)), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xba)), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, SFR(0xea)), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, SFR(0xeb)), ==, 0xaa);

    qtest_writeb(qts, SFR(0xe3), 0x08);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0x00);
    qtest_writeb(qts, SFR(0xae), 0xaa);
    qtest_writeb(qts, SFR(0xae), 0x55);
    qtest_writeb(qts, SFR(0xe3), 0x08);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0x08);
    qtest_writeb(qts, SFR(0xe3), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0xf9);

    qtest_quit(qts);
}

static void test_dsp32_registers_and_conversion(void)
{
    static const struct {
        uint8_t selector;
        uint8_t base;
    } cdrs_vectors[] = {
        { 0, 0 },
        { 1, 8 },
        { 2, 16 },
        { 3, 24 },
    };
    QTestState *qts = qtest_init(MACHINE);
    unsigned i;

    qtest_writeb(qts, SFR(0x86), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0x86)), ==, 0x00);

    /*
     * DPUCFG resets to CDRS=5, selecting architectural DR16/DR20
     * instead of fixed-bank DATA bytes with the same numbers.
     */
    dsp_write_data32(qts, 16, 0x11223344);
    dsp_write_data32(qts, 20, 0x55667788);
    dsp_command(qts, 0xb6);
    dsp_command(qts, 0xb4);
    g_assert_cmphex(dsp_read_data32(qts, 16), ==, 0x11223344);
    g_assert_cmphex(dsp_read_data32(qts, 20), ==, 0x55667788);

    qtest_writeb(qts, SFR(0xd0), 0x08);
    for (i = 0; i < G_N_ELEMENTS(cdrs_vectors); i++) {
        unsigned base = cdrs_vectors[i].base;

        qtest_writeb(qts, SFR(0xe0), cdrs_vectors[i].selector);
        dsp_command(qts, 0x80);
        dsp_write_data32(qts, base, 0x11223344);
        dsp_write_data32(qts, base + 4, 0x55667788);
        dsp_command(qts, 0xb6);
        dsp_command(qts, 0xb4);
        g_assert_cmphex(dsp_read_data32(qts, base), ==, 0);
        g_assert_cmphex(dsp_read_data32(qts, base + 4), ==, 0);
    }

    /*
     * CDRS=5/6 select extended architectural registers, not the
     * fixed-bank DATA bytes selected by CDRS=2/3.
     */
    for (i = 5; i <= 6; i++) {
        unsigned base = i == 5 ? 16 : 24;

        qtest_writeb(qts, SFR(0xe0), i);
        dsp_command(qts, 0x80);
        dsp_write_data32(qts, base, 0x11223344);
        dsp_write_data32(qts, base + 4, 0x55667788);
        dsp_command(qts, 0xb6);
        dsp_command(qts, 0xb4);
        g_assert_cmphex(dsp_read_data32(qts, base), ==, 0x11223344);
        g_assert_cmphex(dsp_read_data32(qts, base + 4), ==, 0x55667788);
    }

    qtest_writeb(qts, SFR(0xe0), 0);
    dsp_command(qts, 0x80);
    dsp_write32(qts, DSP_EAX, 0x11223344);
    dsp_write32(qts, DSP_EBX, 0x55667788);
    dsp_command(qts, 0x8f);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x11223344);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0x55667788);

    dsp_write32(qts, DSP_EBX, 123456789);
    dsp_command(qts, 0x81);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x00000001);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0x23456789);
    dsp_command(qts, 0x82);
    g_assert_cmpuint(dsp_read32(qts, DSP_EAX), ==, 123456789);

    /* CDRS=4 is reserved, so C/D-register commands have no effect. */
    qtest_writeb(qts, SFR(0xe0), 4);
    dsp_command(qts, 0x80);
    dsp_write_data32(qts, 0, 0x11223344);
    dsp_write_data32(qts, 4, 0x55667788);
    dsp_command(qts, 0x87);
    g_assert_cmphex(dsp_read_data32(qts, 0), ==, 0x11223344);
    g_assert_cmphex(dsp_read_data32(qts, 4), ==, 0x55667788);

    qtest_quit(qts);
}

static void test_dsp32_normalization_and_swap(void)
{
    QTestState *qts = dsp_test_init();

    dsp_write32(qts, DSP_EBX, 0xaaaa0100);
    dsp_command(qts, 0x83);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0xaaaa4000);
    g_assert_cmphex(qtest_readb(qts, SFR(0x86)) & DPUST_SC_MASK,
                    ==, 6);

    dsp_write32(qts, DSP_EBX, 0x01000000);
    dsp_command(qts, 0x84);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0x40000000);
    g_assert_cmphex(qtest_readb(qts, SFR(0x86)) & DPUST_SC_MASK,
                    ==, 6);

    dsp_write32(qts, DSP_EAX, 0x01000000);
    dsp_write32(qts, DSP_EBX, 0x00000000);
    dsp_command(qts, 0x85);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x40000000);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0x00000000);
    g_assert_cmphex(qtest_readb(qts, SFR(0x86)) & DPUST_SC_MASK,
                    ==, 6);

    dsp_write32(qts, DSP_EAX, 1);
    dsp_write32(qts, DSP_EBX, 2);
    dsp_command(qts, 0x86);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 2);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 1);

    dsp_write32(qts, DSP_ECX, 3);
    dsp_write32(qts, DSP_EDX, 4);
    dsp_command(qts, 0x87);
    g_assert_cmphex(dsp_read32(qts, DSP_ECX), ==, 4);
    g_assert_cmphex(dsp_read32(qts, DSP_EDX), ==, 3);

    dsp_write32(qts, DSP_EAX, 5);
    dsp_write32(qts, DSP_ECX, 6);
    dsp_command(qts, 0x88);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 6);
    g_assert_cmphex(dsp_read32(qts, DSP_ECX), ==, 5);

    dsp_write32(qts, DSP_EBX, 7);
    dsp_write32(qts, DSP_EDX, 8);
    dsp_command(qts, 0x89);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 8);
    g_assert_cmphex(dsp_read32(qts, DSP_EDX), ==, 7);

    qtest_quit(qts);
}

static void test_dsp32_arithmetic(void)
{
    QTestState *qts = dsp_test_init();

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, UINT32_MAX);
    dsp_write32(qts, DSP_EBX, 0);
    dsp_command(qts, 0x90);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0);
    dsp_assert_flags(qts, true, false, true);

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, 0xa5a5ffff);
    dsp_write32(qts, DSP_EBX, 0);
    dsp_command(qts, 0x91);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a50000);
    dsp_assert_flags(qts, true, false, true);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 0x7fffffff);
    dsp_write32(qts, DSP_EBX, 1);
    dsp_command(qts, 0x92);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x80000000);
    dsp_assert_flags(qts, false, true, false);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 0xa5a57fff);
    dsp_write32(qts, DSP_EBX, 1);
    dsp_command(qts, 0x93);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a58000);
    dsp_assert_flags(qts, false, true, false);

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, 0);
    dsp_write32(qts, DSP_EBX, 0);
    dsp_command(qts, 0x94);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, UINT32_MAX);
    dsp_assert_flags(qts, true, false, false);

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, 0);
    dsp_write32(qts, DSP_EBX, 0x7fffffff);
    dsp_command(qts, 0x94);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x80000000);
    dsp_assert_flags(qts, true, false, false);

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, 0x80000000);
    dsp_write32(qts, DSP_EBX, 0x7fffffff);
    dsp_command(qts, 0x94);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0);
    dsp_assert_flags(qts, false, true, true);

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, 0xa5a50000);
    dsp_write32(qts, DSP_EBX, 0);
    dsp_command(qts, 0x95);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a5ffff);
    dsp_assert_flags(qts, true, false, false);

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, 0xa5a50000);
    dsp_write32(qts, DSP_EBX, 0x5a5a7fff);
    dsp_command(qts, 0x95);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a58000);
    dsp_assert_flags(qts, true, false, false);

    dsp_set_flags(qts, true);
    dsp_write32(qts, DSP_EAX, 0xa5a58000);
    dsp_write32(qts, DSP_EBX, 0x5a5a7fff);
    dsp_command(qts, 0x95);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a50000);
    dsp_assert_flags(qts, false, true, true);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 0x80000000);
    dsp_write32(qts, DSP_EBX, 1);
    dsp_command(qts, 0x96);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x7fffffff);
    dsp_assert_flags(qts, false, true, false);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 0xa5a58000);
    dsp_write32(qts, DSP_EBX, 1);
    dsp_command(qts, 0x97);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a57fff);
    dsp_assert_flags(qts, false, true, false);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 2);
    dsp_write32(qts, DSP_EBX, 3);
    dsp_command(qts, 0x98);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 2);
    dsp_assert_flags(qts, true, false, false);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 0xa5a51234);
    dsp_write32(qts, DSP_EBX, 0x5a5a1234);
    dsp_command(qts, 0x99);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a51234);
    dsp_assert_flags(qts, false, false, true);

    qtest_quit(qts);
}

static void test_dsp32_multiply_and_divide(void)
{
    QTestState *qts = dsp_test_init();

    dsp_write32(qts, DSP_EAX, UINT32_MAX);
    dsp_write32(qts, DSP_EBX, 2);
    dsp_command(qts, 0x9a);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffffffe);
    g_assert_cmphex(dsp_read32(qts, DSP_EDX), ==, 1);

    dsp_write32(qts, DSP_EAX, -2);
    dsp_write32(qts, DSP_EBX, 3);
    dsp_command(qts, 0x9b);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffffffa);
    g_assert_cmphex(dsp_read32(qts, DSP_EDX), ==, UINT32_MAX);

    dsp_write32(qts, DSP_EAX, 0x0000ffff);
    dsp_write32(qts, DSP_EBX, 2);
    dsp_command(qts, 0x9c);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x0001fffe);

    dsp_write32(qts, DSP_EAX, 0x0000fffe);
    dsp_write32(qts, DSP_EBX, 3);
    dsp_command(qts, 0x9d);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffffffa);

    dsp_write32(qts, DSP_EAX, 0x00030004);
    dsp_command(qts, 0x9e);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x0003000c);

    dsp_write32(qts, DSP_EAX, 17);
    dsp_write32(qts, DSP_EBX, 5);
    dsp_command(qts, 0x9f);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 3);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 2);

    dsp_write32(qts, DSP_EAX, -17);
    dsp_write32(qts, DSP_EBX, 5);
    dsp_command(qts, 0xa0);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffffffd);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0xfffffffe);

    dsp_write32(qts, DSP_EAX, 17);
    dsp_write32(qts, DSP_EBX, 5);
    dsp_command(qts, 0xa1);
    g_assert_cmphex(dsp_read16(qts, DSP_EAX), ==, 3);
    g_assert_cmphex(dsp_read16(qts, DSP_EBX), ==, 2);

    dsp_write32(qts, DSP_EAX, 0x00050011);
    dsp_command(qts, 0xa2);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x00020003);

    dsp_write32(qts, DSP_EAX, 0x0000ffef);
    dsp_write32(qts, DSP_EBX, 5);
    dsp_command(qts, 0xa3);
    g_assert_cmphex(dsp_read16(qts, DSP_EAX), ==, 0xfffd);
    g_assert_cmphex(dsp_read16(qts, DSP_EBX), ==, 0xfffe);

    dsp_write32(qts, DSP_EAX, 0x0005ffef);
    dsp_command(qts, 0xa4);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffefffd);

    dsp_write32(qts, DSP_EAX, 1);
    dsp_write32(qts, DSP_EBX, 0);
    dsp_write32(qts, DSP_EDX, 3);
    dsp_command(qts, 0xa5);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0x55555555);
    g_assert_cmphex(dsp_read32(qts, DSP_EDX), ==, 1);

    dsp_write32(qts, DSP_EAX, 0x00010000);
    dsp_write32(qts, DSP_EBX, 3);
    dsp_command(qts, 0xa6);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x00005555);
    g_assert_cmphex(dsp_read16(qts, DSP_EBX), ==, 1);

    dsp_write32(qts, DSP_EAX, 17);
    dsp_write32(qts, DSP_EBX, 0);
    dsp_command(qts, 0x9f);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 17);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x86)) & DPUST_DBZ,
                    ==, DPUST_DBZ);
    dsp_write32(qts, DSP_EBX, 5);
    dsp_command(qts, 0x9f);
    g_assert_cmphex(qtest_readb(qts, SFR(0x86)) & DPUST_DBZ,
                    ==, 0);

    qtest_quit(qts);
}

static void test_dsp32_unary_and_accumulate(void)
{
    static const enum DSPRegister set_targets[] = {
        DSP_EAX, DSP_EAX, DSP_EBX, DSP_EBX,
        DSP_ECX, DSP_ECX, DSP_EDX, DSP_EDX,
    };
    static const DSPAccumulateVector accumulate_vectors[] = {
        { 0xc0, DSP_EAX, UINT32_MAX, 0, true },
        { 0xc1, DSP_EBX, UINT32_MAX, 0, true },
        { 0xc2, DSP_EAX, 0xfffffffc, 0, true },
        { 0xc3, DSP_EBX, 0xfffffffc, 0, true },
        { 0xc4, DSP_EAX, 0xa5a5ffff, 0xa5a50000, true },
        { 0xc5, DSP_EBX, 0xa5a5ffff, 0xa5a50000, true },
        { 0xc6, DSP_EAX, 0xa5a5fffe, 0xa5a50000, true },
        { 0xc7, DSP_EBX, 0xa5a5fffe, 0xa5a50000, true },
        { 0xc8, DSP_EAX, 0, UINT32_MAX, false },
        { 0xc9, DSP_EBX, 0, UINT32_MAX, false },
        { 0xca, DSP_EAX, 3, UINT32_MAX, false },
        { 0xcb, DSP_EBX, 3, UINT32_MAX, false },
        { 0xcc, DSP_EAX, 0xa5a50000, 0xa5a5ffff, false },
        { 0xcd, DSP_EBX, 0xa5a50000, 0xa5a5ffff, false },
        { 0xce, DSP_EAX, 0xa5a50001, 0xa5a5ffff, false },
        { 0xcf, DSP_EBX, 0xa5a50001, 0xa5a5ffff, false },
    };
    QTestState *qts = dsp_test_init();
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(set_targets); i++) {
        dsp_write32(qts, set_targets[i], 0xa5a5a5a5);
        dsp_command(qts, 0xb0 + i);
        g_assert_cmphex(dsp_read32(qts, set_targets[i]), ==,
                        i & 1 ? UINT32_MAX : 0);
    }

    dsp_write32(qts, DSP_EAX, 5);
    dsp_command(qts, 0xb8);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffffffb);
    dsp_write32(qts, DSP_EBX, 5);
    dsp_command(qts, 0xb9);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0xfffffffb);
    dsp_write32(qts, DSP_EAX, 0xa5a50005);
    dsp_command(qts, 0xba);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xa5a5fffb);
    dsp_write32(qts, DSP_EBX, 0x5a5a0005);
    dsp_command(qts, 0xbb);
    g_assert_cmphex(dsp_read32(qts, DSP_EBX), ==, 0x5a5afffb);

    for (i = 0; i < G_N_ELEMENTS(accumulate_vectors); i++) {
        const DSPAccumulateVector *vector = &accumulate_vectors[i];

        dsp_set_flags(qts, false);
        dsp_write32(qts, vector->target, vector->input);
        dsp_command(qts, vector->command);
        g_assert_cmphex(dsp_read32(qts, vector->target),
                        ==, vector->expected);
        dsp_assert_flags(qts, true, false, vector->zero);
    }

    qtest_quit(qts);
}

static void test_dsp32_logic_and_shift(void)
{
    static const struct {
        uint8_t command;
        enum DSPRegister target;
        uint32_t expected;
    } logic_vectors[] = {
        { 0xd0, DSP_EBX, 0x030300ff },
        { 0xd1, DSP_EBX, 0x333300ff },
        { 0xd2, DSP_EBX, 0x3f3fffff },
        { 0xd3, DSP_EBX, 0x3333ffff },
        { 0xd4, DSP_EBX, 0x3c3cff00 },
        { 0xd5, DSP_EBX, 0x3333ff00 },
        { 0xd6, DSP_EAX, 0xf0f0ff00 },
        { 0xd7, DSP_EBX, 0xcccc0000 },
    };
    static const struct {
        uint8_t command;
        enum DSPRegister target;
        uint32_t input;
        uint32_t expected;
        bool updates_carry;
    } shift_vectors[] = {
        { 0xe0, DSP_EAX, 0x80000001, 0x00000002, true },
        { 0xe1, DSP_EBX, 0x80000001, 0x00000002, true },
        { 0xe2, DSP_EAX, 0xa5a58001, 0xa5a50002, true },
        { 0xe3, DSP_EBX, 0x5a5a8001, 0x5a5a0002, true },
        { 0xe4, DSP_EAX, 0x00000001, 0x00000000, true },
        { 0xe5, DSP_EBX, 0x00000001, 0x00000000, true },
        { 0xe6, DSP_EAX, 0xa5a50001, 0xa5a50000, true },
        { 0xe7, DSP_EBX, 0x5a5a0001, 0x5a5a0000, true },
        { 0xe8, DSP_EAX, 0x80000001, 0xc0000000, true },
        { 0xe9, DSP_EBX, 0x80000001, 0xc0000000, true },
        { 0xea, DSP_EAX, 0xa5a58001, 0xa5a5c000, true },
        { 0xeb, DSP_EBX, 0x5a5a8001, 0x5a5ac000, true },
        { 0xec, DSP_EAX, 0x00000001, 0x80000000, false },
        { 0xed, DSP_EBX, 0x00000001, 0x80000000, false },
        { 0xee, DSP_EAX, 0xa5a50001, 0xa5a58000, false },
        { 0xef, DSP_EBX, 0x5a5a0001, 0x5a5a8000, false },
    };
    QTestState *qts = dsp_test_init();
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(logic_vectors); i++) {
        dsp_write32(qts, DSP_EAX, 0x0f0f00ff);
        dsp_write32(qts, DSP_EBX, 0x3333ffff);
        dsp_command(qts, logic_vectors[i].command);
        g_assert_cmphex(dsp_read32(qts, logic_vectors[i].target),
                        ==, logic_vectors[i].expected);
        g_assert_cmphex(qtest_readb(qts, SFR(0x86)) & DPUST_Z,
                        ==, 0);
    }

    qtest_writeb(qts, SFR(0xe0), 1);
    for (i = 0; i < G_N_ELEMENTS(shift_vectors); i++) {
        dsp_set_flags(qts, false);
        dsp_write32(qts, shift_vectors[i].target,
                    shift_vectors[i].input);
        dsp_command(qts, shift_vectors[i].command);
        g_assert_cmphex(dsp_read32(qts, shift_vectors[i].target),
                        ==, shift_vectors[i].expected);
        if (shift_vectors[i].updates_carry) {
            g_assert_cmphex(qtest_readb(qts, SFR(0xd0)) & 0x80,
                            ==, 0x80);
        }
    }

    qtest_quit(qts);
}

static void test_dsp32_compound(void)
{
    QTestState *qts = dsp_test_init();

    dsp_write32(qts, DSP_EAX, 6);
    dsp_write32(qts, DSP_EBX, -4);
    dsp_write32(qts, DSP_EDX, 3);
    dsp_command(qts, 0xf0);
    g_assert_cmphex(dsp_read32(qts, DSP_ECX), ==, UINT32_MAX);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffffff8);

    dsp_write32(qts, DSP_EAX, 6);
    dsp_write32(qts, DSP_EBX, 0x0000fffc);
    dsp_write32(qts, DSP_EDX, 3);
    dsp_command(qts, 0xf1);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0xfffffff8);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 10);
    dsp_write32(qts, DSP_EBX, 2);
    dsp_write32(qts, DSP_ECX, 6);
    dsp_write32(qts, DSP_EDX, 4);
    dsp_command(qts, 0xf2);
    g_assert_cmphex(dsp_read32(qts, DSP_ECX), ==, 0);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 12);
    dsp_assert_flags(qts, false, false, false);

    dsp_set_flags(qts, false);
    dsp_write32(qts, DSP_EAX, 10);
    dsp_write32(qts, DSP_EBX, 2);
    dsp_write32(qts, DSP_ECX, 6);
    dsp_write32(qts, DSP_EDX, 4);
    dsp_command(qts, 0xf3);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 12);
    dsp_assert_flags(qts, false, false, false);

    dsp_write32(qts, DSP_EAX, 3);
    dsp_write32(qts, DSP_EBX, 4);
    dsp_write32(qts, DSP_EDX, 5);
    dsp_command(qts, 0xf4);
    g_assert_cmphex(dsp_read32(qts, DSP_EDX), ==, 17);

    dsp_write32(qts, DSP_EAX, 3);
    dsp_write32(qts, DSP_EBX, 4);
    dsp_write32(qts, DSP_ECX, 0);
    dsp_write32(qts, DSP_EDX, 5);
    dsp_command(qts, 0xf5);
    g_assert_cmphex(dsp_read32(qts, DSP_ECX), ==, 0);
    g_assert_cmphex(dsp_read32(qts, DSP_EDX), ==, 17);

    qtest_quit(qts);
}

static void test_tfpu_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, SFR(0xd0), 0x08);
    dsp_write32(qts, DSP_EAX, 0x3f800000);
    dsp_write32(qts, DSP_EBX, 0x40000000);

    /*
     * DMAIR only starts a command for a CPU MOV direct,#immediate.
     * A qtest bus write behaves like another instruction or debugger.
     */
    qtest_writeb(qts, SFR(0xed), 0x1c);
    g_assert_cmphex(qtest_readb(qts, SFR(0xed)), ==, 0x1c);
    g_assert_cmphex(dsp_read32(qts, DSP_EAX), ==, 0x3f800000);

    qtest_writeb(qts, 0x7efe93, 0xa5);
    g_assert_cmphex(qtest_readb(qts, 0x7efe93), ==, 0xa5);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0xed)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, 0x7efe93), ==, 0);

    qtest_quit(qts);
}

static void test_gpio_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned port;

    for (port = 0; port < G_N_ELEMENTS(gpio_data_address); port++) {
        uint8_t value = 0x11 * (port + 1);

        qtest_writeb(qts, SFR(gpio_mode1_address[port]), 0x00);
        qtest_writeb(qts, SFR(gpio_mode0_address[port]), 0xff);
        qtest_writeb(qts, SFR(gpio_data_address[port]), value);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_data_address[port])),
                        ==, value);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode1_address[port])),
                        ==, 0x00);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode0_address[port])),
                        ==, 0xff);
    }

    qtest_quit(qts);
}

static void test_gpio_input_reset(void)
{
    QTestState *qts = qtest_init(MACHINE);
    const unsigned pin = 8;

    qtest_writeb(qts, SFR(0x91), 0x00);
    qtest_writeb(qts, SFR(0x92), 0x01);
    qtest_writeb(qts, SFR(0x90), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, SFR(0x91)), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0x92)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 0x01, ==, 0x00);

    qtest_quit(qts);
}

static void test_gpio_modes(void)
{
    QTestState *qts = qtest_init(MACHINE);
    const unsigned pin = 8;

    qtest_irq_intercept_out_named(qts, GPIO, "gpio-out");

    /* Quasi-bidirectional: a zero drives low; a one samples the pin. */
    qtest_writeb(qts, SFR(0x91), 0x00);
    qtest_writeb(qts, SFR(0x92), 0x00);
    qtest_writeb(qts, SFR(0x90), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 0);
    g_assert_false(qtest_get_irq(qts, pin));
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, pin));
    qtest_writeb(qts, SFR(0x90), 0x00);
    g_assert_false(qtest_get_irq(qts, pin));

    /* Push-pull output ignores the sampled input. */
    qtest_writeb(qts, SFR(0x91), 0x00);
    qtest_writeb(qts, SFR(0x92), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    qtest_writeb(qts, SFR(0x90), 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, pin));

    /* High-impedance input follows the sampled input. */
    qtest_writeb(qts, SFR(0x91), 0x01);
    qtest_writeb(qts, SFR(0x92), 0x00);
    qtest_writeb(qts, SFR(0x90), 0x00);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, pin));
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    g_assert_false(qtest_get_irq(qts, pin));

    /* Open drain drives a zero and samples the pin when released. */
    qtest_writeb(qts, SFR(0x91), 0x01);
    qtest_writeb(qts, SFR(0x92), 0x01);
    qtest_writeb(qts, SFR(0x90), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 0);
    qtest_writeb(qts, SFR(0x90), 0x00);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_false(qtest_get_irq(qts, pin));

    qtest_quit(qts);
}

static void test_external_interrupts(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_irq_intercept_in(qts, CPU);

    /* IT0=0: an active-low input holds IE0 and INT0 asserted. */
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_INT0));
    qtest_writeb(qts, SFR(0x88), 0x00);
    g_assert_true(qtest_get_irq(qts, IRQ_INT0));
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT0));

    /* IT0=1: rising is ignored and falling latches IE0. */
    qtest_writeb(qts, SFR(0x88), 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT0));
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_INT0));
    qtest_writeb(qts, SFR(0x88), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 0);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT0));

    /* INT1 follows the same level and edge selection on P3.3. */
    qtest_writeb(qts, SFR(0x88), 0x00);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x08, ==, 0x08);
    g_assert_true(qtest_get_irq(qts, IRQ_INT1));
    qtest_writeb(qts, SFR(0x88), 0x00);
    g_assert_true(qtest_get_irq(qts, IRQ_INT1));
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x08, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT1));

    qtest_writeb(qts, SFR(0x88), 0x04);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x08, ==, 0x08);
    g_assert_true(qtest_get_irq(qts, IRQ_INT1));
    qtest_writeb(qts, SFR(0x88), 0x04);
    g_assert_false(qtest_get_irq(qts, IRQ_INT1));
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x08, ==, 0x00);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 0);
    g_assert_true(qtest_get_irq(qts, IRQ_INT1));

    qtest_quit(qts);
}

static void test_gpio_alternate_interrupts(void)
{
    unsigned interrupt;

    for (interrupt = 0; interrupt < 2; interrupt++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned pin = 2 + interrupt;
        unsigned gpio_pin = 3 * 8 + pin;
        uint8_t pin_mask = BIT(pin);
        uint8_t trigger_mask = BIT(interrupt * 2);
        uint8_t flag_mask = BIT(interrupt * 2 + 1);
        unsigned irq = interrupt ? IRQ_INT1 : IRQ_INT0;

        qtest_irq_intercept_in(qts, CPU);

        /* Start low in input mode, then make the latch drive high. */
        qtest_set_irq_in(qts, GPIO, "gpio-in", gpio_pin, 0);
        qtest_writeb(qts, SFR(0x88), trigger_mask);
        qtest_writeb(qts, SFR(0xb1),
                     qtest_readb(qts, SFR(0xb1)) & ~pin_mask);
        qtest_writeb(qts, SFR(0xb2),
                     qtest_readb(qts, SFR(0xb2)) | pin_mask);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag_mask, ==, 0);

        /* A push-pull latch transition must reach the alternate line. */
        qtest_writeb(qts, SFR(0xb0),
                     qtest_readb(qts, SFR(0xb0)) & ~pin_mask);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag_mask,
                        ==, flag_mask);
        g_assert_true(qtest_get_irq(qts, irq));

        qtest_quit(qts);
    }
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
    qtest_writeb(qts, SFR(0x88), 0x00);
    g_assert_false(qtest_get_irq(qts, timer_irq(timer)));

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
}

static void test_timer_reload_while_running(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);

        qtest_writeb(qts, SFR(0x89), 0x00);
        timer_set_count(qts, timer, 0xff, 0xfc);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        timer_set_count(qts, timer, 0xaa, 0xbb);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfc);

        qtest_clock_step(qts, 2000);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xaa);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xbb);

        qtest_quit(qts);
    }
}

static void test_timer_gates(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned gate_pin = 3 * 8 + 2 + timer;
        uint8_t tmod = 0x09 << (timer * 4);

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

static void test_timer_external_counters(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned counter_pin = 3 * 8 + 4 + timer;
        uint8_t tmod = 0x05 << (timer * 4);

        qtest_system_reset(qts);
        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_writeb(qts, 0x7efea0 + timer, 0x01);

        qtest_set_irq_in(qts, GPIO, "gpio-in", counter_pin, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfe);
        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);

        qtest_set_irq_in(qts, GPIO, "gpio-in", counter_pin, 1);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);
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
}

static void check_timer_rate(unsigned timer, uint8_t auxr,
                             uint8_t prescaler, int64_t before_ns,
                             int64_t final_ns)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, SFR(0x8e), auxr);
    qtest_writeb(qts, 0x7efea0 + timer, prescaler);
    qtest_writeb(qts, SFR(0x89), 0x01 << (timer * 4));
    timer_set_count(qts, timer, 0xff, 0xfe);
    qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
    qtest_clock_step(qts, before_ns);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                    timer_flag_mask(timer), ==, 0);
    qtest_clock_step(qts, final_ns);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                    timer_flag_mask(timer),
                    ==, timer_flag_mask(timer));

    qtest_quit(qts);
}

static void test_timer_clock_and_prescaler(void)
{
    check_timer_rate(0, 0x81, 0, 83, 1);
    check_timer_rate(1, 0x41, 0, 83, 1);
    check_timer_rate(0, 0x01, 1, 1999, 1);
    check_timer_rate(1, 0x01, 1, 1999, 1);
}

static void test_timer_fractional_ticks(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned step;

        qtest_writeb(qts, SFR(0x89), 0x01 << (timer * 4));
        timer_set_count(qts, timer, 0x00, 0x00);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));

        for (step = 1; step <= 5; step++) {
            qtest_clock_step(qts, 400);
            g_assert_cmphex(
                qtest_readb(qts, SFR(timer_tl_address(timer))),
                ==, step * 4 / 5);
        }

        qtest_quit(qts);
    }
}

static void test_timer_fractional_rate(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        uint16_t count;

        qtest_writeb(qts, SFR(0x8e), 0x01);
        qtest_writeb(qts, 0x7efea0 + timer, 0xff);
        qtest_writeb(qts, SFR(0x89), 0x01 << (timer * 4));
        timer_set_count(qts, timer, 0x00, 0x00);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));

        qtest_clock_step(qts, 2000000000LL);
        count = qtest_readb(qts, SFR(timer_th_address(timer))) << 8;
        count |= qtest_readb(qts, SFR(timer_tl_address(timer)));
        g_assert_cmphex(count, ==, 15625);

        qtest_writeb(qts, SFR(0x88), 0);
        timer_set_count(qts, timer, 0xc2, 0xf7);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_clock_step(qts, 1999999999LL);
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
    char socket_path[] = "stc32-uart-tx.XXXXXX";
    QTestState *qts;
    uint8_t received;
    int socket_fd;
    int ret;

    qts = uart_test_init(socket_path, &socket_fd);
    qtest_irq_intercept_in(qts, CPU);
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
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x02, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_UART1));

    uart_test_quit(qts, socket_fd, socket_path);
}

static void test_uart1_receive(void)
{
    char socket_path[] = "stc32-uart-rx.XXXXXX";
    QTestState *qts;
    int socket_fd;
    int ret;

    qts = uart_test_init(socket_path, &socket_fd);
    qtest_irq_intercept_in(qts, CPU);

    /* REN gates delivery from the character backend. */
    ret = send(socket_fd, "A", 1, 0);
    g_assert_cmpint(ret, ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 0);
    qtest_writeb(qts, SFR(0x98), 0x10);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'A');
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 1);
    g_assert_true(qtest_get_irq(qts, IRQ_UART1));

    /* RI applies backpressure and prevents an unread byte being replaced. */
    ret = send(socket_fd, "B", 1, 0);
    g_assert_cmpint(ret, ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'A');
    qtest_writeb(qts, SFR(0x98), 0x10);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'B');
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 1);

    /* TI and RI share one level interrupt; either flag keeps it asserted. */
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

    qtest_add_func("/stc32/reset-registers", test_reset_registers);
    qtest_add_func("/stc32/memory-regions", test_memory_regions);
    qtest_add_func("/stc32/firmware/intel-hex",
                   test_hex_firmware_loading);
    qtest_add_data_func("/stc32/firmware/intel-hex-out-of-range",
                        ":020000040100F9\n"
                        ":0100000000FF\n"
                        ":00000001FF\n",
                        test_invalid_hex_firmware);
    qtest_add_data_func("/stc32/firmware/intel-hex-bad-checksum",
                        ":0200000400FFFB\n"
                        ":0400000001020304F3\n"
                        ":00000001FF\n",
                        test_invalid_hex_firmware);
    qtest_add_func("/stc32/cpu/instruction-disassembly",
                   test_instruction_disassembly);
    qtest_add_func("/stc32/cpu-control-registers",
                   test_cpu_control_registers);
    qtest_add_func("/stc32/dsp32/registers-and-conversion",
                   test_dsp32_registers_and_conversion);
    qtest_add_func("/stc32/dsp32/normalization-and-swap",
                   test_dsp32_normalization_and_swap);
    qtest_add_func("/stc32/dsp32/arithmetic",
                   test_dsp32_arithmetic);
    qtest_add_func("/stc32/dsp32/multiply-and-divide",
                   test_dsp32_multiply_and_divide);
    qtest_add_func("/stc32/dsp32/unary-and-accumulate",
                   test_dsp32_unary_and_accumulate);
    qtest_add_func("/stc32/dsp32/logic-and-shift",
                   test_dsp32_logic_and_shift);
    qtest_add_func("/stc32/dsp32/compound",
                   test_dsp32_compound);
    qtest_add_func("/stc32/tfpu/registers", test_tfpu_registers);
    qtest_add_func("/stc32/gpio/registers", test_gpio_registers);
    qtest_add_func("/stc32/gpio/input-reset", test_gpio_input_reset);
    qtest_add_func("/stc32/gpio/modes", test_gpio_modes);
    qtest_add_func("/stc32/gpio/external-interrupts",
                   test_external_interrupts);
    qtest_add_func("/stc32/gpio/alternate-interrupts",
                   test_gpio_alternate_interrupts);
    qtest_add_func("/stc32/timer/modes", test_timer_modes);
    qtest_add_func("/stc32/timer/reload-while-running",
                   test_timer_reload_while_running);
    qtest_add_func("/stc32/timer/gates", test_timer_gates);
    qtest_add_func("/stc32/timer/external-counters",
                   test_timer_external_counters);
    qtest_add_func("/stc32/timer/clock-and-prescaler",
                   test_timer_clock_and_prescaler);
    qtest_add_func("/stc32/timer/fractional-ticks",
                   test_timer_fractional_ticks);
    qtest_add_func("/stc32/timer/fractional-rate",
                   test_timer_fractional_rate);
    qtest_add_func("/stc32/uart1/transmit", test_uart1_transmit);
    qtest_add_func("/stc32/uart1/receive", test_uart1_receive);

    return g_test_run();
}
