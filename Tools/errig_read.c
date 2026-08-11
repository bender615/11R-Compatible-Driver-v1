#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (raw_length >= 36) {
        char name[29];
        size_t name_length = 0;
        for (size_t word = 0; word < 28 && name_length < 28; word += 4) {
            for (int byte_index = 3; byte_index >= 0; --byte_index) {
                UInt8 byte = raw[8 + word + (size_t)byte_index];
                if (byte == 0)
                    goto name_complete;
                if (byte < 0x20 || byte > 0x7e)
                    goto name_complete;
                name[name_length++] = (char)byte;
            }
        }
name_complete:
        name[name_length] = '\0';
        if (name_length)
            printf("current rig name: %s\n", name);
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

int main(int argc, char **argv)
{
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
    const char *output_path = read_rig_volume
        ? (argc >= 3 ? argv[2] : "eleven_rack_rig_volume.syx")
        : (argc > 1 ? argv[1] : "eleven_rack_edit_buffer.syx");
    int exit_code = 1;
    IOUSBDeviceInterface500 **device = NULL;
    IOUSBInterfaceInterface500 **midi = NULL;
    UInt8 *response = NULL;

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
    if (!device)
        goto cleanup;

    IOReturn result = (*device)->USBDeviceOpen(device);
    if (result == kIOReturnExclusiveAccess)
        result = (*device)->USBDeviceOpenSeize(device);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "device open failed: 0x%x\n", result);
        goto cleanup;
    }
    (*device)->SetConfiguration(device, 1);

    midi = open_midi_interface(device);
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

    if (!send_identity_request(midi, output_pipe)) {
        fputs("identity request failed\n", stderr);
        goto cleanup;
    }
    size_t response_length = 0;
    int found_response = 0;
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

    UInt8 device_id = response[2];
    printf("Eleven Rack identity confirmed; device ID 0x%02X\n", device_id);
    UInt8 requested_object = read_rig_volume ? 0x07 : 0x01;
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
