/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VL53L8CX SPI/DCI interface with measured frames supplied by MuJoCo. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "hw/ssi/ssi.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"

#define TYPE_VL53L8CX "microduck-vl53l8cx"
OBJECT_DECLARE_SIMPLE_TYPE(VL53L8CXState, VL53L8CX)
#define FW_SIZE 0x15000
#define FRAME_LIMIT 4096

struct VL53L8CXState {
    SSIPeripheral parent_obj;
    CharFrontend depth;
    uint8_t memory[3][0x8000];
    uint8_t firmware[FW_SIZE];
    uint8_t written[FW_SIZE];
    unsigned written_count;
    bool firmware_valid;
    uint8_t dci[65536];
    uint8_t page;
    uint16_t address, transaction_start;
    unsigned position;
    bool writing, ranging;
    uint8_t streamcount;
    uint8_t input[192];
    unsigned input_length;
    uint8_t frame[FRAME_LIMIT], snapshot[FRAME_LIMIT];
    unsigned frame_length;
    unsigned zones;
};

static void swap_words(uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t word = ldl_le_p(bytes + i);
        stl_be_p(bytes + i, word);
    }
}

static void ui_status(VL53L8CXState *s, unsigned first, unsigned second,
                      unsigned error)
{
    s->memory[2][0x2c00] = first;
    s->memory[2][0x2c01] = second;
    s->memory[2][0x2c02] = error;
    s->memory[2][0x2c03] = 0;
    if (error >= 0x7f) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "vl53l8cx: UI rejected at %04x footer=%02x:%02x:%02x:%02x\n",
            s->transaction_start, s->memory[2][0x2ffc], s->memory[2][0x2ffd],
            s->memory[2][0x2ffe], s->memory[2][0x2fff]);
    }
}

static bool dci_write_blocks(VL53L8CXState *s)
{
    uint8_t *mem = s->memory[2];
    unsigned length = lduw_be_p(mem + 0x2ffe);
    unsigned cursor = s->transaction_start;
    if (length + 4 != 0x3000 - cursor) {
        return false;
    }
    while (cursor < 0x2ff8) {
        unsigned index = lduw_be_p(mem + cursor);
        unsigned size = ((unsigned)mem[cursor + 2] << 4) |
                        (mem[cursor + 3] >> 4);
        unsigned type = mem[cursor + 3] & 15;
        if (type > 0 && type < 13) {
            size *= type;
        }
        if (!size || (size & 3) || index + size > sizeof(s->dci) ||
            cursor + 4 + size > 0x2ff8) {
            qemu_log_mask(LOG_GUEST_ERROR,
                "vl53l8cx: invalid DCI block %04x size %u at %04x\n",
                index, size, cursor);
            return false;
        }
        memcpy(s->dci + index, mem + cursor + 4, size);
        swap_words(s->dci + index, size);
        cursor += size + 4;
    }
    return cursor == 0x2ff8;
}

static void firmware_check(VL53L8CXState *s)
{
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                        s->firmware, FW_SIZE);
    s->firmware_valid = s->written_count == FW_SIZE &&
        !strcmp(hash, "be6b5eeda86a0040b11d12eb824c9587f3feac41"
                       "caa6c48f1d65c207c216c4e5");
    /* This is the checksum exposed by the supported, verified ST firmware. */
    stl_be_p(&s->memory[2][0x2ffc], s->firmware_valid ? 0x28c6adac : 0);
    if (!s->firmware_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vl53l8cx: unsupported or incomplete firmware upload\n");
    }
}

static void ui_execute(VL53L8CXState *s)
{
    uint8_t *mem = s->memory[2];
    unsigned code = mem[0x2ffd];
    if (!s->firmware_valid) {
        ui_status(s, 0, 0, 0x80);
        return;
    }
    if (mem[0x2ffc] == 0 && code == 3 && mem[0x2ffe] == 0 && mem[0x2fff] == 0) {
        unsigned size = ldl_le_p(&s->dci[0xd968]);
        unsigned zones = s->dci[0x5450] * s->dci[0x5451];
        if ((zones != 16 && zones != 64) || size < 24 || size > FRAME_LIMIT ||
            (size & 3)) {
            ui_status(s, 0, 0, 0x80);
            return;
        }
        s->frame_length = size;
        s->zones = zones;
        stw_le_p(&s->dci[0x5448], size);
        s->ranging = true;
        s->frame[0] = 255;
        ui_status(s, 0, 3, 0);
    } else if (s->transaction_start == 0x2fd8) {
        /* The ideal optical model uses zero per-zone range offsets. */
        memset(mem + 0x2c04, 0, 492);
        ui_status(s, 2, 0, 0);
    } else if (code == 1 && (mem[0x2ffc] == 0 || mem[0x2ffc] == 5)) {
        bool valid = dci_write_blocks(s);
        ui_status(s, 0, valid ? 3 : 0, valid ? 0 : 0x80);
    } else if (code == 2 && mem[0x2ffc] == 0) {
        uint8_t *command = mem + 0x2ff4;
        unsigned index = lduw_be_p(command);
        unsigned size = ((unsigned)command[2] << 4) | (command[3] >> 4);
        if (index + size > sizeof(s->dci) || size + 12 > 0x3fc || (size & 3)) {
            ui_status(s, 0, 0, 0x80);
            return;
        }
        memset(mem + 0x2c04, 0, size + 12);
        memcpy(mem + 0x2c08, s->dci + index, size);
        swap_words(mem + 0x2c04, size + 12);
        ui_status(s, 0, 3, 0);
    } else if (code == 1 && mem[0x2ffc] == 3) {
        /*
         * Offset, cross-talk and default configuration command payloads
         * are retained.
         */
        ui_status(s, 0, 3, 0);
    } else {
        ui_status(s, 0, 0, 0x80);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vl53l8cx: unsupported UI command %02x:%02x\n",
                      mem[0x2ffc], code);
    }
}

static void depth_frame(VL53L8CXState *s)
{
    if (!s->ranging || !s->firmware_valid || !s->frame_length) {
        return;
    }
    if (s->zones != 16 && s->zones != 64) {
        ui_status(s, 0, 0, 0x80);
        s->ranging = false;
        return;
    }
    memset(s->frame, 0, s->frame_length);
    unsigned position = 16;
    unsigned zones = s->zones;
    unsigned count = MIN(ldl_le_p(&s->dci[0xd96c]), 12);
    for (unsigned i = 0; i < count; i++) {
        uint32_t header = ldl_le_p(&s->dci[0xd980 + i * 4]);
        if (!(ldl_le_p(&s->dci[0xd970 + (i / 32) * 4]) &
              (1u << (i % 32)))) {
            continue;
        }
        unsigned type = header & 15;
        unsigned elements = (header >> 4) & 0xfff;
        unsigned bytes = (type > 1 && type < 13) ? type * elements : elements;
        if (position + bytes + 4 > s->frame_length - 8) {
            ui_status(s, 0, 0, 0x80);
            s->ranging = false;
            return;
        }
        stl_le_p(s->frame + position, header);
        uint8_t *payload = s->frame + position + 4;
        unsigned index = header >> 16;
        if (index == 0xdf44 && bytes == zones * 2) {
            for (unsigned zone = 0; zone < zones; zone++) {
                unsigned src = zones == 64 ? zone :
                               (zone / 4 * 2) * 8 + zone % 4 * 2;
                uint16_t mm = lduw_le_p(s->input + src * 2);
                stw_le_p(payload + zone * 2, MIN(mm, 8191) * 4);
            }
        } else if (index == 0xe084 && bytes == zones) {
            for (unsigned zone = 0; zone < zones; zone++) {
                unsigned src = zones == 64 ? zone :
                               (zone / 4 * 2) * 8 + zone % 4 * 2;
                payload[zone] = s->input[128 + src];
            }
        }
        position += bytes + 4;
    }
    s->streamcount = (s->streamcount + 1) % 255;
    s->frame[8] = s->frame[s->frame_length - 4] = 0;
    s->frame[9] = s->frame[s->frame_length - 3] = s->streamcount;
    swap_words(s->frame, s->frame_length);
    s->frame[0] = s->streamcount;
    s->frame[1] = 5;
    s->frame[2] = 5;
    s->frame[3] = 0x10;
}

static int depth_can_read(void *opaque) { return 4096; }
static void depth_receive(void *opaque, const uint8_t *bytes, int size)
{
    VL53L8CXState *s = opaque;
    for (int i = 0; i < size; i++) {
        s->input[s->input_length++] = bytes[i];
        if (s->input_length == sizeof(s->input)) {
            depth_frame(s);
            s->input_length = 0;
        }
    }
}

static void register_write(VL53L8CXState *s, uint16_t address, uint8_t value)
{
    if (s->transaction_start == 0x7fff && s->position == 2) {
        s->page = value;
        return;
    }
    if (s->page >= 9 && s->page <= 11) {
        unsigned index = (s->page - 9) * 0x8000 + address;
        if (index < FW_SIZE) {
            s->firmware[index] = value;
            if (!s->written[index]) {
                s->written[index] = 1;
                s->written_count++;
            }
        }
        return;
    }
    if (s->page > 2) {
        return;
    }
    s->memory[s->page][address] = value;
    if (s->page == 0) {
        if (address == 0x0a && value == 1) {
            s->memory[0][6] = 1;
        }
        if (address == 0x0b && value == 1 && s->firmware_valid) {
            s->memory[0][6] = 1;
        }
        if (address == 0x14 && value == 1) {
            s->ranging = false;
            s->memory[0][6] = 0x80;
            s->memory[0][7] = 0x84;
        }
    }
    if (s->page == 1 && address == 6) {
        if (value == 1) {
            s->memory[1][0x21] = 4;
        }
        if (value == 3) {
            firmware_check(s);
        }
    }
}
static uint8_t register_read(VL53L8CXState *s, uint16_t address)
{
    if (address == 0x7fff) {
        return s->page;
    }
    if (s->page == 0 && address < 2) {
        return address ? 0x0c : 0xf0;
    }
    if (s->page == 2 && address < s->frame_length &&
        s->transaction_start == 0) {
        return s->snapshot[address];
    }
    return s->page < 3 ? s->memory[s->page][address] : 0;
}
static uint32_t tof_transfer(SSIPeripheral *device, uint32_t value)
{
    VL53L8CXState *s = VL53L8CX(device);
    uint8_t result = 0;
    if (s->position == 0) {
        s->writing = (value & 0x80) != 0;
        s->address = (value & 0x7f) << 8;
    } else if (s->position == 1) {
        s->address |= value & 255;
        s->transaction_start = s->address;
        if (!s->writing && s->page == 2 && !s->address) {
            memcpy(s->snapshot, s->frame, s->frame_length);
        }
    } else {
        if (s->writing) {
            register_write(s, s->address, value);
        } else {
            result = register_read(s, s->address);
        }
        s->address = (s->address + 1) & 0x7fff;
    }
    s->position++;
    return result;
}
static int tof_cs(SSIPeripheral *device, bool level)
{
    VL53L8CXState *s = VL53L8CX(device);
    if (!level) {
        s->position = 0;
    } else if (s->writing && s->page == 2 && s->position > 2 &&
               s->transaction_start >= 0x2c04 && s->address == 0x3000) {
        ui_execute(s);
    }
    return 0;
}
static void tof_realize(SSIPeripheral *device, Error **errp)
{
    VL53L8CXState *s = VL53L8CX(device);
    s->page = 2;
    s->memory[0][6] = 1;
    s->memory[0][9] = 4;
    s->dci[0x5450] = s->dci[0x5451] = 4;
    s->dci[0x5454] = s->dci[0x5455] = 8;
    s->dci[0x5459] = 15;
    s->frame[0] = 255;
    qemu_chr_fe_set_handlers(&s->depth, depth_can_read, depth_receive,
                             NULL, NULL, s, NULL, true);
}
static const Property tof_properties[] = {
    DEFINE_PROP_CHR("chardev", VL53L8CXState, depth),
};
static void tof_class_init(ObjectClass *oc, const void *data)
{
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);
    sc->realize = tof_realize;
    sc->transfer = tof_transfer;
    sc->set_cs = tof_cs;
    sc->cs_polarity = SSI_CS_LOW;
    device_class_set_props(DEVICE_CLASS(oc), tof_properties);
}
static const TypeInfo tof_info = {
    .name = TYPE_VL53L8CX, .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(VL53L8CXState), .class_init = tof_class_init,
};
static void tof_register(void) { type_register_static(&tof_info); }
type_init(tof_register)
