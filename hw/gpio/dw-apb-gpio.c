/*
 * Synopsys DesignWare APB GPIO
 *
 * This local model implements the single-port data, direction, and external
 * value registers used by Linux gpio-dwapb.  Interrupts and additional ports
 * are deliberately outside its current contract.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/gpio/dw-apb-gpio.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

REG32(SWPORTA_DR,  0x00)
REG32(SWPORTA_DDR, 0x04)
REG32(EXT_PORTA,   0x50)

static void dw_apb_gpio_update(DWAPBGPIOState *s)
{
    uint32_t data = s->regs[R_SWPORTA_DR];
    uint32_t direction = s->regs[R_SWPORTA_DDR];

    s->regs[R_EXT_PORTA] = (data & direction) | (s->input & ~direction);
    for (unsigned int i = 0; i < DW_APB_GPIO_NR_PINS; i++) {
        qemu_set_irq(s->output[i],
                     (direction & BIT(i)) && (data & BIT(i)));
    }
}

static void dw_apb_gpio_data_postw(RegisterInfo *reg, uint64_t value)
{
    dw_apb_gpio_update(DW_APB_GPIO(reg->opaque));
}

static const RegisterAccessInfo dw_apb_gpio_regs_info[] = {
    { .name = "SWPORTA_DR",  .addr = A_SWPORTA_DR,
      .post_write = dw_apb_gpio_data_postw,
    },
    { .name = "SWPORTA_DDR", .addr = A_SWPORTA_DDR,
      .post_write = dw_apb_gpio_data_postw,
    },
    { .name = "EXT_PORTA",   .addr = A_EXT_PORTA,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
};

static const MemoryRegionOps dw_apb_gpio_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void dw_apb_gpio_set(void *opaque, int line, int level)
{
    DWAPBGPIOState *s = opaque;

    s->input = deposit32(s->input, line, 1, !!level);
    dw_apb_gpio_update(s);
}

static void dw_apb_gpio_reset(DeviceState *dev)
{
    DWAPBGPIOState *s = DW_APB_GPIO(dev);

    for (unsigned int i = 0; i < DW_APB_GPIO_NR_REGS; i++) {
        register_reset(&s->regs_info[i]);
    }
    s->input = 0;
    dw_apb_gpio_update(s);
}

static int dw_apb_gpio_post_load(void *opaque, int version_id)
{
    dw_apb_gpio_update(DW_APB_GPIO(opaque));
    return 0;
}

static const VMStateDescription vmstate_dw_apb_gpio = {
    .name = TYPE_DW_APB_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dw_apb_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, DWAPBGPIOState, DW_APB_GPIO_NR_REGS),
        VMSTATE_UINT32(input, DWAPBGPIOState),
        VMSTATE_END_OF_LIST(),
    },
};

static void dw_apb_gpio_init(Object *obj)
{
    DWAPBGPIOState *s = DW_APB_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(DEVICE(obj),
                                         dw_apb_gpio_regs_info,
                                         ARRAY_SIZE(dw_apb_gpio_regs_info),
                                         s->regs_info, s->regs,
                                         &dw_apb_gpio_ops, false,
                                         DW_APB_GPIO_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    qdev_init_gpio_in(DEVICE(obj), dw_apb_gpio_set, DW_APB_GPIO_NR_PINS);
    qdev_init_gpio_out(DEVICE(obj), s->output, DW_APB_GPIO_NR_PINS);
}

static void dw_apb_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Synopsys DesignWare APB GPIO";
    device_class_set_legacy_reset(dc, dw_apb_gpio_reset);
    dc->vmsd = &vmstate_dw_apb_gpio;
}

static const TypeInfo dw_apb_gpio_types[] = {
    {
        .name = TYPE_DW_APB_GPIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(DWAPBGPIOState),
        .instance_init = dw_apb_gpio_init,
        .class_init = dw_apb_gpio_class_init,
    },
};
DEFINE_TYPES(dw_apb_gpio_types)
