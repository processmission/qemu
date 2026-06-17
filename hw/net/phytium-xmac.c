/*
 * Phytium XMAC Ethernet controller model
 *
 * This is a minimal model for firmware and Zephyr/ZVM link bring-up.  It
 * implements the FXMAC MDIO command register and enough MAC registers for the
 * driver to configure the controller and observe a stable PHY link.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/net/phytium-xmac.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"

#define PHYTIUM_XMAC_MMIO_SIZE 0x2000
#define PHYTIUM_XMAC_NUM_REGS (PHYTIUM_XMAC_MMIO_SIZE / sizeof(uint32_t))

#define XMAC_NETWORK_STATUS 0x008
#define XMAC_INTERRUPT_STATUS 0x024
#define XMAC_PHY_MAINTENANCE 0x034
#define XMAC_PCS_STATUS 0x214
#define XMAC_DESIGN_CFG_DEBUG2 0x280

#define XMAC_MDIO_IDLE BIT(2)
#define XMAC_PCS_LINK_UP BIT(15)
#define XMAC_MDIO_OP_MASK 0xf0000000u
#define XMAC_MDIO_OP_WRITE 0x50000000u
#define XMAC_MDIO_OP_READ 0x60000000u
#define XMAC_MDIO_PHY(value) (((value) >> 23) & 0x1f)
#define XMAC_MDIO_REG(value) (((value) >> 18) & 0x1f)
#define XMAC_MDIO_DATA(value) ((value) & 0xffff)

#define PHY_ADDR 0
#define PHY_REG_BMCR 0
#define PHY_REG_BMSR 1
#define PHY_REG_PHYID1 2
#define PHY_REG_PHYID2 3
#define PHY_REG_ANAR 4
#define PHY_REG_ANLPAR 5
#define PHY_REG_1000_CTRL 9
#define PHY_REG_1000_STATUS 10
#define PHY_REG_SPEC_STATUS 17
#define PHY_NUM_REGS 32

#define PHY_BMCR_AUTONEG_ENABLE BIT(12)
#define PHY_BMCR_FULL_DUPLEX BIT(8)
#define PHY_BMCR_SPEED1000 BIT(6)
#define PHY_BMCR_RESET BIT(15)

#define PHY_BMSR_EXTENDED_CAP BIT(0)
#define PHY_BMSR_LINK_STATUS BIT(2)
#define PHY_BMSR_AUTONEG_ABILITY BIT(3)
#define PHY_BMSR_AUTONEG_COMPLETE BIT(5)
#define PHY_BMSR_PREAMBLE_SUPPRESS BIT(6)
#define PHY_BMSR_EXTENDED_STATUS BIT(8)
#define PHY_BMSR_100BASE_T2_HALF BIT(9)
#define PHY_BMSR_100BASE_T2_FULL BIT(10)
#define PHY_BMSR_10_HALF BIT(11)
#define PHY_BMSR_10_FULL BIT(12)
#define PHY_BMSR_100_HALF BIT(13)
#define PHY_BMSR_100_FULL BIT(14)

struct PhytiumXmacState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[PHYTIUM_XMAC_NUM_REGS];
    uint16_t phy_regs[PHY_NUM_REGS];
    uint8_t phy_addr;
};

static uint16_t phytium_xmac_phy_read(PhytiumXmacState *s, uint8_t phy,
                                      uint8_t reg)
{
    if (phy != s->phy_addr) {
        return 0xffff;
    }

    switch (reg) {
    case PHY_REG_BMSR:
        return s->phy_regs[reg] | PHY_BMSR_LINK_STATUS |
               PHY_BMSR_AUTONEG_COMPLETE;
    case PHY_REG_BMCR:
        return s->phy_regs[reg] & ~PHY_BMCR_RESET;
    default:
        return s->phy_regs[reg];
    }
}

static void phytium_xmac_phy_write(PhytiumXmacState *s, uint8_t phy,
                                   uint8_t reg, uint16_t value)
{
    if (phy != s->phy_addr) {
        return;
    }

    if (reg == PHY_REG_BMCR) {
        value &= ~PHY_BMCR_RESET;
    }
    s->phy_regs[reg] = value;
}

static void phytium_xmac_mdio_write(PhytiumXmacState *s, uint32_t value)
{
    uint8_t phy = XMAC_MDIO_PHY(value);
    uint8_t reg = XMAC_MDIO_REG(value);
    uint16_t data = XMAC_MDIO_DATA(value);

    switch (value & XMAC_MDIO_OP_MASK) {
    case XMAC_MDIO_OP_READ:
        data = phytium_xmac_phy_read(s, phy, reg);
        s->regs[XMAC_PHY_MAINTENANCE / sizeof(uint32_t)] =
            (value & 0xffff0000u) | data;
        break;
    case XMAC_MDIO_OP_WRITE:
        phytium_xmac_phy_write(s, phy, reg, data);
        s->regs[XMAC_PHY_MAINTENANCE / sizeof(uint32_t)] = value;
        break;
    default:
        s->regs[XMAC_PHY_MAINTENANCE / sizeof(uint32_t)] = value;
        break;
    }

    s->regs[XMAC_NETWORK_STATUS / sizeof(uint32_t)] |= XMAC_MDIO_IDLE;
}

static uint64_t phytium_xmac_read(void *opaque, hwaddr offset, unsigned size)
{
    PhytiumXmacState *s = opaque;
    uint32_t index = offset / sizeof(uint32_t);

    if (index >= ARRAY_SIZE(s->regs)) {
        return 0;
    }

    switch (offset) {
    case XMAC_NETWORK_STATUS:
        return s->regs[index] | XMAC_MDIO_IDLE;
    case XMAC_PCS_STATUS:
        return s->regs[index] | XMAC_PCS_LINK_UP;
    case XMAC_PHY_MAINTENANCE:
        return s->regs[index];
    default:
        return s->regs[index];
    }
}

static void phytium_xmac_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    PhytiumXmacState *s = opaque;
    uint32_t index = offset / sizeof(uint32_t);

    if (index >= ARRAY_SIZE(s->regs)) {
        return;
    }

    switch (offset) {
    case XMAC_INTERRUPT_STATUS:
        s->regs[index] &= ~(uint32_t)value;
        break;
    case XMAC_PHY_MAINTENANCE:
        phytium_xmac_mdio_write(s, value);
        break;
    default:
        s->regs[index] = value;
        break;
    }
}

static const MemoryRegionOps phytium_xmac_ops = {
    .read = phytium_xmac_read,
    .write = phytium_xmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void phytium_xmac_reset(DeviceState *dev)
{
    PhytiumXmacState *s = PHYTIUM_XMAC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->phy_regs, 0, sizeof(s->phy_regs));

    s->phy_addr = PHY_ADDR;
    s->regs[XMAC_NETWORK_STATUS / sizeof(uint32_t)] = XMAC_MDIO_IDLE;
    s->regs[XMAC_PCS_STATUS / sizeof(uint32_t)] = XMAC_PCS_LINK_UP;
    s->regs[XMAC_DESIGN_CFG_DEBUG2 / sizeof(uint32_t)] = 0;

    s->phy_regs[PHY_REG_BMCR] = PHY_BMCR_AUTONEG_ENABLE |
                                PHY_BMCR_SPEED1000 |
                                PHY_BMCR_FULL_DUPLEX;
    s->phy_regs[PHY_REG_BMSR] = PHY_BMSR_EXTENDED_CAP |
                                PHY_BMSR_LINK_STATUS |
                                PHY_BMSR_AUTONEG_ABILITY |
                                PHY_BMSR_AUTONEG_COMPLETE |
                                PHY_BMSR_PREAMBLE_SUPPRESS |
                                PHY_BMSR_EXTENDED_STATUS |
                                PHY_BMSR_100BASE_T2_HALF |
                                PHY_BMSR_100BASE_T2_FULL |
                                PHY_BMSR_10_HALF |
                                PHY_BMSR_10_FULL |
                                PHY_BMSR_100_HALF |
                                PHY_BMSR_100_FULL;
    s->phy_regs[PHY_REG_PHYID1] = 0x001c;
    s->phy_regs[PHY_REG_PHYID2] = 0xc915;
    s->phy_regs[PHY_REG_ANAR] = 0x01e1;
    s->phy_regs[PHY_REG_ANLPAR] = 0x0de1;
    s->phy_regs[PHY_REG_1000_CTRL] = 0x0300;
    s->phy_regs[PHY_REG_1000_STATUS] = 0x7800;
    s->phy_regs[PHY_REG_SPEC_STATUS] = 0xac00;
}

static void phytium_xmac_init(Object *obj)
{
    PhytiumXmacState *s = PHYTIUM_XMAC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &phytium_xmac_ops, s,
                          TYPE_PHYTIUM_XMAC, PHYTIUM_XMAC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_phytium_xmac = {
    .name = TYPE_PHYTIUM_XMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, PhytiumXmacState, PHYTIUM_XMAC_NUM_REGS),
        VMSTATE_UINT16_ARRAY(phy_regs, PhytiumXmacState, PHY_NUM_REGS),
        VMSTATE_UINT8(phy_addr, PhytiumXmacState),
        VMSTATE_END_OF_LIST()
    },
};

static void phytium_xmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, phytium_xmac_reset);
    dc->vmsd = &vmstate_phytium_xmac;
}

static const TypeInfo phytium_xmac_info = {
    .name = TYPE_PHYTIUM_XMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PhytiumXmacState),
    .instance_init = phytium_xmac_init,
    .class_init = phytium_xmac_class_init,
};

static void phytium_xmac_register_types(void)
{
    type_register_static(&phytium_xmac_info);
}

type_init(phytium_xmac_register_types)
