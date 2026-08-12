#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The rig section layout and the initial effect/amp identifier catalog are
 * adapted from ElevenHack by Guillaume Schmid (2013-2020), Apache-2.0.
 * This file has been substantially modified and extended for the native
 * Eleven Rack Driver project. See NOTICE.md and
 * LICENSES/ElevenHack-Apache-2.0.txt.
 */

#define ER_VID 0x0DBA
#define ER_PID 0xB011
#define ER_MIDI_IF 2
#define ER_MAX_SYSEX (1024U * 1024U)

static IOUSBDeviceInterface500 **open_device(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    IOUSBDeviceInterface500 **device = NULL;
    SInt32 score = 0;

    if (IOCreatePlugInInterfaceForService(service,
                                          kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID,
                                          &plugin, &score) != KERN_SUCCESS ||
        !plugin)
        return NULL;

    (*plugin)->QueryInterface(plugin,
                              CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),
                              (LPVOID *)&device);
    (*plugin)->Release(plugin);
    return device;
}

static IOUSBInterfaceInterface500 **open_midi_interface(
    IOUSBDeviceInterface500 **device)
{
    IOUSBFindInterfaceRequest request = {
        kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare
    };
    io_iterator_t iterator = 0;
    IOUSBInterfaceInterface500 **answer = NULL;

    if ((*device)->CreateInterfaceIterator(device, &request, &iterator))
        return NULL;

    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        IOCFPlugInInterface **plugin = NULL;
        IOUSBInterfaceInterface500 **candidate = NULL;
        SInt32 score = 0;

        if (IOCreatePlugInInterfaceForService(service,
                                              kIOUSBInterfaceUserClientTypeID,
                                              kIOCFPlugInInterfaceID,
                                              &plugin, &score) == KERN_SUCCESS &&
            plugin) {
            (*plugin)->QueryInterface(
                plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),
                (LPVOID *)&candidate);
            (*plugin)->Release(plugin);
        }

        if (candidate) {
            UInt8 interface_number = 255;
            (*candidate)->GetInterfaceNumber(candidate, &interface_number);
            if (interface_number == ER_MIDI_IF) {
                answer = candidate;
                IOObjectRelease(service);
                break;
            }
            (*candidate)->Release(candidate);
        }
        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);
    return answer;
}

/* Modern macOS may allow an interface user client while declining the legacy
   whole-device IOUSBLib user client.  Open interface 2 directly in that case. */
static IOUSBInterfaceInterface500 **open_midi_interface_direct(void)
{
    SInt32 vid = ER_VID;
    SInt32 pid = ER_PID;
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBInterfaceClassName);
    if (!match)
        return NULL;
    CFNumberRef vendor = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
    CFNumberRef product = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
    if (!vendor || !product) {
        if (vendor) CFRelease(vendor);
        if (product) CFRelease(product);
        CFRelease(match);
        return NULL;
    }
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), vendor);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), product);
    CFRelease(vendor);
    CFRelease(product);

    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator) !=
        KERN_SUCCESS)
        return NULL;

    IOUSBInterfaceInterface500 **answer = NULL;
    io_service_t service = 0;
    while ((service = IOIteratorNext(iterator)) != 0) {
        IOCFPlugInInterface **plugin = NULL;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(
                service, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &plugin, &score) == KERN_SUCCESS &&
            plugin) {
            IOUSBInterfaceInterface500 **candidate = NULL;
            (*plugin)->QueryInterface(
                plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),
                (LPVOID *)&candidate);
            (*plugin)->Release(plugin);
            if (candidate) {
                UInt8 interface_number = 0xff;
                (*candidate)->GetInterfaceNumber(candidate, &interface_number);
                if (interface_number == ER_MIDI_IF) {
                    answer = candidate;
                    IOObjectRelease(service);
                    break;
                }
                (*candidate)->Release(candidate);
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return answer;
}

static UInt8 cin_length(UInt8 cin)
{
    static const UInt8 lengths[16] = {
        0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
    };
    return lengths[cin & 0x0f];
}

typedef struct {
    IOUSBInterfaceInterface500 **midi;
    UInt8 input_pipe;
    UInt8 *buffer;
    UInt32 capacity;
    IOReturn result;
    UInt32 transferred;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    int done;
} ERBoundedRead;

static void *read_worker(void *argument)
{
    ERBoundedRead *read = argument;
    UInt32 transferred = read->capacity;
    IOReturn result = (*read->midi)->ReadPipeTO(
        read->midi, read->input_pipe, read->buffer, &transferred,
        1000, 1000);

    pthread_mutex_lock(&read->lock);
    read->result = result;
    read->transferred = transferred;
    read->done = 1;
    pthread_cond_signal(&read->condition);
    pthread_mutex_unlock(&read->lock);
    return NULL;
}

static IOReturn read_pipe_bounded(IOUSBInterfaceInterface500 **midi,
                                  UInt8 input_pipe, UInt8 *buffer,
                                  UInt32 capacity, UInt32 *transferred)
{
    ERBoundedRead read = {
        midi, input_pipe, buffer, capacity, kIOReturnError, 0,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0
    };
    pthread_t thread;
    int thread_result = pthread_create(&thread, NULL, read_worker, &read);
    if (thread_result != 0)
        return kIOReturnNoResources;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&read.lock);
    while (!read.done) {
        int wait_result = pthread_cond_timedwait(&read.condition, &read.lock,
                                                 &deadline);
        if (wait_result == ETIMEDOUT)
            break;
    }
    int timed_out = !read.done;
    pthread_mutex_unlock(&read.lock);

    if (timed_out)
        (*midi)->AbortPipe(midi, input_pipe);
    pthread_join(thread, NULL);
    *transferred = read.transferred;
    IOReturn result = read.result;
    pthread_cond_destroy(&read.condition);
    pthread_mutex_destroy(&read.lock);
    return result;
}

static int read_sysex(IOUSBInterfaceInterface500 **midi, UInt8 input_pipe,
                      UInt8 *output, size_t capacity, size_t *output_length)
{
    int in_sysex = 0;
    size_t used = 0;

    /* The endpoint has 512-byte packets. Read one packet at a time so long
       responses can stream, while read_pipe_bounded can abort a final partial
       packet if the deprecated IOUSBLib timeout is ignored by modern macOS. */
    for (int attempt = 0; attempt < 4096; ++attempt) {
        UInt8 usb[512];
        UInt32 usb_length = sizeof(usb);
        IOReturn result = read_pipe_bounded(midi, input_pipe, usb,
                                            usb_length, &usb_length);
        if ((result == kIOReturnTimeout ||
             result == kIOUSBTransactionTimeout ||
             result == kIOReturnAborted) && usb_length == 0)
            break;
        if (result != kIOReturnSuccess && usb_length == 0) {
            fprintf(stderr, "MIDI read failed: 0x%x\n", result);
            return 0;
        }

        for (UInt32 offset = 0; offset + 3 < usb_length; offset += 4) {
            UInt8 cable = usb[offset] >> 4;
            UInt8 count = cin_length(usb[offset]);
            if (cable != 0)
                continue;

            for (UInt8 index = 0; index < count; ++index) {
                UInt8 byte = usb[offset + 1 + index];
                if (!in_sysex) {
                    if (byte != 0xf0)
                        continue;
                    in_sysex = 1;
                }
                if (used == capacity) {
                    fputs("SysEx response exceeds buffer\n", stderr);
                    return 0;
                }
                output[used++] = byte;
                if (byte == 0xf7) {
                    *output_length = used;
                    return 1;
                }
            }
        }
    }

    *output_length = used;
    if (in_sysex)
        fprintf(stderr, "incomplete SysEx response (%zu bytes)\n", used);
    else
        fputs("no SysEx response\n", stderr);
    return 0;
}

static int send_identity_request(IOUSBInterfaceInterface500 **midi,
                                 UInt8 output_pipe)
{
    const UInt8 request[8] = {
        0x04, 0xf0, 0x7e, 0x7f,
        0x07, 0x06, 0x01, 0xf7
    };
    return (*midi)->WritePipeTO(midi, output_pipe, (void *)request,
                                sizeof(request), 1000, 1000) ==
           kIOReturnSuccess;
}

static int send_get_request(IOUSBInterfaceInterface500 **midi,
                            UInt8 output_pipe, UInt8 device_id,
                            UInt8 object)
{
    /* Decoded MIDI: F0 13 0B <device-id> 01 <object> F7.
       Function 1 = Get; object 1 = edit-buffer patch data and object 7 =
       edit-buffer rig volume. */
    UInt8 request[12] = {
        0x04, 0xf0, 0x13, 0x0b,
        0x04, device_id, 0x01, object,
        0x05, 0xf7, 0x00, 0x00
    };
    return (*midi)->WritePipeTO(midi, output_pipe, (void *)request,
                                sizeof(request), 1000, 1000) ==
           kIOReturnSuccess;
}

static void print_hex(const UInt8 *bytes, size_t length, size_t limit)
{
    if (length > limit)
        length = limit;
    for (size_t index = 0; index < length; ++index) {
        if ((index % 16) == 0)
            printf("%04zx:", index);
        printf(" %02X", bytes[index]);
        if ((index % 16) == 15 || index + 1 == length)
            putchar('\n');
    }
}

static size_t unpack_7bit(const UInt8 *packed, size_t packed_length,
                          UInt8 *raw, size_t raw_capacity)
{
    size_t used = 0;
    for (size_t offset = 0; offset + 1 < packed_length; offset += 8) {
        size_t available = packed_length - offset;
        if (available > 8)
            available = 8;
        for (unsigned phase = 1; phase < available && phase <= 7; ++phase) {
            if (used == raw_capacity)
                return used;
            unsigned value = (unsigned)packed[offset + phase - 1] << phase;
            value |= (unsigned)packed[offset + phase] >> (7 - phase);
            raw[used++] = (UInt8)value;
        }
    }
    return used;
}

typedef struct {
    char key[5];
    int32_t value;
} ERRigSetting;

typedef struct {
    char id;
    size_t count;
    const UInt8 *pairs;
    size_t end_offset;
} ERRigSection;

typedef struct {
    int32_t code;
    const char *name;
} ERNamedCode;

static const ERNamedCode er_amp_names[] = {
    { 0, "59 Tweed Lux" }, { 1, "59 Tweed Bass" },
    { 2, "64 Black Panel Lux Vib" }, { 3, "64 Black Panel Lux Norm" },
    { 4, "66 AC Hi Boost" }, { 5, "67 Black Duo" },
    { 6, "69 Plexiglas 100W" }, { 7, "82 Lead 800 100W" },
    { 8, "85 M-2 Lead" }, { 9, "89 SL-100 Drive" },
    { 10, "89 SL-100 Crunch" }, { 11, "89 SL-100 Clean" },
    { 12, "92 Treadplate Modern" }, { 13, "92 Treadplate Vintage" },
    { 14, "DC Modern Overdrive" }, { 15, "DC Vintage Crunch" },
    { INT32_MIN, "59 Tweed Lux" }, { -1861152495, "59 Tweed Bass" },
    { -1574821342, "64 Black Panel Lux Vib" },
    { -1288490189, "64 Black Panel Lux Norm" },
    { -1002159036, "66 AC Hi Boost" }, { -715827883, "67 Black Duo" },
    { -429496730, "69 Plexiglas 100W" }, { -143165577, "82 Lead 800 100W" },
    { 143165576, "85 M-2 Lead" }, { 429496729, "89 SL-100 Drive" },
    { 715827882, "89 SL-100 Crunch" }, { 1002159035, "89 SL-100 Clean" },
    { 1095980628, "69 Blue Line Bass" }, { 1112957526, "64 Black Vib" },
    { 1112764530, "65 Black SR" }, { 1129136945, "DC Modern Clean" },
    { 1129726769, "DC Vintage Clean" }, { 1128428337, "DC Bass" },
    { 1128940365, "DC Modern 800" }, { 1145261362, "DC Modern SOD" },
    { 1145263666, "DC Vintage OD" }, { 1247032373, "65 J45" },
    { 1288490188, "92 Treadplate Modern" }, { 1296315184, "93 MS 30" },
    { 1345663095, "68 Plexiglas 50W" }, { 1349277298, "67 Plexiglas Vari" },
    { 1447258221, "65 Black Mini" }, { 1481917250, "97 RB-01b Blue" },
    { 1481917255, "97 RB-01b Green" }, { 1481917266, "97 RB-01b Red" },
    { 1574821341, "92 Treadplate Vintage" },
    { 1861152494, "DC Modern Overdrive" }, { INT32_MAX, "DC Vintage Crunch" }
};

static const char *const er_cab_names[] = {
    "1x12 Blackpanel Lux", "1x12 Tweed Lux", "2x12 AC Blue",
    "2x12 Blackpanel Duo", "4x10 Tweed Bass", "4x12 Classic 30",
    "4x12 Green 25W", "(reserved cabinet 7)", "1x8 Custom",
    "1x15 Open Back", "2x12 B30", "2x12 Silver Cone",
    "4x10 Black SR", "4x12 65W", "4x12 Green 20W", "8x10 Blue Line"
};

static const char *const er_mic_names[] = {
    "Dyn 7", "Dyn 57", "Dyn 409", "Dyn 421",
    "Cond 67", "Cond 87", "Cond 414", "Ribbon 121"
};

static int32_t read_le32(const UInt8 *bytes)
{
    uint32_t value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                     ((uint32_t)bytes[2] << 16) |
                     ((uint32_t)bytes[3] << 24);
    return (int32_t)value;
}

static void read_quad_key(const UInt8 *bytes, char key[5])
{
    key[0] = (char)bytes[3];
    key[1] = (char)bytes[2];
    key[2] = (char)bytes[1];
    key[3] = (char)bytes[0];
    key[4] = '\0';
}

static int parse_rig_section(const UInt8 *raw, size_t raw_length,
                             size_t offset, ERRigSection *section)
{
    if (offset + 4 > raw_length)
        return 0;
    size_t byte_size = (size_t)raw[offset] | ((size_t)raw[offset + 1] << 8);
    if ((byte_size % 8) != 0 || offset + 4 + byte_size > raw_length)
        return 0;
    section->id = (char)raw[offset + 3];
    section->count = byte_size / 8;
    section->pairs = raw + offset + 4;
    section->end_offset = offset + 4 + byte_size;
    return 1;
}

static ERRigSetting rig_setting_at(const ERRigSection *section, size_t index)
{
    ERRigSetting setting;
    const UInt8 *pair = section->pairs + index * 8;
    read_quad_key(pair, setting.key);
    setting.value = read_le32(pair + 4);
    return setting;
}

static int find_rig_setting(const ERRigSection *section, const char *key,
                            int32_t *value)
{
    for (size_t index = 0; index < section->count; ++index) {
        ERRigSetting setting = rig_setting_at(section, index);
        if (memcmp(setting.key, key, 4) == 0) {
            *value = setting.value;
            return 1;
        }
    }
    return 0;
}

static const char *named_code(const ERNamedCode *codes, size_t count,
                              int32_t value)
{
    for (size_t index = 0; index < count; ++index)
        if (codes[index].code == value)
            return codes[index].name;
    return NULL;
}

static const char *cabinet_name(int32_t value)
{
    if (value >= 0 &&
        (size_t)value < sizeof(er_cab_names) / sizeof(er_cab_names[0]))
        return er_cab_names[value];
    switch ((uint32_t)value) {
    case 0x4A53584D: return "2x12 Silver Cone"; /* JSXM */
    case 0x4A617A7A: return "1x15 Open Back";   /* Jazz */
    case 0x32783330: return "2x12 B30";         /* 2x30 */
    case 0x34435453: return "4x10 Black SR";    /* 4CTS */
    case 0x34783230: return "4x12 Green 20W";   /* 4x20 */
    case 0x34783635: return "4x12 65W";         /* 4x65 */
    case 0x38535654: return "8x10 Blue Line";   /* 8SVT */
    default: return NULL;
    }
}

static const char *effect_name(int32_t effect_id)
{
    switch (effect_id) {
    case 12: return "Amp/Cab";
    case 38: case 72: return "Volume Pedal";
    case 29: return "Tri-Knob Fuzz";
    case 30: return "Black Op Distortion";
    case 31: return "Green JRC Overdrive";
    case 87: return "White Boost";
    case 91: return "DC Distortion";
    case 36: return "Shine Wah";
    case 55: return "Black Wah";
    case 16: case 17: case 56: case 57: case 73: case 14: case 15:
        return "FX Loop";
    case 32: return "Gray Compressor";
    case 85: case 86: return "Dyn3 Compressor";
    case 33: case 50: return "Graphic EQ";
    case 78: case 79: return "Parametric EQ";
    case 34: case 71: return "Orange Phaser";
    case 35: case 46: return "Vibe Phaser";
    case 37: case 47: return "Blackpanel Spring Reverb";
    case 51: case 52: case 53: return "Eleven SR";
    case 27: case 48: return "BBD Delay";
    case 28: case 49: return "Tape Echo";
    case 80: case 81: case 82: return "Dyn Delay";
    case 11: case 39: case 40: return "Chorus/Vibrato";
    case 88: case 89: case 90: return "Multi Chorus";
    case 69: case 70: return "Flanger";
    case 75: case 76: case 77: return "Roto Speaker";
    default: return "Unknown effect";
    }
}

static const char *category_name(int32_t category)
{
    static const char *const names[] = {
        "Amp/Cab", "FX Loop", "Volume", "Wah", "Modulation", "Reverb",
        "Delay", "Distortion", "FX1", "FX2", "Input", "Type 0B",
        "Type 0C", "Type 0D", "Type 0E", "Type 0F", "Globals"
    };
    if (category >= 0 && (size_t)category < sizeof(names) / sizeof(names[0]))
        return names[category];
    return "Unknown category";
}

static const char *global_parameter_name(const char *key)
{
    if (!memcmp(key, "RVol", 4)) return "Rig Volume";
    if (!memcmp(key, "RMno", 4)) return "Mono/Stereo";
    if (!memcmp(key, "Tmpo", 4)) return "Tempo";
    if (!memcmp(key, "PIGI", 4)) return "True-Z";
    if (!memcmp(key, "ExpT", 4)) return "Expression Pedal Target";
    if (!memcmp(key, "WorB", 4)) return "Rig Input";
    if (!memcmp(key, "GlSF", 4)) return "Global Signal Flow";
    if (!memcmp(key, "Msyc", 4)) return "MIDI Sync";
    if (!memcmp(key, "RslL", 4)) return "Rig Output Level";
    if (!memcmp(key, "FXc1", 4)) return "Multi-FX Control 1";
    if (!memcmp(key, "FXc2", 4)) return "Multi-FX Control 2";
    if (!memcmp(key, "FXc3", 4)) return "Multi-FX Control 3";
    if (!memcmp(key, "FXc4", 4)) return "Multi-FX Control 4";
    if (!memcmp(key, "Vol1", 4)) return "Volume Pedal Heel";
    if (!memcmp(key, "Vol2", 4)) return "Volume Pedal Toe";
    return key;
}

static const char *parameter_name(int32_t effect_id, int32_t amp_code,
                                  const char *key)
{
    if (!memcmp(key, "bypa", 4)) return "Effect State";
    if (effect_id == 12) {
        if (!memcmp(key, "sld1", 4)) return "Amp/Cab Internal Control";
        if (!memcmp(key, "sld2", 4)) return "Output";
        if (!memcmp(key, "sld3", 4)) return "Noise Gate Threshold";
        if (!memcmp(key, "sld4", 4)) return "Noise Gate Release";
        if (!memcmp(key, "sld5", 4)) return "Amp Bypass";
        if (!memcmp(key, "sld6", 4)) return "Amp Model";
        if (!memcmp(key, "sldJ", 4)) return "Cabinet Bypass";
        if (!memcmp(key, "sldK", 4)) return "Cabinet Model";
        if (!memcmp(key, "sldL", 4)) return "Microphone Model";
        if (!memcmp(key, "sldM", 4)) return "Microphone Axis";
        if (!memcmp(key, "sldN", 4)) return "Speaker Breakup";
        if (!memcmp(key, "Sync", 4)) return "Tremolo Sync";
        if (!memcmp(key, "TrOn", 4)) return "Tremolo On/Off";
        if (amp_code == 1481917250 || amp_code == 1481917255 ||
            amp_code == 1481917266) {
            if (!memcmp(key, "sldD", 4)) return "Presence";
            if (!memcmp(key, "sld9", 4)) return "Volume";
            if (!memcmp(key, "sldA", 4)) return "Treble";
            if (!memcmp(key, "sldB", 4)) return "Middle";
            if (!memcmp(key, "sldC", 4)) return "Bass";
            if (!memcmp(key, "sld7", 4)) return "Gain";
            if (!memcmp(key, "sld8", 4)) return "Boost";
            if (!memcmp(key, "sldE", 4)) return "Bright";
        }
        if (!memcmp(key, "sld7", 4)) return "Gain 1";
        if (!memcmp(key, "sld8", 4)) return "Gain 2";
        if (!memcmp(key, "sld9", 4)) return "Master";
        if (!memcmp(key, "sldA", 4)) return "Bass";
        if (!memcmp(key, "sldB", 4)) return "Middle";
        if (!memcmp(key, "sldC", 4)) return "Treble";
        if (!memcmp(key, "sldD", 4)) return "Presence";
        if (!memcmp(key, "sldE", 4)) return "Bright";
        if (!memcmp(key, "sldF", 4)) return "Tremolo Speed";
        if (!memcmp(key, "sldG", 4)) return "Tremolo Depth";
        if (!memcmp(key, "sldO", 4)) return "Additional Amp Control";
    }
    if (effect_id == 38 || effect_id == 72) {
        if (!memcmp(key, "Vol ", 4)) return "Position";
        if (!memcmp(key, "Min ", 4)) return "Minimum Volume";
        if (!memcmp(key, "Tapr", 4)) return "Taper (Linear/Log)";
    }
    if (effect_id == 36 || effect_id == 55) {
        if (!memcmp(key, "Filt", 4)) return "Position";
        if (!memcmp(key, "VxCr", 4)) return "Vox/Cry Character";
    }
    if (effect_id == 29 || effect_id == 30 || effect_id == 31 ||
        effect_id == 87 || effect_id == 91) {
        if (!memcmp(key, "Driv", 4)) {
            if (effect_id == 29) return "Sustain";
            if (effect_id == 30 || effect_id == 91) return "Distortion";
            if (effect_id == 31) return "Overdrive";
            return "Gain";
        }
        if (!memcmp(key, "Tone", 4)) return effect_id == 30 ? "Cut" : "Tone";
        if (!memcmp(key, "Levl", 4)) return effect_id == 31 ? "Level" : "Volume";
        if (!memcmp(key, "Treb", 4)) return "Treble";
        if (!memcmp(key, "Bass", 4)) return "Bass";
    }
    if (effect_id == 78 || effect_id == 79) {
        if (!memcmp(key, "B1Gn", 4)) return "Low Gain";
        if (!memcmp(key, "B1Fr", 4)) return "Low Frequency";
        if (!memcmp(key, "B1Q ", 4) || !memcmp(key, "B1Q", 3)) return "Low Q";
        if (!memcmp(key, "B1Ty", 4)) return "Low Type";
        if (!memcmp(key, "B2Gn", 4)) return "Low-Mid Gain";
        if (!memcmp(key, "B2Fr", 4)) return "Low-Mid Frequency";
        if (!memcmp(key, "B2Q ", 4) || !memcmp(key, "B2Q", 3)) return "Low-Mid Q";
        if (!memcmp(key, "B3Gn", 4)) return "High-Mid Gain";
        if (!memcmp(key, "B3Fr", 4)) return "High-Mid Frequency";
        if (!memcmp(key, "B3Q ", 4) || !memcmp(key, "B3Q", 3)) return "High-Mid Q";
        if (!memcmp(key, "B4Gn", 4)) return "High Gain";
        if (!memcmp(key, "B4Fr", 4)) return "High Frequency";
        if (!memcmp(key, "B4Q ", 4) || !memcmp(key, "B4Q", 3)) return "High Q";
        if (!memcmp(key, "B4Ty", 4)) return "High Type";
        if (!memcmp(key, "Out ", 4)) return "Output";
    }
    if (effect_id == 85 || effect_id == 86) {
        if (!memcmp(key, "sldC", 4)) return "Threshold";
        if (!memcmp(key, "sld1", 4)) return "Attack";
        if (!memcmp(key, "sld3", 4)) return "Release";
        if (!memcmp(key, "sld2", 4)) return "Ratio";
        if (!memcmp(key, "sldG", 4)) return "Knee";
        if (!memcmp(key, "sldD", 4)) return "Gain";
    }
    if (effect_id == 88 || effect_id == 89 || effect_id == 90) {
        if (!memcmp(key, "Rate", 4)) return "Rate";
        if (!memcmp(key, "Sync", 4)) return "Sync";
        if (!memcmp(key, "Dpth", 4)) return "Depth";
        if (!memcmp(key, "Mix ", 4)) return "Mix";
        if (!memcmp(key, "PDly", 4)) return "Pre-Delay";
        if (!memcmp(key, "Voic", 4)) return "Voices";
        if (!memcmp(key, "LoCt", 4)) return "Low Cut";
        if (!memcmp(key, "Wdth", 4)) return "Width";
        if (!memcmp(key, "Wave", 4)) return "Waveform";
    }
    if (effect_id == 80 || effect_id == 81 || effect_id == 82) {
        if (!memcmp(key, "Dly ", 4)) return "Delay";
        if (!memcmp(key, "Fdbk", 4)) return "Feedback";
        if (!memcmp(key, "DMix", 4)) return "Mix";
        if (!memcmp(key, "Sync", 4)) return "Sync";
        if (!memcmp(key, "Mode", 4)) return "Mode";
        if (!memcmp(key, "Rtio", 4)) return "Ratio";
        if (!memcmp(key, "Wdth", 4)) return "Width";
        if (!memcmp(key, "HiCt", 4)) return "High Cut";
        if (!memcmp(key, "LoCt", 4)) return "Low Cut";
        if (!memcmp(key, "EnRt", 4)) return "Envelope Rate";
        if (!memcmp(key, "EnFb", 4)) return "Envelope Feedback";
        if (!memcmp(key, "EnMx", 4)) return "Envelope Mix";
    }
    if (effect_id == 51 || effect_id == 52 || effect_id == 53) {
        if (!memcmp(key, "Dcay", 4)) return "Decay";
        if (!memcmp(key, "Tone", 4)) return "Tone";
        if (!memcmp(key, "RMix", 4)) return "Mix";
        if (!memcmp(key, "Type", 4)) return "Reverb Type";
        if (!memcmp(key, "PDly", 4)) return "Pre-Delay";
    }
    if (effect_id == 14 || effect_id == 15 || effect_id == 16 ||
        effect_id == 17 || effect_id == 56 || effect_id == 57 || effect_id == 73) {
        if (!memcmp(key, "send", 4)) return "Send";
        if (!memcmp(key, "rtrn", 4)) return "Return";
        if (!memcmp(key, "wetp", 4)) return "Mix";
    }
    if (effect_id == 11 || effect_id == 39 || effect_id == 40) {
        if (!memcmp(key, "ChIn", 4)) return "Chorus";
        if (!memcmp(key, "VbRt", 4)) return "Vibrato Rate";
        if (!memcmp(key, "VbDp", 4)) return "Vibrato Depth";
        if (!memcmp(key, "Mode", 4)) return "Chorus/Vibrato";
        if (!memcmp(key, "Sync", 4)) return "Sync";
    }
    if (effect_id == 27 || effect_id == 48) {
        if (!memcmp(key, "Dly ", 4)) return "Delay";
        if (!memcmp(key, "Blnd", 4)) return "Mix";
        if (!memcmp(key, "Fdbk", 4)) return "Feedback";
        if (!memcmp(key, "InLv", 4)) return "Input Level";
        if (!memcmp(key, "ChVb", 4)) return "Modulation Mode";
        if (!memcmp(key, "Dpth", 4)) return "Modulation Depth";
        if (!memcmp(key, "4X  ", 4)) return "Expanded Delay";
        if (!memcmp(key, "Nois", 4)) return "Noise";
        if (!memcmp(key, "Sync", 4)) return "Sync";
    }
    if (effect_id == 28 || effect_id == 49) {
        if (!memcmp(key, "EDly", 4)) return "Delay";
        if (!memcmp(key, "Sust", 4)) return "Feedback";
        if (!memcmp(key, "Vol ", 4)) return "Mix";
        if (!memcmp(key, "Rec ", 4)) return "Record Level";
        if (!memcmp(key, "Tilt", 4)) return "Head";
        if (!memcmp(key, "Wow ", 4)) return "Wow";
        if (!memcmp(key, "4X  ", 4)) return "Expanded Delay";
        if (!memcmp(key, "Hiss", 4)) return "Hiss";
        if (!memcmp(key, "Sync", 4)) return "Sync";
    }
    if (effect_id == 32) {
        if (!memcmp(key, "Sust", 4)) return "Sustain";
        if (!memcmp(key, "Levl", 4)) return "Level";
    }
    if (effect_id == 33 || effect_id == 50) {
        if (!memcmp(key, "LwSh", 4)) return "100 Hz";
        if (!memcmp(key, "LMGn", 4)) return "370 Hz";
        if (!memcmp(key, "MGn ", 4)) return "800 Hz";
        if (!memcmp(key, "HMGn", 4)) return "2 kHz";
        if (!memcmp(key, "HiSh", 4)) return "3.25 kHz";
        if (!memcmp(key, "Out ", 4)) return "Output";
    }
    if (effect_id == 34 || effect_id == 71) {
        if (!memcmp(key, "Sped", 4)) return "Speed";
        if (!memcmp(key, "Sync", 4)) return "Sync";
    }
    if (effect_id == 35 || effect_id == 46) {
        if (!memcmp(key, "Levl", 4)) return "Volume";
        if (!memcmp(key, "Intn", 4)) return "Depth";
        if (!memcmp(key, "Sped", 4)) return "Speed";
        if (!memcmp(key, "Chor", 4)) return "Chorus/Vibrato";
        if (!memcmp(key, "Sync", 4)) return "Sync";
    }
    if (effect_id == 37 || effect_id == 47) {
        if (!memcmp(key, "Dcay", 4)) return "Decay";
        if (!memcmp(key, "Tone", 4)) return "Tone";
        if (!memcmp(key, "RMix", 4)) return "Mix";
    }
    if (effect_id == 69 || effect_id == 70) {
        if (!memcmp(key, "PDly", 4)) return "Pre-Delay";
        if (!memcmp(key, "Dpth", 4)) return "Depth";
        if (!memcmp(key, "Sped", 4)) return "Rate";
        if (!memcmp(key, "Fdbk", 4)) return "Feedback";
        if (!memcmp(key, "Sync", 4)) return "Sync";
    }
    if (effect_id == 75 || effect_id == 76 || effect_id == 77) {
        if (!memcmp(key, "Sped", 4)) return "Speed";
        if (!memcmp(key, "Blnc", 4)) return "Rotor Balance";
        if (!memcmp(key, "Type", 4)) return "Speaker Type";
    }
    return key;
}

static double normalized_value(int32_t value)
{
    uint64_t position = (uint64_t)((int64_t)value - INT32_MIN);
    return (double)position / UINT32_MAX;
}

static int is_ten_scale_setting(int32_t effect_id, const char *key)
{
    if (effect_id == 12)
        return !memcmp(key, "sld7", 4) || !memcmp(key, "sld8", 4) ||
               !memcmp(key, "sld9", 4) || !memcmp(key, "sldA", 4) ||
               !memcmp(key, "sldB", 4) || !memcmp(key, "sldC", 4) ||
               !memcmp(key, "sldD", 4) || !memcmp(key, "sldF", 4) ||
               !memcmp(key, "sldG", 4) || !memcmp(key, "sldN", 4) ||
               !memcmp(key, "sldO", 4);
    if (effect_id == 29 || effect_id == 30 || effect_id == 31 ||
        effect_id == 32 || effect_id == 34 || effect_id == 35 ||
        effect_id == 69 ||
        effect_id == 70 || effect_id == 71 || effect_id == 87 ||
        effect_id == 91)
        return memcmp(key, "bypa", 4) != 0 && memcmp(key, "Sync", 4) != 0 &&
               memcmp(key, "Chor", 4) != 0;
    if (effect_id == 11 || effect_id == 39 || effect_id == 40)
        return !memcmp(key, "ChIn", 4) || !memcmp(key, "VbRt", 4) ||
               !memcmp(key, "VbDp", 4);
    if (effect_id == 27 || effect_id == 48)
        return !memcmp(key, "InLv", 4) || !memcmp(key, "Dpth", 4);
    if (effect_id == 28 || effect_id == 49)
        return !memcmp(key, "Rec ", 4) || !memcmp(key, "Tilt", 4) ||
               !memcmp(key, "Wow ", 4);
    if (effect_id == 37 || effect_id == 47)
        return !memcmp(key, "Dcay", 4) || !memcmp(key, "Tone", 4);
    if (effect_id == 51 || effect_id == 52 || effect_id == 53)
        return !memcmp(key, "Dcay", 4) || !memcmp(key, "Tone", 4);
    return 0;
}

static int is_percent_setting(int32_t effect_id, const char *key)
{
    if ((effect_id == 14 || effect_id == 15 || effect_id == 16 ||
         effect_id == 17 || effect_id == 56 || effect_id == 57 ||
         effect_id == 73) && !memcmp(key, "wetp", 4)) return 1;
    if ((effect_id == 27 || effect_id == 48) &&
        (!memcmp(key, "Blnd", 4) || !memcmp(key, "Fdbk", 4))) return 1;
    if ((effect_id == 28 || effect_id == 49) &&
        (!memcmp(key, "Vol ", 4) || !memcmp(key, "Sust", 4))) return 1;
    if ((effect_id == 37 || effect_id == 47 || effect_id == 51 ||
         effect_id == 52 || effect_id == 53) && !memcmp(key, "RMix", 4)) return 1;
    if ((effect_id == 38 || effect_id == 72) &&
        (!memcmp(key, "Vol ", 4) || !memcmp(key, "Min ", 4))) return 1;
    if ((effect_id == 36 || effect_id == 55) && !memcmp(key, "Filt", 4)) return 1;
    if ((effect_id == 75 || effect_id == 76 || effect_id == 77) &&
        !memcmp(key, "Blnc", 4)) return 1;
    return 0;
}

/* Physical-unit metadata. "Verified" means that the unit and endpoints are
   stated by Avid documentation or printed in the installed Eleven Rack UI.
   "Family" means that the unit is verified for the source AIR effect family,
   but the Eleven Rack endpoint still needs a hardware value-string check. */
static const char * __attribute__((unused))
parameter_unit(int32_t effect_id, const char *key)
{
    if (!memcmp(key, "RVol", 4)) return "dB";
    if (!memcmp(key, "Tmpo", 4)) return "BPM";
    if (is_ten_scale_setting(effect_id, key)) return "0-10";
    if (is_percent_setting(effect_id, key)) return "%";
    if ((effect_id == 33 || effect_id == 50) &&
        (memcmp(key, "bypa", 4) != 0)) return "dB";
    if (effect_id == 85 || effect_id == 86) {
        if (!memcmp(key, "sldC", 4) || !memcmp(key, "sldG", 4) ||
            !memcmp(key, "sldD", 4)) return "dB";
        if (!memcmp(key, "sld1", 4) || !memcmp(key, "sld3", 4)) return "ms";
        if (!memcmp(key, "sld2", 4)) return "ratio";
    }
    if (effect_id == 80 || effect_id == 81 || effect_id == 82) {
        if (!memcmp(key, "Dly ", 4)) return "ms";
        if (!memcmp(key, "HiCt", 4) || !memcmp(key, "LoCt", 4)) return "Hz";
        if (!memcmp(key, "Rtio", 4)) return "L:R";
        if (!memcmp(key, "Fdbk", 4) || !memcmp(key, "DMix", 4) ||
            !memcmp(key, "Wdth", 4) || !memcmp(key, "EnRt", 4) ||
            !memcmp(key, "EnFb", 4) || !memcmp(key, "EnMx", 4)) return "%";
    }
    if (effect_id == 88 || effect_id == 89 || effect_id == 90) {
        if (!memcmp(key, "Rate", 4)) return "Hz";
        if (!memcmp(key, "Dpth", 4) || !memcmp(key, "PDly", 4)) return "ms";
        if (!memcmp(key, "Mix ", 4) || !memcmp(key, "Wdth", 4)) return "%";
        if (!memcmp(key, "LoCt", 4)) return "Hz";
    }
    if ((effect_id == 51 || effect_id == 52 || effect_id == 53) &&
        !memcmp(key, "PDly", 4)) return "ms";
    return "";
}

static const char *parameter_range(int32_t effect_id, const char *key)
{
    if (!memcmp(key, "RVol", 4)) return "-24.0..0.0";
    if (!memcmp(key, "Tmpo", 4)) return "hardware tempo range";
    if (is_ten_scale_setting(effect_id, key))
        return "0.0..10.0";
    if (is_percent_setting(effect_id, key)) return "0..100";
    if ((effect_id == 33 || effect_id == 50) &&
        (!memcmp(key, "LwSh", 4) || !memcmp(key, "HiSh", 4))) return "-12.0..12.0";
    if ((effect_id == 33 || effect_id == 50) &&
        (!memcmp(key, "LMGn", 4) || !memcmp(key, "MGn ", 4) ||
         !memcmp(key, "HMGn", 4))) return "-18.0..18.0";
    if ((effect_id == 33 || effect_id == 50) && !memcmp(key, "Out ", 4)) return "-20.0..6.0";
    if (effect_id == 85 || effect_id == 86) {
        if (!memcmp(key, "sldC", 4)) return "-60.0..0.0";
        if (!memcmp(key, "sld1", 4)) return "0.01..300";
        if (!memcmp(key, "sld3", 4)) return "5..4000";
        if (!memcmp(key, "sld2", 4)) return "1:1..100:1";
        if (!memcmp(key, "sldG", 4)) return "0..30";
        if (!memcmp(key, "sldD", 4)) return "0..40";
    }
    if (effect_id == 80 || effect_id == 81 || effect_id == 82) {
        if (!memcmp(key, "Dly ", 4)) return "1..4000";
        if (!memcmp(key, "Fdbk", 4) || !memcmp(key, "DMix", 4) ||
            !memcmp(key, "Wdth", 4) || !memcmp(key, "EnRt", 4)) return "0..100";
        if (!memcmp(key, "EnFb", 4) || !memcmp(key, "EnMx", 4)) return "-100..100";
        if (!memcmp(key, "Rtio", 4)) return "50:100..100:50";
    }
    if (effect_id == 88 || effect_id == 89 || effect_id == 90) {
        if (!memcmp(key, "Rate", 4)) return "0.01..10.0";
        if (!memcmp(key, "Dpth", 4) || !memcmp(key, "PDly", 4)) return "0..24";
        if (!memcmp(key, "Mix ", 4) || !memcmp(key, "Wdth", 4)) return "0..100";
        if (!memcmp(key, "LoCt", 4)) return "20..1000";
    }
    return "";
}

static int is_discrete_setting(int32_t effect_id, const char *key);

static const char * __attribute__((unused))
parameter_curve(int32_t effect_id, const char *key)
{
    if (is_discrete_setting(effect_id, key)) return "enum";
    if ((effect_id == 85 || effect_id == 86) &&
        (!memcmp(key, "sld1", 4) || !memcmp(key, "sld2", 4) ||
         !memcmp(key, "sld3", 4))) return "log";
    if ((effect_id == 88 || effect_id == 89 || effect_id == 90) &&
        (!memcmp(key, "Rate", 4) || !memcmp(key, "LoCt", 4))) return "log";
    return parameter_range(effect_id, key)[0] ? "linear" : "unverified";
}

static const char *rig_input_name(int32_t value)
{
    switch (value) {
    case 60: return "Guitar"; case 61: return "Mic";
    case 20: return "Line L"; case 21: return "Line R";
    case 63: return "Line L + R"; case 22: return "Digital L";
    case 23: return "Digital R"; case 64: return "Digital L + R";
    case 18: return "Re-Amp"; default: return NULL;
    }
}

static const char *true_z_name(int32_t value)
{
    static const char *const names[] = {
        "1 MOhm", "1 MOhm + Cap", "230 kOhm", "230 kOhm + Cap",
        "90 kOhm", "90 kOhm + Cap", "70 kOhm", "70 kOhm + Cap",
        "32 kOhm", "32 kOhm + Cap", "22 kOhm", "22 kOhm + Cap"
    };
    if (value >= 0 && (size_t)value < sizeof(names) / sizeof(names[0]))
        return names[value];
    if (value == 125 || value == 126)
        return "Auto";
    return NULL;
}

static const char *sync_name(int32_t value)
{
    static const char *const names[] = {
        "Off", "Whole Note", "Dotted Half", "Half Note", "Half Triplet",
        "Dotted Quarter", "Quarter Note", "Quarter Triplet", "Dotted 8th",
        "8th Note", "8th Triplet", "Dotted 16th", "16th Note",
        "16th Triplet"
    };
    if (value >= 0 && (size_t)value < sizeof(names) / sizeof(names[0]))
        return names[value];
    return NULL;
}

static int is_discrete_setting(int32_t effect_id, const char *key)
{
    if (!memcmp(key, "bypa", 4) || !memcmp(key, "RMno", 4) ||
        !memcmp(key, "PIGI", 4) || !memcmp(key, "ExpT", 4) ||
        !memcmp(key, "FXc1", 4) || !memcmp(key, "FXc2", 4) ||
        !memcmp(key, "FXc3", 4) || !memcmp(key, "FXc4", 4) ||
        !memcmp(key, "GlSF", 4) || !memcmp(key, "Msyc", 4) ||
        !memcmp(key, "WorB", 4) || !memcmp(key, "WorC", 4) ||
        !memcmp(key, "WorD", 4) || !memcmp(key, "WorE", 4) ||
        !memcmp(key, "WorF", 4) || !memcmp(key, "WorG", 4) ||
        !memcmp(key, "WorH", 4) || !memcmp(key, "WorI", 4) ||
        !memcmp(key, "WorJ", 4) || !memcmp(key, "WorK", 4) ||
        !memcmp(key, "WorL", 4) || !memcmp(key, "WstB", 4) ||
        !memcmp(key, "WstC", 4) || !memcmp(key, "WstD", 4) ||
        !memcmp(key, "WstE", 4) || !memcmp(key, "WstF", 4) ||
        !memcmp(key, "WstG", 4) || !memcmp(key, "WstH", 4) ||
        !memcmp(key, "WstI", 4) || !memcmp(key, "WstJ", 4) ||
        !memcmp(key, "WstK", 4) || !memcmp(key, "WstL", 4) ||
        !memcmp(key, "WoLS", 4) || !memcmp(key, "Tapr", 4) ||
        !memcmp(key, "4X  ", 4) || !memcmp(key, "ChVb", 4) ||
        !memcmp(key, "Chor", 4) || !memcmp(key, "VxCr", 4) ||
        !memcmp(key, "Nois", 4) || !memcmp(key, "Hiss", 4) ||
        !memcmp(key, "Sped", 4) ||
        !memcmp(key, "Sync", 4) || !memcmp(key, "Mode", 4) ||
        !memcmp(key, "Type", 4) || !memcmp(key, "Voic", 4) ||
        !memcmp(key, "Wave", 4) || !memcmp(key, "B1Ty", 4) ||
        !memcmp(key, "B4Ty", 4))
        return 1;
    if (effect_id == 12 && (!memcmp(key, "sld1", 4) ||
                            !memcmp(key, "sld5", 4) ||
                            !memcmp(key, "sld6", 4) ||
                            !memcmp(key, "sldE", 4) ||
                            !memcmp(key, "TrOn", 4) ||
                            !memcmp(key, "sldJ", 4) ||
                            !memcmp(key, "sldK", 4) ||
                            !memcmp(key, "sldL", 4) ||
                            !memcmp(key, "sldM", 4)))
        return 1;
    return 0;
}

static void format_setting_value(char *output, size_t capacity,
                                 int32_t effect_id, const char *key,
                                 int32_t value)
{
    const char *name = NULL;
    if (!memcmp(key, "RVol", 4)) {
        uint64_t position = (uint64_t)((int64_t)value - INT32_MIN);
        double db = -24.0 + 24.0 * (double)position / UINT32_MAX;
        snprintf(output, capacity, "%.1f dB", db);
        return;
    }
    if (!memcmp(key, "Tmpo", 4)) {
        snprintf(output, capacity, "%.1f BPM", (double)value / 10000.0);
        return;
    }
    if (!memcmp(key, "WorB", 4))
        name = rig_input_name(value);
    else if (!memcmp(key, "PIGI", 4))
        name = true_z_name(value);
    else if (!memcmp(key, "ExpT", 4) && value >= 0 && value <= 4) {
        static const char *const targets[] = {
            "None", "Multiple FX", "Rig Volume", "Volume", "Wah"
        };
        name = targets[value];
    } else if (!memcmp(key, "Sync", 4))
        name = sync_name(value);
    else if (effect_id == 12 && !memcmp(key, "sld6", 4))
        name = named_code(er_amp_names,
                          sizeof(er_amp_names) / sizeof(er_amp_names[0]), value);
    else if (effect_id == 12 && !memcmp(key, "sldK", 4))
        name = cabinet_name(value);
    else if (effect_id == 12 && !memcmp(key, "sldL", 4) && value >= 0 &&
             (size_t)value < sizeof(er_mic_names) / sizeof(er_mic_names[0]))
        name = er_mic_names[value];
    else if (effect_id == 12 && !memcmp(key, "sldM", 4))
        name = value ? "On Axis" : "Off Axis";
    else if ((effect_id == 80 || effect_id == 81 || effect_id == 82) &&
             !memcmp(key, "Mode", 4) && value >= 0 && value <= 3) {
        static const char *const modes[] = { "Mono", "Stereo", "Cross", "Pong" };
        name = modes[value];
    } else if ((effect_id == 11 || effect_id == 39 || effect_id == 40) &&
               !memcmp(key, "Mode", 4) && value >= 0 && value <= 1)
        name = value ? "Vibrato" : "Chorus";
    else if ((effect_id == 27 || effect_id == 48) &&
             !memcmp(key, "ChVb", 4) && value >= 0 && value <= 1)
        name = value ? "Vibrato" : "Chorus";
    else if ((effect_id == 27 || effect_id == 28 || effect_id == 48 ||
              effect_id == 49) &&
             (!memcmp(key, "4X  ", 4) || !memcmp(key, "Nois", 4) ||
              !memcmp(key, "Hiss", 4)) && value >= 0 && value <= 1)
        name = value ? "On" : "Off";
    else if ((effect_id == 75 || effect_id == 76 || effect_id == 77) &&
             !memcmp(key, "Sped", 4) && value >= 0 && value <= 2) {
        static const char *const speeds[] = { "Slow", "Brake", "Fast" };
        name = speeds[value];
    } else if ((effect_id == 75 || effect_id == 76 || effect_id == 77) &&
               !memcmp(key, "Type", 4) && value >= 0 && value <= 8) {
        static const char *const types[] = {
            "120", "122", "21H", "Foam", "Drum", "Rover", "Memphis",
            "Wolf", "Watery"
        };
        name = types[value];
    } else if (!memcmp(key, "bypa", 4))
        name = value ? "Disabled (bypassed)" : "Enabled";
    else if ((effect_id == 38 || effect_id == 72) && !memcmp(key, "Tapr", 4))
        name = value ? "Log" : "Linear";
    else if ((effect_id == 88 || effect_id == 89 || effect_id == 90) &&
             !memcmp(key, "Wave", 4))
        name = value ? "Sine" : "Triangle";
    else if ((effect_id == 78 || effect_id == 79) &&
             (!memcmp(key, "B1Ty", 4) || !memcmp(key, "B4Ty", 4)) &&
             value >= 0 && value <= 5) {
        static const char *const low_types[] = {
            "Shelf", "Peak", "HP6", "HP12", "HP24", "Notch"
        };
        static const char *const high_types[] = {
            "Shelf", "Peak", "LP6", "LP12", "LP24", "Notch"
        };
        name = !memcmp(key, "B1Ty", 4) ? low_types[value] : high_types[value];
    } else if ((effect_id == 51 || effect_id == 52 || effect_id == 53) &&
               !memcmp(key, "Type", 4) && value >= 0 && value <= 24) {
        static const char *const reverb_types[] = {
            "Echo Room", "Studio", "Small Room", "Jazz Club", "Small Club",
            "Garage", "Medium Room", "Tiled Room", "Wood Room", "Small Theater",
            "Medium Theater", "Large Theater", "Rich Hall", "Concert Hall",
            "Bright Hall", "Church", "Cathedral", "Arena", "Small Plate",
            "Medium Plate", "Large Plate", "Canyon", "Supa Long",
            "Early Reflect 1", "Early Reflect 2"
        };
        name = reverb_types[value];
    }
    else if (effect_id == 12 && (!memcmp(key, "sld5", 4) ||
                                 !memcmp(key, "sldJ", 4)))
        name = value ? "Bypassed" : "Not bypassed";
    else if (effect_id == 12 && (!memcmp(key, "sldE", 4) ||
                                 !memcmp(key, "TrOn", 4)))
        name = value ? "On" : "Off";

    if (name) {
        snprintf(output, capacity, "%s", name);
        return;
    }
    if (is_discrete_setting(effect_id, key)) {
        snprintf(output, capacity, "%d", value);
        return;
    }
    double x = normalized_value(value);
    if (is_ten_scale_setting(effect_id, key)) {
        snprintf(output, capacity, "%.1f", 10.0 * x);
        return;
    }
    if (is_percent_setting(effect_id, key)) {
        snprintf(output, capacity, "%.1f%%", 100.0 * x);
        return;
    }
    if (effect_id == 33 || effect_id == 50) {
        if (!memcmp(key, "LwSh", 4) || !memcmp(key, "HiSh", 4)) {
            snprintf(output, capacity, "%+.1f dB", -12.0 + 24.0 * x);
            return;
        }
        if (!memcmp(key, "LMGn", 4) || !memcmp(key, "MGn ", 4) ||
            !memcmp(key, "HMGn", 4)) {
            snprintf(output, capacity, "%+.1f dB", -18.0 + 36.0 * x);
            return;
        }
        if (!memcmp(key, "Out ", 4)) {
            snprintf(output, capacity, "%+.1f dB", -20.0 + 26.0 * x);
            return;
        }
    }
    if (effect_id == 85 || effect_id == 86) {
        if (!memcmp(key, "sldC", 4)) {
            snprintf(output, capacity, "%.1f dB", -60.0 + 60.0 * x);
            return;
        }
        if (!memcmp(key, "sld1", 4)) {
            double milliseconds = 0.01 * pow(30000.0, x);
            if (milliseconds < 1.0)
                snprintf(output, capacity, "%.0f us", milliseconds * 1000.0);
            else
                snprintf(output, capacity, "%.1f ms", milliseconds);
            return;
        }
        if (!memcmp(key, "sld3", 4)) {
            snprintf(output, capacity, "%.1f ms", 5.0 * pow(800.0, x));
            return;
        }
        if (!memcmp(key, "sld2", 4)) {
            snprintf(output, capacity, "%.3g:1", pow(100.0, x));
            return;
        }
        if (!memcmp(key, "sldG", 4)) {
            snprintf(output, capacity, "%.1f dB", 30.0 * x);
            return;
        }
        if (!memcmp(key, "sldD", 4)) {
            snprintf(output, capacity, "%+.1f dB", 40.0 * x);
            return;
        }
    }
    if (effect_id == 80 || effect_id == 81 || effect_id == 82) {
        if (!memcmp(key, "Dly ", 4)) {
            snprintf(output, capacity, "%.0f ms", 1.0 + 3999.0 * x);
            return;
        }
        if (!memcmp(key, "Rtio", 4)) {
            double left = x <= 0.5 ? 50.0 + 100.0 * x : 100.0;
            double right = x >= 0.5 ? 150.0 - 100.0 * x : 100.0;
            snprintf(output, capacity, "%.0f:%.0f", left, right);
            return;
        }
        if (!memcmp(key, "EnFb", 4) || !memcmp(key, "EnMx", 4)) {
            snprintf(output, capacity, "%+.1f%%", -100.0 + 200.0 * x);
            return;
        }
        if (!memcmp(key, "Fdbk", 4) || !memcmp(key, "DMix", 4) ||
            !memcmp(key, "Wdth", 4) || !memcmp(key, "EnRt", 4)) {
            snprintf(output, capacity, "%.1f%%", 100.0 * x);
            return;
        }
    }
    if (effect_id == 88 || effect_id == 89 || effect_id == 90) {
        if (!memcmp(key, "Rate", 4)) {
            snprintf(output, capacity, "%.2f Hz", 0.01 * pow(1000.0, x));
            return;
        }
        if (!memcmp(key, "Dpth", 4) || !memcmp(key, "PDly", 4)) {
            snprintf(output, capacity, "%.2f ms", 24.0 * x);
            return;
        }
        if (!memcmp(key, "Mix ", 4) || !memcmp(key, "Wdth", 4)) {
            snprintf(output, capacity, "%.1f%%", 100.0 * x);
            return;
        }
        if (!memcmp(key, "LoCt", 4)) {
            snprintf(output, capacity, "%.0f Hz", 20.0 * pow(50.0, x));
            return;
        }
    }
    snprintf(output, capacity, "%.2f%% normalized", 100.0 * x);
}

static void print_rig_summary(const UInt8 *sysex, size_t sysex_length)
{
    if (sysex_length < 8 || sysex[0] != 0xf0 || sysex[1] != 0x13 ||
        sysex[2] != 0x0b || sysex[4] != 0x12 || sysex[5] != 0x01 ||
        sysex[sysex_length - 1] != 0xf7)
        return;

    size_t packed_length = sysex_length - 7;
    size_t raw_capacity = (packed_length * 7) / 8 + 1;
    UInt8 *raw = malloc(raw_capacity);
    if (!raw)
        return;
    size_t raw_length = unpack_7bit(sysex + 6, packed_length,
                                    raw, raw_capacity);
    printf("decoded rig payload: %zu bytes\n", raw_length);

    /* Gtr_SettingsData begins with an eight-byte header followed by a
       28-byte, NUL-padded edit-buffer name. */
    if (raw_length < 36) {
        fputs("decoded rig payload is too short\n", stderr);
        free(raw);
        return;
    }

    char name[29];
    size_t name_length = 0;
    for (size_t word = 0; word < 28 && name_length < 28; word += 4) {
        for (int byte_index = 3; byte_index >= 0; --byte_index) {
            UInt8 byte = raw[8 + word + (size_t)byte_index];
            if (byte == 0 || byte < 0x20 || byte > 0x7e)
                goto name_complete;
            name[name_length++] = (char)byte;
        }
    }
name_complete:
    name[name_length] = '\0';
    printf("current rig name: %s\n", name_length ? name : "(unnamed)");

    ERRigSection toc;
    if (!parse_rig_section(raw, raw_length, 36, &toc) || toc.id != 'A') {
        fputs("could not decode rig table of contents\n", stderr);
        free(raw);
        return;
    }

    int32_t effect_ids[10] = { 0 };
    int32_t categories[10] = { 0 };
    puts("\nRig parameters:");
    for (size_t index = 0; index < toc.count; ++index) {
        ERRigSetting setting = rig_setting_at(&toc, index);
        if (!memcmp(setting.key, "Wor", 3) && setting.key[3] >= 'C' &&
            setting.key[3] <= 'L') {
            effect_ids[setting.key[3] - 'C'] = setting.value;
            continue;
        }
        if (!memcmp(setting.key, "Wst", 3) && setting.key[3] >= 'C' &&
            setting.key[3] <= 'L') {
            categories[setting.key[3] - 'C'] = setting.value;
            continue;
        }
        char display[96];
        format_setting_value(display, sizeof(display), -1, setting.key,
                             setting.value);
        printf("  %-25s [%s] = %-22s raw=%d (0x%08X)\n",
               global_parameter_name(setting.key), setting.key, display,
               setting.value, (uint32_t)setting.value);
    }

    size_t offset = toc.end_offset;
    while (offset + 12 <= raw_length) {
        ERRigSection section;
        if (!parse_rig_section(raw, raw_length, offset, &section))
            break;
        offset = section.end_offset;
        if (section.id < 'C' || section.id > 'L')
            continue;

        size_t slot = (size_t)(section.id - 'C');
        int32_t effect_id = effect_ids[slot];
        int32_t amp_code = 0;
        find_rig_setting(&section, "sld6", &amp_code);
        const char *module_name = effect_name(effect_id);
        if (effect_id == 12) {
            const char *amp = named_code(
                er_amp_names, sizeof(er_amp_names) / sizeof(er_amp_names[0]),
                amp_code);
            printf("\nSlot %c — %s / %s (category %d, effect %d)",
                   section.id, category_name(categories[slot]), module_name,
                   categories[slot], effect_id);
            if (amp)
                printf(" — %s", amp);
            putchar('\n');
        } else {
            printf("\nSlot %c — %s / %s (category %d, effect %d)\n",
                   section.id, category_name(categories[slot]), module_name,
                   categories[slot], effect_id);
        }

        for (size_t index = 0; index < section.count; ++index) {
            ERRigSetting setting = rig_setting_at(&section, index);
            char display[96];
            format_setting_value(display, sizeof(display), effect_id,
                                 setting.key, setting.value);
            printf("  %-25s [%s] = %-22s raw=%d (0x%08X)\n",
                   parameter_name(effect_id, amp_code, setting.key),
                   setting.key, display, setting.value,
                   (uint32_t)setting.value);
        }
    }
    free(raw);
}

static void print_rig_volume(const UInt8 *sysex, size_t sysex_length)
{
    if (sysex_length < 8 || sysex[0] != 0xf0 || sysex[1] != 0x13 ||
        sysex[2] != 0x0b || (sysex[4] != 0x02 && sysex[4] != 0x12) ||
        sysex[5] != 0x07 ||
        sysex[sysex_length - 1] != 0xf7) {
        fputs("unexpected rig-volume response\n", stderr);
        return;
    }

    size_t packed_length = sysex_length - 7;
    if (packed_length != 5) {
        fprintf(stderr, "unexpected rig-volume payload length: %zu\n",
                packed_length);
        return;
    }

    /* Scalar replies split a 32-bit word into 7/7/7/7/4 bits, most-significant
       group first. This differs from the bulk edit-buffer byte-stream packing. */
    const UInt8 *packed = sysex + 6;
    uint32_t bits = ((uint32_t)packed[0] << 25) |
                    ((uint32_t)packed[1] << 18) |
                    ((uint32_t)packed[2] << 11) |
                    ((uint32_t)packed[3] << 4) |
                    (uint32_t)(packed[4] & 0x0f);
    UInt8 raw[4] = {
        (UInt8)(bits >> 24), (UInt8)(bits >> 16),
        (UInt8)(bits >> 8), (UInt8)bits
    };
    puts("decoded rig-volume word:");
    print_hex(raw, sizeof(raw), sizeof(raw));
    int32_t signed_bits = (int32_t)bits;
    printf("rig-volume raw code: %d (0x%08X)\n", signed_bits, bits);

    /* The full signed range maps linearly from -24.0 through 0.0 dB.
       Hardware display precision is one decimal place. */
    uint64_t position = (uint64_t)((int64_t)signed_bits + 2147483648LL);
    double decibels = -24.0 + 24.0 * ((double)position / 4294967295.0);
    if (decibels > -0.000001 && decibels < 0.000001)
        decibels = 0.0;
    printf("current Rig Vol: %.1f dB (unrounded %.6f dB)\n",
           decibels, decibels);
}

static int is_identity_response(const UInt8 *response, size_t length)
{
    return length >= 9 && response[0] == 0xf0 && response[1] == 0x7e &&
           response[3] == 0x06 && response[4] == 0x02 &&
           response[5] == 0x13 && response[6] == 0x0b;
}

static int is_object_response(const UInt8 *response, size_t length,
                              UInt8 device_id, UInt8 object)
{
    if (length < 8 || response[0] != 0xf0 || response[1] != 0x13 ||
        response[2] != 0x0b || response[3] != device_id ||
        response[5] != object || response[length - 1] != 0xf7)
        return 0;
    if (object == 0x01)
        return response[4] == 0x12;
    return response[4] == 0x02 || response[4] == 0x12;
}

#ifndef ERRIG_READ_LIBRARY
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int decode_rig = argc == 3 && strcmp(argv[1], "--decode") == 0;
    int decode_rig_volume = argc == 3 &&
                            strcmp(argv[1], "--decode-rig-volume") == 0;
    if (decode_rig || decode_rig_volume) {
        FILE *input = fopen(argv[2], "rb");
        if (!input) {
            perror(argv[2]);
            return 1;
        }
        if (fseek(input, 0, SEEK_END) != 0) {
            fclose(input);
            return 1;
        }
        long file_length = ftell(input);
        if (file_length <= 0 || fseek(input, 0, SEEK_SET) != 0) {
            fclose(input);
            return 1;
        }
        UInt8 *file_data = malloc((size_t)file_length);
        if (!file_data) {
            fclose(input);
            return 1;
        }
        size_t read_length = fread(file_data, 1, (size_t)file_length, input);
        fclose(input);
        if (read_length != (size_t)file_length) {
            free(file_data);
            return 1;
        }
        if (decode_rig_volume)
            print_rig_volume(file_data, read_length);
        else
            print_rig_summary(file_data, read_length);
        free(file_data);
        return 0;
    }

    int read_rig_volume = argc >= 2 && strcmp(argv[1], "--rig-volume") == 0;
    int read_current_rig = argc >= 2 && strcmp(argv[1], "--current-rig") == 0;
    int known_device_id = argc == 3 &&
                          strcmp(argv[1], "--known-device-id") == 0;
    const char *output_path = read_rig_volume
        ? (argc >= 3 ? argv[2] : "eleven_rack_rig_volume.syx")
        : (known_device_id ? argv[2] :
           (argc > 1 ? argv[1] : "eleven_rack_edit_buffer.syx"));
    int exit_code = 1;
    IOUSBDeviceInterface500 **device = NULL;
    IOUSBInterfaceInterface500 **midi = NULL;
    UInt8 *response = NULL;
    int direct_midi_access = 0;

    SInt32 vid = ER_VID;
    SInt32 pid = ER_PID;
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef vendor = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
    CFNumberRef product = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), vendor);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), product);
    CFRelease(vendor);
    CFRelease(product);

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                        match);
    if (!service) {
        fputs("Eleven Rack not found\n", stderr);
        goto cleanup;
    }
    device = open_device(service);
    IOObjectRelease(service);
    IOReturn result = kIOReturnSuccess;
    if (device) {
        result = (*device)->USBDeviceOpen(device);
        if (result == kIOReturnExclusiveAccess)
            result = (*device)->USBDeviceOpenSeize(device);
        if (result == kIOReturnSuccess) {
            (*device)->SetConfiguration(device, 1);
            midi = open_midi_interface(device);
        } else {
            fprintf(stderr, "whole-device open unavailable: 0x%x\n", result);
            (*device)->Release(device);
            device = NULL;
        }
    }
    if (!midi) {
        fputs("using direct MIDI-interface access\n", stderr);
        midi = open_midi_interface_direct();
        direct_midi_access = midi != NULL;
    }
    if (!midi) {
        fputs("MIDI interface 2 not found\n", stderr);
        goto cleanup;
    }
    result = (*midi)->USBInterfaceOpen(midi);
    if (result == kIOReturnExclusiveAccess) {
        fputs("MIDI interface is owned; seizing it for this read-only test\n",
              stderr);
        result = (*midi)->USBInterfaceOpenSeize(midi);
    }
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "MIDI interface open failed: 0x%x\n", result);
        goto cleanup;
    }
    UInt8 input_pipe = 0;
    UInt8 output_pipe = 0;
    UInt8 endpoint_count = 0;
    (*midi)->GetNumEndpoints(midi, &endpoint_count);
    for (UInt8 pipe = 1; pipe <= endpoint_count; ++pipe) {
        UInt8 direction = 0, number = 0, transfer = 0, interval = 0;
        UInt16 maximum = 0;
        (*midi)->GetPipeProperties(midi, pipe, &direction, &number,
                                   &transfer, &maximum, &interval);
        if (transfer == kUSBBulk && direction == kUSBIn)
            input_pipe = pipe;
        if (transfer == kUSBBulk && direction == kUSBOut)
            output_pipe = pipe;
    }
    if (!input_pipe || !output_pipe) {
        fputs("bulk MIDI endpoints missing\n", stderr);
        goto cleanup;
    }

    response = malloc(ER_MAX_SYSEX);
    if (!response)
        goto cleanup;

    UInt8 device_id = 0x0f;
    size_t response_length = 0;
    int found_response = 0;
    if (!direct_midi_access && !known_device_id) {
        if (!send_identity_request(midi, output_pipe)) {
            fputs("identity request failed\n", stderr);
            goto cleanup;
        }
        for (int attempt = 0; attempt < 32; ++attempt) {
            response_length = 0;
            if (!read_sysex(midi, input_pipe, response, ER_MAX_SYSEX,
                            &response_length))
                break;
            if (is_identity_response(response, response_length)) {
                found_response = 1;
                break;
            }
            printf("ignored pending SysEx: function 0x%02X object 0x%02X (%zu bytes)\n",
                   response_length > 4 ? response[4] : 0,
                   response_length > 5 ? response[5] : 0, response_length);
        }
        if (!found_response) {
            fputs("MIDI identity response not received\n", stderr);
            goto cleanup;
        }
        device_id = response[2];
        printf("Eleven Rack identity confirmed; device ID 0x%02X\n",
               device_id);
    } else {
        puts("using observed Eleven Rack editor device ID 0x0F");
    }
    UInt8 requested_object = read_rig_volume ? 0x07 :
                             (read_current_rig ? 0x02 : 0x01);
    if (!send_get_request(midi, output_pipe, device_id, requested_object)) {
        fputs(read_rig_volume ? "rig-volume request failed\n"
                              : "edit-buffer request failed\n", stderr);
        goto cleanup;
    }
    response_length = 0;
    found_response = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        response_length = 0;
        if (!read_sysex(midi, input_pipe, response, ER_MAX_SYSEX,
                        &response_length))
            break;
        if (is_object_response(response, response_length, device_id,
                               requested_object)) {
            found_response = 1;
            break;
        }
        printf("ignored asynchronous SysEx: function 0x%02X object 0x%02X (%zu bytes)\n",
               response_length > 4 ? response[4] : 0,
               response_length > 5 ? response[5] : 0, response_length);
    }
    if (!found_response) {
        fputs(read_rig_volume ? "rig-volume response not received\n"
                              : "edit-buffer response not received\n", stderr);
        goto cleanup;
    }

    if (read_current_rig) {
        if (response_length < 9) {
            fputs("current-program response is too short\n", stderr);
            goto cleanup;
        }
        UInt8 bank = response[6], rig = response[7];
        printf("current program: %s %02u%c (bank=%u index=%u)\n",
               bank == 1 ? "Factory" : "User", (unsigned)(rig / 4 + 1),
               (char)('A' + rig % 4), (unsigned)bank, (unsigned)rig);
        exit_code = 0;
        goto cleanup;
    }

    printf("%s SysEx received: %zu bytes\n",
           read_rig_volume ? "rig-volume" : "edit-buffer", response_length);
    print_hex(response, response_length, 128);
    if (read_rig_volume)
        print_rig_volume(response, response_length);
    else
        print_rig_summary(response, response_length);

    FILE *output = fopen(output_path, "wb");
    if (!output) {
        perror(output_path);
        goto cleanup;
    }
    size_t written = fwrite(response, 1, response_length, output);
    if (fclose(output) != 0 || written != response_length) {
        perror(output_path);
        goto cleanup;
    }
    printf("saved: %s\n", output_path);
    exit_code = 0;

cleanup:
    free(response);
    if (midi) {
        (*midi)->USBInterfaceClose(midi);
        (*midi)->Release(midi);
    }
    if (device) {
        (*device)->USBDeviceClose(device);
        (*device)->Release(device);
    }
    return exit_code;
}
#endif
