#define ERRIG_READ_LIBRARY 1
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "errig_read.c"
#pragma clang diagnostic pop

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#define ER_RIGS_PER_BANK 104
#define ER_SWITCH_INTERVAL_SECONDS 15.0
#define ER_SETTLE_SECONDS 2

static volatile sig_atomic_t er_inventory_stop;

static void inventory_signal(int signal_number)
{
    (void)signal_number;
    er_inventory_stop = 1;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static int wait_for_switch_window(const struct timespec *last_switch,
                                  int have_last_switch)
{
    if (!have_last_switch)
        return !er_inventory_stop;

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double remaining = ER_SWITCH_INTERVAL_SECONDS -
                           elapsed_seconds(last_switch, &now);
        if (remaining <= 0.0)
            return !er_inventory_stop;
        struct timespec delay = {
            (time_t)remaining,
            (long)((remaining - (time_t)remaining) * 1000000000.0)
        };
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR)
            return 0;
        if (er_inventory_stop)
            return 0;
    }
}

static IOReturn send_rig_change(IOUSBInterfaceInterface500 **midi,
                                UInt8 output_pipe, UInt8 device_id,
                                UInt8 bank, UInt8 rig)
{
    /* MIDI: F0 13 0B <device> 00 02 <bank> <rig> F7.
       Bank 0 is User and bank 1 is Factory. */
    UInt8 request[12] = {
        0x04, 0xf0, 0x13, 0x0b,
        0x04, device_id, 0x00, 0x02,
        0x07, bank, rig, 0xf7
    };
    return (*midi)->WritePipeTO(midi, output_pipe, request,
                                sizeof(request), 1000, 1000);
}

static int wait_for_object(IOUSBInterfaceInterface500 **midi,
                           UInt8 input_pipe, UInt8 *response,
                           size_t *response_length, UInt8 device_id,
                           UInt8 object)
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        *response_length = 0;
        if (!read_sysex(midi, input_pipe, response, ER_MAX_SYSEX,
                        response_length))
            return 0;
        if (is_object_response(response, *response_length, device_id, object))
            return 1;
    }
    return 0;
}

static int query_current_rig(IOUSBInterfaceInterface500 **midi,
                             UInt8 input_pipe, UInt8 output_pipe,
                             UInt8 device_id, UInt8 *bank, UInt8 *rig,
                             UInt8 *response)
{
    if (!send_get_request(midi, output_pipe, device_id, 0x02))
        return 0;
    size_t length = 0;
    if (!wait_for_object(midi, input_pipe, response, &length, device_id, 0x02) ||
        length < 9)
        return 0;
    *bank = response[6];
    *rig = response[7];
    return 1;
}

static int query_edit_buffer(IOUSBInterfaceInterface500 **midi,
                             UInt8 input_pipe, UInt8 output_pipe,
                             UInt8 device_id, UInt8 *response,
                             size_t *response_length)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!send_get_request(midi, output_pipe, device_id, 0x01))
            continue;
        if (wait_for_object(midi, input_pipe, response, response_length,
                            device_id, 0x01) && *response_length > 100)
            return 1;
    }
    return 0;
}

static void program_label(UInt8 rig, char label[4])
{
    snprintf(label, 4, "%02u%c", (unsigned)(rig / 4 + 1),
             (char)('A' + rig % 4));
}

static int valid_capture(const char *path)
{
    struct stat status;
    return stat(path, &status) == 0 && status.st_size > 100;
}

static void capture_name(const UInt8 *sysex, size_t sysex_length,
                         char name[29])
{
    name[0] = '\0';
    if (sysex_length < 8)
        return;
    size_t packed_length = sysex_length - 7;
    size_t capacity = packed_length * 7 / 8 + 1;
    UInt8 *raw = malloc(capacity);
    if (!raw)
        return;
    size_t raw_length = unpack_7bit(sysex + 6, packed_length, raw, capacity);
    if (raw_length >= 36) {
        size_t used = 0;
        for (size_t word = 0; word < 28 && used < 28; word += 4) {
            for (int byte_index = 3; byte_index >= 0; --byte_index) {
                UInt8 byte = raw[8 + word + (size_t)byte_index];
                if (byte == 0 || byte < 0x20 || byte > 0x7e)
                    goto done;
                name[used++] = (char)byte;
            }
        }
done:
        name[used] = '\0';
    }
    free(raw);
}

static int save_capture(const char *path, const UInt8 *bytes, size_t length)
{
    char partial[1024];
    if (snprintf(partial, sizeof(partial), "%s.partial", path) >=
        (int)sizeof(partial))
        return 0;
    FILE *file = fopen(partial, "wb");
    if (!file)
        return 0;
    size_t written = fwrite(bytes, 1, length, file);
    int close_result = fclose(file);
    if (written != length || close_result != 0) {
        unlink(partial);
        return 0;
    }
    if (rename(partial, path) != 0) {
        unlink(partial);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
        return 64;
    }
    const char *output_directory = argv[1];
    if (mkdir(output_directory, 0755) != 0 && errno != EEXIST) {
        perror(output_directory);
        return 1;
    }

    signal(SIGINT, inventory_signal);
    signal(SIGTERM, inventory_signal);

    int exit_code = 1;
    IOUSBDeviceInterface500 **device = NULL;
    IOUSBInterfaceInterface500 **midi = NULL;
    UInt8 *response = malloc(ER_MAX_SYSEX);
    if (!response)
        return 1;

    SInt32 vid = ER_VID, pid = ER_PID;
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
    if (!midi)
        goto cleanup;
    result = (*midi)->USBInterfaceOpen(midi);
    if (result == kIOReturnExclusiveAccess)
        result = (*midi)->USBInterfaceOpenSeize(midi);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "MIDI interface open failed: 0x%x\n", result);
        goto cleanup;
    }

    UInt8 input_pipe = 0, output_pipe = 0, endpoint_count = 0;
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
    if (!input_pipe || !output_pipe)
        goto cleanup;

    if (!send_identity_request(midi, output_pipe))
        goto cleanup;
    size_t response_length = 0;
    int identity_found = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        if (!read_sysex(midi, input_pipe, response, ER_MAX_SYSEX,
                        &response_length))
            break;
        if (is_identity_response(response, response_length)) {
            identity_found = 1;
            break;
        }
    }
    if (!identity_found) {
        fputs("identity response not received\n", stderr);
        goto cleanup;
    }
    UInt8 device_id = response[2];
    UInt8 original_bank = 0, original_rig = 0;
    int have_original = query_current_rig(midi, input_pipe, output_pipe,
                                          device_id, &original_bank,
                                          &original_rig, response);
    char original_path[1024];
    if (snprintf(original_path, sizeof(original_path), "%s/original_program.txt",
                 output_directory) >= (int)sizeof(original_path))
        goto cleanup;
    FILE *original_file = fopen(original_path, "r");
    if (original_file) {
        unsigned saved_bank = 0, saved_rig = 0;
        if (fscanf(original_file, "%u %u", &saved_bank, &saved_rig) == 2 &&
            saved_bank <= 1 && saved_rig < ER_RIGS_PER_BANK) {
            original_bank = (UInt8)saved_bank;
            original_rig = (UInt8)saved_rig;
            have_original = 1;
        }
        fclose(original_file);
    } else if (have_original) {
        original_file = fopen(original_path, "w");
        if (original_file) {
            fprintf(original_file, "%u %u\n", original_bank, original_rig);
            fclose(original_file);
        }
    }
    if (have_original) {
        char label[4];
        program_label(original_rig, label);
        printf("original program: %s %s\n",
               original_bank == 1 ? "Factory" : "User", label);
    } else {
        fputs("warning: current program could not be recorded\n", stderr);
    }

    const UInt8 banks[2] = { 1, 0 };
    struct timespec last_switch = { 0, 0 };
    int have_last_switch = 0;
    unsigned completed = 0;
    for (size_t bank_index = 0; bank_index < 2 && !er_inventory_stop;
         ++bank_index) {
        UInt8 bank = banks[bank_index];
        const char *bank_name = bank == 1 ? "factory" : "user";
        for (UInt8 rig = 0; rig < ER_RIGS_PER_BANK && !er_inventory_stop;
             ++rig) {
            char label[4], path[1024];
            program_label(rig, label);
            if (snprintf(path, sizeof(path), "%s/%s_%s.syx",
                         output_directory, bank_name, label) >=
                (int)sizeof(path))
                goto cleanup;
            if (valid_capture(path)) {
                ++completed;
                printf("[%3u/208] resume: %s %s already captured\n",
                       completed, bank_name, label);
                fflush(stdout);
                continue;
            }

            if (!wait_for_switch_window(&last_switch, have_last_switch))
                break;
            result = send_rig_change(midi, output_pipe, device_id, bank, rig);
            if (result != kIOReturnSuccess) {
                fprintf(stderr, "switch failed for %s %s: 0x%x\n",
                        bank_name, label, result);
                goto cleanup;
            }
            clock_gettime(CLOCK_MONOTONIC, &last_switch);
            have_last_switch = 1;
            sleep(ER_SETTLE_SECONDS);

            UInt8 actual_bank = 0xff, actual_rig = 0xff;
            int program_verified = 0;
            for (int verify_attempt = 0; verify_attempt < 5;
                 ++verify_attempt) {
                if (query_current_rig(midi, input_pipe, output_pipe, device_id,
                                      &actual_bank, &actual_rig, response) &&
                    actual_bank == bank && actual_rig == rig) {
                    program_verified = 1;
                    break;
                }
                sleep(1);
            }
            if (!program_verified) {
                fprintf(stderr, "program verification failed for %s %s\n",
                        bank_name, label);
                goto cleanup;
            }
            response_length = 0;
            if (!query_edit_buffer(midi, input_pipe, output_pipe, device_id,
                                   response, &response_length)) {
                fprintf(stderr, "edit-buffer read failed for %s %s\n",
                        bank_name, label);
                goto cleanup;
            }
            if (!save_capture(path, response, response_length)) {
                perror(path);
                goto cleanup;
            }
            char name[29];
            capture_name(response, response_length, name);
            ++completed;
            printf("[%3u/208] captured %-7s %s  %-28s  %zu bytes\n",
                   completed, bank_name, label,
                   name[0] ? name : "(unnamed)", response_length);
            fflush(stdout);
        }
    }

    if (have_original && have_last_switch &&
        wait_for_switch_window(&last_switch, have_last_switch)) {
        if (send_rig_change(midi, output_pipe, device_id, original_bank,
                            original_rig) == kIOReturnSuccess) {
            char label[4];
            program_label(original_rig, label);
            printf("restored original program: %s %s\n",
                   original_bank == 1 ? "Factory" : "User", label);
        }
    }
    if (!er_inventory_stop && completed == 208) {
        puts("inventory complete: 208/208 programs captured");
        exit_code = 0;
    } else {
        fprintf(stderr, "inventory stopped after %u/208 captures; rerun to resume\n",
                completed);
        exit_code = er_inventory_stop ? 130 : 1;
    }

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
