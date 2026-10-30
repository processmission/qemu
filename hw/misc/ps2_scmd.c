/*
 * Sony PlayStation 2 CDVD controller S-command (SCMD) interface
 *
 * The CDVD mechanism controller also answers "S-commands" for the
 * real-time clock, the machine name and power control.  The guest writes
 * parameter bytes to SEND, the command number to COMMAND, waits for BUSY
 * to clear and pops the reply from RECV until EMPTY is set.  Commands
 * complete immediately here.  The RTC keeps Japan Standard Time.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/ps2_scmd.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/bcd.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/rtc.h"
#include "system/runstate.h"

enum {
    PS2_SCMD_READ_RTC = 8,
    PS2_SCMD_WRITE_RTC = 9,
    PS2_SCMD_POWER_OFF = 15,
    PS2_SCMD_READ_MACHINE_NAME = 23,
};

/* Japan Standard Time is UTC+9. */
#define PS2_SCMD_JST_OFFSET     (9 * 60 * 60)

static void ps2_scmd_reply(PS2SCMDState *s, const uint8_t *buf, uint32_t len)
{
    memcpy(s->reply, buf, len);
    s->reply_len = len;
    s->reply_pos = 0;
}

static void ps2_scmd_read_rtc(PS2SCMDState *s)
{
    struct tm tm;
    uint8_t buf[8];

    qemu_get_timedate(&tm, s->rtc_offset + PS2_SCMD_JST_OFFSET);
    buf[0] = 0;
    buf[1] = to_bcd(tm.tm_sec);
    buf[2] = to_bcd(tm.tm_min);
    buf[3] = to_bcd(tm.tm_hour);
    buf[4] = 0;
    buf[5] = to_bcd(tm.tm_mday);
    buf[6] = to_bcd(tm.tm_mon + 1);
    buf[7] = to_bcd(tm.tm_year % 100);
    ps2_scmd_reply(s, buf, sizeof(buf));
}

static void ps2_scmd_write_rtc(PS2SCMDState *s)
{
    struct tm tm = { 0 };
    uint8_t status = 0;

    if (s->param_len < 7) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: short write_rtc parameters\n",
                      __func__);
        status = 1;
    } else {
        tm.tm_sec = from_bcd(s->param[0]);
        tm.tm_min = from_bcd(s->param[1]);
        tm.tm_hour = from_bcd(s->param[2]);
        tm.tm_mday = from_bcd(s->param[4]);
        tm.tm_mon = from_bcd(s->param[5]) - 1;
        tm.tm_year = from_bcd(s->param[6]) + 100;
        s->rtc_offset = qemu_timedate_diff(&tm) - PS2_SCMD_JST_OFFSET;
    }
    ps2_scmd_reply(s, &status, 1);
}

static void ps2_scmd_read_machine_name(PS2SCMDState *s)
{
    uint8_t buf[9] = { 0 };
    size_t offset = s->param_len ? s->param[0] : 0;
    size_t len = strlen(s->machine_name);

    if (offset < len) {
        memcpy(&buf[1], s->machine_name + offset, MIN(len - offset, 8));
    }
    ps2_scmd_reply(s, buf, sizeof(buf));
}

static void ps2_scmd_command(PS2SCMDState *s, uint8_t cmd)
{
    uint8_t status = 0;

    switch (cmd) {
    case PS2_SCMD_READ_RTC:
        ps2_scmd_read_rtc(s);
        break;
    case PS2_SCMD_WRITE_RTC:
        ps2_scmd_write_rtc(s);
        break;
    case PS2_SCMD_POWER_OFF:
        ps2_scmd_reply(s, &status, 1);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        break;
    case PS2_SCMD_READ_MACHINE_NAME:
        ps2_scmd_read_machine_name(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented command %u\n",
                      __func__, cmd);
        s->reply_len = 0;
        s->reply_pos = 0;
        break;
    }
    s->param_len = 0;
}

static uint64_t ps2_scmd_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2SCMDState *s = opaque;

    switch (addr) {
    case PS2_SCMD_STATUS:
        return s->reply_pos < s->reply_len ? 0 : PS2_SCMD_STATUS_EMPTY;
    case PS2_SCMD_RECV:
        if (s->reply_pos < s->reply_len) {
            return s->reply[s->reply_pos++];
        }
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void ps2_scmd_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned int size)
{
    PS2SCMDState *s = opaque;

    switch (addr) {
    case PS2_SCMD_COMMAND:
        ps2_scmd_command(s, value);
        break;
    case PS2_SCMD_SEND:
        if (s->param_len < PS2_SCMD_FIFO_SIZE) {
            s->param[s->param_len++] = value;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps ps2_scmd_ops = {
    .read = ps2_scmd_read,
    .write = ps2_scmd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ps2_scmd_reset_hold(Object *obj, ResetType type)
{
    PS2SCMDState *s = PS2_SCMD(obj);

    s->param_len = 0;
    s->reply_len = 0;
    s->reply_pos = 0;
}

static void ps2_scmd_init(Object *obj)
{
    PS2SCMDState *s = PS2_SCMD(obj);

    memory_region_init_io(&s->iomem, obj, &ps2_scmd_ops, s, TYPE_PS2_SCMD,
                          PS2_CDVD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property ps2_scmd_properties[] = {
    DEFINE_PROP_STRING("machine-name", PS2SCMDState, machine_name),
};

static void ps2_scmd_realize(DeviceState *dev, Error **errp)
{
    PS2SCMDState *s = PS2_SCMD(dev);

    if (!s->machine_name) {
        s->machine_name = g_strdup("SCPH-50004");
    }
}

static bool ps2_scmd_fifo_valid(void *opaque, int version_id)
{
    PS2SCMDState *s = opaque;

    return s->param_len <= PS2_SCMD_FIFO_SIZE &&
           s->reply_len <= PS2_SCMD_FIFO_SIZE &&
           s->reply_pos <= s->reply_len;
}

static const VMStateDescription vmstate_ps2_scmd = {
    .name = TYPE_PS2_SCMD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(param, PS2SCMDState, PS2_SCMD_FIFO_SIZE),
        VMSTATE_UINT32(param_len, PS2SCMDState),
        VMSTATE_UINT8_ARRAY(reply, PS2SCMDState, PS2_SCMD_FIFO_SIZE),
        VMSTATE_UINT32(reply_len, PS2SCMDState),
        VMSTATE_UINT32(reply_pos, PS2SCMDState),
        VMSTATE_INT64(rtc_offset, PS2SCMDState),
        VMSTATE_VALIDATE("fifos in range", ps2_scmd_fifo_valid),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_scmd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_scmd_reset_hold;
    dc->realize = ps2_scmd_realize;
    dc->vmsd = &vmstate_ps2_scmd;
    device_class_set_props(dc, ps2_scmd_properties);
}

static const TypeInfo ps2_scmd_types[] = {
    {
        .name = TYPE_PS2_SCMD,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2SCMDState),
        .instance_init = ps2_scmd_init,
        .class_init = ps2_scmd_class_init,
    },
};

DEFINE_TYPES(ps2_scmd_types);
