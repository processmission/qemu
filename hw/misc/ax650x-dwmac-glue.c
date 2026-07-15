/*
 * Axera AX650X DWMAC clock/reset/interface glue
 *
 * The register locations and bit assignments are extracted from the vendor
 * Linux 5.15.73 dwmac-axera driver.  This model stores the RGMII interface,
 * speed mux, and clock enable controls and implements software-reset set/clear
 * aliases.  It does not model clock waveforms or RGMII timing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/ax650x-dwmac-glue.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"

REG32(GLB_EMAC1_PHY_IF, 0x94)
REG32(GLB_EMAC0_PHY_IF, 0x9c)

REG32(CLK_MUX,         0x00)
REG32(CLK_ENABLE0,     0x04)
REG32(CLK_ENABLE1,     0x08)
REG32(CLK_DIV,         0x0c)
REG32(SW_RESET0,       0x10)
REG32(SW_RESET0_SET,   0x38)
REG32(SW_RESET0_CLEAR, 0x3c)

static void ax650x_dwmac_reset_set_postw(RegisterInfo *reg, uint64_t value)
{
    AX650XDWMACGlueState *s = AX650X_DWMAC_GLUE(reg->opaque);
    uint32_t mask = BIT(s->port ? 2 : 7);

    s->clk_regs[R_SW_RESET0] |= value & mask;
    s->clk_regs[R_SW_RESET0_SET] = 0;
}

static void ax650x_dwmac_reset_clear_postw(RegisterInfo *reg, uint64_t value)
{
    AX650XDWMACGlueState *s = AX650X_DWMAC_GLUE(reg->opaque);
    uint32_t mask = BIT(s->port ? 2 : 7);

    s->clk_regs[R_SW_RESET0] &= ~(value & mask);
    s->clk_regs[R_SW_RESET0_CLEAR] = 0;
}

static const RegisterAccessInfo ax650x_dwmac_glb0_regs_info[] = {
    { .name = "EMAC0_PHY_IF", .addr = A_GLB_EMAC0_PHY_IF },
};

static const RegisterAccessInfo ax650x_dwmac_glb1_regs_info[] = {
    { .name = "EMAC1_PHY_IF", .addr = A_GLB_EMAC1_PHY_IF },
};

static const RegisterAccessInfo ax650x_dwmac_clk_regs_info[] = {
    { .name = "CLK_MUX",         .addr = A_CLK_MUX },
    { .name = "CLK_ENABLE0",     .addr = A_CLK_ENABLE0 },
    { .name = "CLK_ENABLE1",     .addr = A_CLK_ENABLE1 },
    { .name = "CLK_DIV",         .addr = A_CLK_DIV },
    { .name = "SW_RESET0",       .addr = A_SW_RESET0,
      .ro = MAKE_64BIT_MASK(0, 32),
    },
    { .name = "SW_RESET0_SET",   .addr = A_SW_RESET0_SET,
      .post_write = ax650x_dwmac_reset_set_postw,
    },
    { .name = "SW_RESET0_CLEAR", .addr = A_SW_RESET0_CLEAR,
      .post_write = ax650x_dwmac_reset_clear_postw,
    },
};

static const MemoryRegionOps ax650x_dwmac_glue_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void ax650x_dwmac_glue_reset(DeviceState *dev)
{
    AX650XDWMACGlueState *s = AX650X_DWMAC_GLUE(dev);

    for (unsigned int i = 0; i < AX650X_DWMAC_GLB_NR_REGS; i++) {
        register_reset(&s->glb_regs_info[i]);
    }
    for (unsigned int i = 0; i < AX650X_DWMAC_CLK_NR_REGS; i++) {
        register_reset(&s->clk_regs_info[i]);
    }
}

static void ax650x_dwmac_glue_realize(DeviceState *dev, Error **errp)
{
    AX650XDWMACGlueState *s = AX650X_DWMAC_GLUE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    const RegisterAccessInfo *glb_regs;
    size_t glb_nr_regs;

    if (s->port > 1) {
        error_setg(errp, "ax650x-dwmac-glue: port must be 0 or 1");
        return;
    }

    if (s->port == 0) {
        glb_regs = ax650x_dwmac_glb0_regs_info;
        glb_nr_regs = ARRAY_SIZE(ax650x_dwmac_glb0_regs_info);
    } else {
        glb_regs = ax650x_dwmac_glb1_regs_info;
        glb_nr_regs = ARRAY_SIZE(ax650x_dwmac_glb1_regs_info);
    }

    s->glb_reg_array = register_init_block32(
        dev, glb_regs, glb_nr_regs, s->glb_regs_info, s->glb_regs,
        &ax650x_dwmac_glue_ops, false, AX650X_DWMAC_GLUE_MMIO_SIZE);
    s->clk_reg_array = register_init_block32(
        dev, ax650x_dwmac_clk_regs_info,
        ARRAY_SIZE(ax650x_dwmac_clk_regs_info), s->clk_regs_info,
        s->clk_regs, &ax650x_dwmac_glue_ops, false,
        AX650X_DWMAC_GLUE_MMIO_SIZE);

    sysbus_init_mmio(sbd, &s->glb_reg_array->mem);
    sysbus_init_mmio(sbd, &s->clk_reg_array->mem);
}

static const VMStateDescription vmstate_ax650x_dwmac_glue = {
    .name = TYPE_AX650X_DWMAC_GLUE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(glb_regs, AX650XDWMACGlueState,
                             AX650X_DWMAC_GLB_NR_REGS),
        VMSTATE_UINT32_ARRAY(clk_regs, AX650XDWMACGlueState,
                             AX650X_DWMAC_CLK_NR_REGS),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property ax650x_dwmac_glue_properties[] = {
    DEFINE_PROP_UINT8("port", AX650XDWMACGlueState, port, 0),
};

static void ax650x_dwmac_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "AX650X DWMAC clock/reset/interface glue";
    dc->realize = ax650x_dwmac_glue_realize;
    device_class_set_legacy_reset(dc, ax650x_dwmac_glue_reset);
    dc->vmsd = &vmstate_ax650x_dwmac_glue;
    device_class_set_props(dc, ax650x_dwmac_glue_properties);
}

static const TypeInfo ax650x_dwmac_glue_types[] = {
    {
        .name = TYPE_AX650X_DWMAC_GLUE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AX650XDWMACGlueState),
        .class_init = ax650x_dwmac_glue_class_init,
    },
};
DEFINE_TYPES(ax650x_dwmac_glue_types)
