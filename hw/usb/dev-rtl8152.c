/*
 * QEMU Realtek RTL8152 USB Ethernet device
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/net.h"
#include "system/system.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qom/object.h"

/*#define TRAFFIC_DEBUG*/

#define RTL8152_VENDOR_NUM      0x0bda
#define RTL8152_PRODUCT_NUM     0x8152

#define RTL8152_BUFSIZE         32768

#define RTL8152_REQ_GET_REGS    0x05
#define RTL8152_REQ_SET_REGS    0x05
#define RTL8152_MCU_TYPE_PLA    0x0100
#define RTL8152_BYTE_EN_MASK    0x000f
#define RTL8152_BYTE_EN_ALL     0x000f
#define RTL8152_PLA_IDR         0xc000
#define RTL8152_PLA_BOOT_CTRL   0xe004
#define RTL8152_PLA_TCR0        0xe610
#define RTL8152_PLA_TCR1        0xe612
#define RTL8152_PLA_CR          0xe813
#define RTL8152_PLA_OCP_BASE    0xe86c
#define RTL8152_PLA_PHYSTATUS   0xe908
#define RTL8152_OCP_BASE_MII    0xa400
#define RTL8152_OCP_PHY_STATUS  0xa420
#define RTL8152_RX_DESC_SIZE    24
#define RTL8152_TX_DESC_SIZE    8
#define RTL8152_RX_FCS_SIZE     4
#define RTL8152_RX_LEN_MASK     0x7fff
#define RTL8152_TX_LEN_MASK     0x3ffff
#define RTL8152_TX_IPV4_CS      (1u << 29)
#define RTL8152_TX_TCP_CS       (1u << 30)
#define RTL8152_TX_UDP_CS       (1u << 31)
#define RTL8152_TX_ALIGN        4
#define RTL8152_INTR_LINK       0x0004
#define RTL8152_LINK_100_FULL   0x0b
#define RTL8152_AUTOLOAD_DONE   (1u << 2)
#define RTL8152_CR_RST          0x10
#define RTL8152_TCR0_TX_EMPTY   0x0800

#define TYPE_USB_RTL8152 "usb-rtl8152"
OBJECT_DECLARE_SIMPLE_TYPE(USBRtl8152State, USB_RTL8152)

enum {
    STRING_MANUFACTURER = 1,
    STRING_PRODUCT,
    STRING_ETHADDR,
    STRING_SERIALNUMBER,
    STRING_RTL8152,
};

struct USBRtl8152State {
    USBDevice dev;

    unsigned int out_ptr;
    uint8_t out_buf[RTL8152_BUFSIZE];

    unsigned int in_ptr, in_len;
    uint8_t in_buf[RTL8152_BUFSIZE];

    USBEndpoint *bulk_in;

    char usbstring_mac[13];
    NICState *nic;
    NICConf conf;

    uint16_t rtl_ocp_base;
    uint8_t rtl_pla[UINT16_MAX + 1];
    uint8_t rtl_usb[UINT16_MAX + 1];
    uint8_t rtl_ocp[UINT16_MAX + 1];
};

static const USBDescIface desc_iface_rtl8152[] = {
    {
        .bInterfaceNumber              = 0,
        .bNumEndpoints                 = 3,
        .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
        .iInterface                    = STRING_RTL8152,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | 0x01,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x40,
            },{
                .bEndpointAddress      = USB_DIR_OUT | 0x02,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x40,
            },{
                .bEndpointAddress      = USB_DIR_IN | 0x03,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 0x02,
                .bInterval             = 8,
            }
        },
    }
};

static const USBDescIface desc_iface_rtl8152_high[] = {
    {
        .bInterfaceNumber              = 0,
        .bNumEndpoints                 = 3,
        .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
        .iInterface                    = STRING_RTL8152,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | 0x01,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x200,
            },{
                .bEndpointAddress      = USB_DIR_OUT | 0x02,
                .bmAttributes          = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize        = 0x200,
            },{
                .bEndpointAddress      = USB_DIR_IN | 0x03,
                .bmAttributes          = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize        = 0x02,
                .bInterval             = 4,
            }
        },
    }
};

static const USBDescDevice desc_device_rtl8152 = {
    .bcdUSB                        = 0x0200,
    .bDeviceClass                  = USB_CLASS_VENDOR_SPEC,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STRING_RTL8152,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower             = 0x32,
            .nif = ARRAY_SIZE(desc_iface_rtl8152),
            .ifs = desc_iface_rtl8152,
        }
    },
};

static const USBDescDevice desc_device_rtl8152_high = {
    .bcdUSB                        = 0x0200,
    .bDeviceClass                  = USB_CLASS_VENDOR_SPEC,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .iConfiguration        = STRING_RTL8152,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower             = 0x32,
            .nif = ARRAY_SIZE(desc_iface_rtl8152_high),
            .ifs = desc_iface_rtl8152_high,
        }
    },
};

static const USBDescStrings usb_rtl8152_stringtable = {
    [STRING_MANUFACTURER]       = "Realtek",
    [STRING_PRODUCT]            = "USB 10/100 LAN",
    [STRING_ETHADDR]            = "400102030405",
    [STRING_SERIALNUMBER]       = "1",
    [STRING_RTL8152]            = "Realtek RTL8152",
};

static const USBDesc desc_rtl8152 = {
    .id = {
        .idVendor          = RTL8152_VENDOR_NUM,
        .idProduct         = RTL8152_PRODUCT_NUM,
        .bcdDevice         = 0x2000,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_rtl8152,
    .high = &desc_device_rtl8152_high,
    .str  = usb_rtl8152_stringtable,
};

static uint8_t *rtl8152_space(USBRtl8152State *s, int index, uint16_t addr)
{
    if (index & RTL8152_MCU_TYPE_PLA) {
        if ((addr & 0xf000) == 0xb000) {
            return &s->rtl_ocp[(s->rtl_ocp_base & 0xf000) | (addr & 0x0fff)];
        }
        return &s->rtl_pla[addr];
    }

    return &s->rtl_usb[addr];
}

static uint16_t rtl8152_mii_addr(unsigned int reg)
{
    return RTL8152_OCP_BASE_MII + reg * 2;
}

static void rtl8152_write_le16(uint8_t *space, uint16_t addr, uint16_t value)
{
    stw_le_p(&space[addr], value);
}

static void rtl8152_reset_regs(USBRtl8152State *s)
{
    memset(s->rtl_pla, 0, sizeof(s->rtl_pla));
    memset(s->rtl_usb, 0, sizeof(s->rtl_usb));
    memset(s->rtl_ocp, 0, sizeof(s->rtl_ocp));

    s->rtl_ocp_base = 0;
    memcpy(&s->rtl_pla[RTL8152_PLA_IDR], s->conf.macaddr.a,
           sizeof(s->conf.macaddr.a));
    rtl8152_write_le16(s->rtl_pla, RTL8152_PLA_TCR0, RTL8152_TCR0_TX_EMPTY);
    rtl8152_write_le16(s->rtl_pla, RTL8152_PLA_TCR1, 0x4c00);
    s->rtl_pla[RTL8152_PLA_BOOT_CTRL] = RTL8152_AUTOLOAD_DONE;
    s->rtl_pla[RTL8152_PLA_CR] = 0;
    s->rtl_pla[RTL8152_PLA_PHYSTATUS] = RTL8152_LINK_100_FULL;
    rtl8152_write_le16(s->rtl_ocp, RTL8152_OCP_PHY_STATUS, 0x0003);
    rtl8152_write_le16(s->rtl_ocp, rtl8152_mii_addr(0), 0x3100);
    rtl8152_write_le16(s->rtl_ocp, rtl8152_mii_addr(1), 0x786d);
    rtl8152_write_le16(s->rtl_ocp, rtl8152_mii_addr(2), 0x001c);
    rtl8152_write_le16(s->rtl_ocp, rtl8152_mii_addr(3), 0xc912);
    rtl8152_write_le16(s->rtl_ocp, rtl8152_mii_addr(4), 0x01e1);
    rtl8152_write_le16(s->rtl_ocp, rtl8152_mii_addr(5), 0x45e1);
    rtl8152_write_le16(s->rtl_ocp, rtl8152_mii_addr(9), 0x0200);
}

static void rtl8152_update_ocp_base(USBRtl8152State *s, uint16_t addr,
                                    const uint8_t *data, unsigned int size,
                                    unsigned int byen)
{
    unsigned int shift = RTL8152_PLA_OCP_BASE & 3;

    if (addr != (RTL8152_PLA_OCP_BASE & ~3) || size < sizeof(uint32_t)) {
        return;
    }

    if (byen & (3u << shift)) {
        uint32_t val = ldl_le_p(data);

        s->rtl_ocp_base = (val >> (shift * 8)) & 0xf000;
    }
}

static void rtl8152_reg_write(USBRtl8152State *s, int index, uint16_t addr,
                              const uint8_t *data, unsigned int size)
{
    uint8_t *space = rtl8152_space(s, index, addr);
    unsigned int byen = index & RTL8152_BYTE_EN_MASK;

    if (!byen) {
        byen = RTL8152_BYTE_EN_ALL;
    }

    for (unsigned int i = 0; i < size; i++) {
        if (byen & (1u << ((addr + i) & 3))) {
            space[i] = data[i];
        }
    }

    if (index & RTL8152_MCU_TYPE_PLA) {
        rtl8152_update_ocp_base(s, addr, data, size, byen);
    }

    if (addr <= RTL8152_PLA_CR && addr + size > RTL8152_PLA_CR) {
        s->rtl_pla[RTL8152_PLA_CR] &= ~RTL8152_CR_RST;
    }
}

static void rtl8152_reg_read(USBRtl8152State *s, int index, uint16_t addr,
                             uint8_t *data, unsigned int size)
{
    uint8_t *space = rtl8152_space(s, index, addr);

    memcpy(data, space, size);
}

static void rtl8152_reset_in_buf(USBRtl8152State *s)
{
    s->in_ptr = s->in_len = 0;
    if (s->nic) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void rtl8152_handle_reset(USBDevice *dev)
{
    USBRtl8152State *s = USB_RTL8152(dev);

    s->out_ptr = 0;
    rtl8152_reset_in_buf(s);
    rtl8152_reset_regs(s);
}

static void rtl8152_handle_control(USBDevice *dev, USBPacket *p, int request,
                                   int value, int index, int length,
                                   uint8_t *data)
{
    USBRtl8152State *s = USB_RTL8152(dev);
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case VendorDeviceRequest | RTL8152_REQ_GET_REGS:
        rtl8152_reg_read(s, index, value, data, length);
        p->actual_length = length;
        return;

    case VendorDeviceOutRequest | RTL8152_REQ_SET_REGS:
        rtl8152_reg_write(s, index, value, data, length);
        return;

    default:
        fprintf(stderr, "rtl8152: failed control transaction: "
                        "request 0x%x value 0x%x index 0x%x length 0x%x\n",
                        request, value, index, length);
        p->status = USB_RET_STALL;
        return;
    }
}

static void rtl8152_handle_statusin(USBRtl8152State *s, USBPacket *p)
{
    uint16_t link = cpu_to_le16(RTL8152_INTR_LINK);

    if (p->iov.size < sizeof(link)) {
        p->status = USB_RET_STALL;
        return;
    }

    usb_packet_copy(p, &link, sizeof(link));
}

static void rtl8152_handle_datain(USBRtl8152State *s, USBPacket *p)
{
    int len;

    if (s->in_ptr > s->in_len) {
        rtl8152_reset_in_buf(s);
        p->status = USB_RET_NAK;
        return;
    }
    if (!s->in_len) {
        p->status = USB_RET_NAK;
        return;
    }

    len = s->in_len - s->in_ptr;
    if (len > p->iov.size) {
        len = p->iov.size;
    }

    usb_packet_copy(p, &s->in_buf[s->in_ptr], len);
    s->in_ptr += len;
    if (s->in_ptr >= s->in_len && ((s->in_len & (64 - 1)) || !len)) {
        rtl8152_reset_in_buf(s);
    }

#ifdef TRAFFIC_DEBUG
    fprintf(stderr, "rtl8152: data in len %zu return %d", p->iov.size, len);
    iov_hexdump(p->iov.iov, p->iov.niov, stderr, "rtl8152", len);
#endif
}

static void rtl8152_fix_tx_checksum(uint8_t *frame, size_t frame_len,
                                    uint32_t opts2)
{
    int csum_flags = 0;

    if (opts2 & RTL8152_TX_IPV4_CS) {
        csum_flags |= CSUM_IP;
    }
    if (opts2 & RTL8152_TX_TCP_CS) {
        csum_flags |= CSUM_TCP;
    }
    if (opts2 & RTL8152_TX_UDP_CS) {
        csum_flags |= CSUM_UDP;
    }

    if (csum_flags) {
        net_checksum_calculate(frame, frame_len, csum_flags);
    }
}

static void rtl8152_handle_dataout(USBRtl8152State *s, USBPacket *p)
{
    size_t pos = 0;

#ifdef TRAFFIC_DEBUG
    fprintf(stderr, "rtl8152: data out len %zu\n", p->iov.size);
    iov_hexdump(p->iov.iov, p->iov.niov, stderr, "rtl8152", p->iov.size);
#endif

    if (p->iov.size > sizeof(s->out_buf) - s->out_ptr) {
        s->out_ptr = 0;
        p->status = USB_RET_STALL;
        return;
    }

    usb_packet_copy(p, &s->out_buf[s->out_ptr], p->iov.size);
    s->out_ptr += p->iov.size;

    while (pos + RTL8152_TX_DESC_SIZE <= s->out_ptr) {
        uint32_t opts1 = ldl_le_p(&s->out_buf[pos]);
        uint32_t opts2 = ldl_le_p(&s->out_buf[pos + sizeof(opts1)]);
        uint32_t frame_len = opts1 & RTL8152_TX_LEN_MASK;
        size_t frame_off = pos + RTL8152_TX_DESC_SIZE;
        size_t frame_end = frame_off + frame_len;
        size_t next;

        if (!frame_len ||
            frame_len > sizeof(s->out_buf) - RTL8152_TX_DESC_SIZE ||
            frame_end > s->out_ptr) {
            break;
        }

        rtl8152_fix_tx_checksum(&s->out_buf[frame_off], frame_len, opts2);
        qemu_send_packet(qemu_get_queue(s->nic), &s->out_buf[frame_off],
                         frame_len);
        next = QEMU_ALIGN_UP(frame_end, RTL8152_TX_ALIGN);
        pos = MIN(next, (size_t)s->out_ptr);
    }

    if (pos) {
        s->out_ptr -= pos;
        memmove(s->out_buf, &s->out_buf[pos], s->out_ptr);
    }
}

static void rtl8152_handle_data(USBDevice *dev, USBPacket *p)
{
    USBRtl8152State *s = USB_RTL8152(dev);

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 1) {
            rtl8152_handle_datain(s, p);
        } else if (p->ep->nr == 3) {
            rtl8152_handle_statusin(s, p);
        } else {
            p->status = USB_RET_STALL;
        }
        break;

    case USB_TOKEN_OUT:
        if (p->ep->nr == 2) {
            rtl8152_handle_dataout(s, p);
        } else {
            p->status = USB_RET_STALL;
        }
        break;

    default:
        p->status = USB_RET_STALL;
        break;
    }

    if (p->status == USB_RET_STALL) {
        fprintf(stderr, "rtl8152: failed data transaction: "
                        "pid 0x%x ep 0x%x len 0x%zx\n",
                        p->pid, p->ep->nr, p->iov.size);
    }
}

static ssize_t rtl8152_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    USBRtl8152State *s = qemu_get_nic_opaque(nc);
    size_t payload_len = MAX(size, (size_t)ETH_ZLEN);
    size_t packet_len = payload_len + RTL8152_RX_FCS_SIZE;
    size_t total_size = RTL8152_RX_DESC_SIZE + packet_len;

    if (total_size > sizeof(s->in_buf)) {
        return -1;
    }

    if (s->in_len > 0) {
        return 0;
    }

    memset(s->in_buf, 0, total_size);
    stl_le_p(s->in_buf, packet_len & RTL8152_RX_LEN_MASK);
    memcpy(&s->in_buf[RTL8152_RX_DESC_SIZE], buf, size);

    s->in_len = total_size;
    s->in_ptr = 0;
    usb_wakeup(s->bulk_in, 0);
    return size;
}

static void rtl8152_cleanup(NetClientState *nc)
{
    USBRtl8152State *s = qemu_get_nic_opaque(nc);

    s->nic = NULL;
}

static NetClientInfo net_rtl8152_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = rtl8152_receive,
    .cleanup = rtl8152_cleanup,
};

static void rtl8152_realize(USBDevice *dev, Error **errp)
{
    USBRtl8152State *s = USB_RTL8152(dev);

    usb_desc_create_serial(dev);
    usb_desc_init(dev);

    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, 1);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_rtl8152_info, &s->conf,
                          object_get_typename(OBJECT(s)), s->dev.qdev.id,
                          &s->dev.qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    snprintf(s->usbstring_mac, sizeof(s->usbstring_mac),
             "%02x%02x%02x%02x%02x%02x",
             s->conf.macaddr.a[0], s->conf.macaddr.a[1],
             s->conf.macaddr.a[2], s->conf.macaddr.a[3],
             s->conf.macaddr.a[4], s->conf.macaddr.a[5]);
    usb_desc_set_string(dev, STRING_ETHADDR, s->usbstring_mac);

    rtl8152_reset_regs(s);
}

static void rtl8152_unrealize(USBDevice *dev)
{
    USBRtl8152State *s = USB_RTL8152(dev);

    qemu_del_nic(s->nic);
}

static void rtl8152_instance_init(Object *obj)
{
    USBDevice *dev = USB_DEVICE(obj);
    USBRtl8152State *s = USB_RTL8152(dev);

    device_add_bootindex_property(obj, &s->conf.bootindex,
                                  "bootindex", "/ethernet-phy@0",
                                  &dev->qdev);
}

static const VMStateDescription vmstate_usb_rtl8152 = {
    .name = "usb-rtl8152",
    .unmigratable = 1,
};

static const Property rtl8152_properties[] = {
    DEFINE_NIC_PROPERTIES(USBRtl8152State, conf),
};

static void rtl8152_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = rtl8152_realize;
    uc->product_desc   = "Realtek RTL8152 USB Ethernet";
    uc->usb_desc       = &desc_rtl8152;
    uc->handle_attach  = usb_desc_attach;
    uc->handle_reset   = rtl8152_handle_reset;
    uc->handle_control = rtl8152_handle_control;
    uc->handle_data    = rtl8152_handle_data;
    uc->unrealize      = rtl8152_unrealize;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->fw_name = "network";
    dc->vmsd = &vmstate_usb_rtl8152;
    device_class_set_props(dc, rtl8152_properties);
}

static const TypeInfo rtl8152_info = {
    .name          = TYPE_USB_RTL8152,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBRtl8152State),
    .class_init    = rtl8152_class_initfn,
    .instance_init = rtl8152_instance_init,
};

static void rtl8152_register_types(void)
{
    type_register_static(&rtl8152_info);
}

type_init(rtl8152_register_types)
