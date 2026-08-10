/*
 * QTest for ARM direct kernel boot
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/uboot_image.h"
#include "libqtest.h"

#define ARM_BOOT_PAGE_SIZE      0x1000
#define ARM_KERNEL_ARGS_OFFSET  0x100

static void test_atags_follow_relocated_trampoline(void)
{
    static const uint32_t kernel_word = GUINT32_TO_LE(0xeafffffe);
    const size_t kernel_size = ARM_BOOT_PAGE_SIZE;
    const uint64_t relocated_atags = ARM_BOOT_PAGE_SIZE +
                                      ARM_KERNEL_ARGS_OFFSET;
    uboot_image_header_t header = {
        .ih_magic = GUINT32_TO_BE(IH_MAGIC),
        .ih_size = GUINT32_TO_BE(ARM_BOOT_PAGE_SIZE),
        .ih_os = IH_OS_LINUX,
        .ih_arch = IH_ARCH_ARM,
        .ih_type = IH_TYPE_KERNEL,
        .ih_comp = IH_COMP_NONE,
        .ih_name = "ARM base-loaded kernel",
    };
    g_autofree uint8_t *image = g_malloc(sizeof(header) + kernel_size);
    g_autofree char *kernel_path = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int kernel_fd;

    memcpy(image, &header, sizeof(header));
    for (size_t offset = 0; offset < kernel_size;
         offset += sizeof(kernel_word)) {
        memcpy(image + sizeof(header) + offset, &kernel_word,
               sizeof(kernel_word));
    }

    kernel_fd = g_file_open_tmp("arm-base-uimage-XXXXXX", &kernel_path,
                                &error);
    g_assert_no_error(error);
    g_assert_cmpint(kernel_fd, >=, 0);
    g_assert_cmpint(close(kernel_fd), ==, 0);
    g_assert_true(g_file_set_contents(kernel_path, (const char *)image,
                                      sizeof(header) + kernel_size, &error));
    g_assert_no_error(error);

    qts = qtest_initf("-machine versatilepb -m 32M -kernel %s",
                      kernel_path);

    /* The ATAG writer must not overwrite the kernel at the RAM base. */
    g_assert_cmphex(qtest_readl(qts, ARM_KERNEL_ARGS_OFFSET), ==,
                    GUINT32_FROM_LE(kernel_word));
    g_assert_cmphex(qtest_readl(qts, relocated_atags), ==, 5);
    g_assert_cmphex(qtest_readl(qts, relocated_atags + 4), ==, 0x54410001);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(kernel_path), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/arm/boot/atags-follow-relocated-trampoline",
                   test_atags_follow_relocated_trampoline);

    return g_test_run();
}
