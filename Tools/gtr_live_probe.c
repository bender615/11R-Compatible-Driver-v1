#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void dump_bytes(const char *name, const uint8_t *bytes, size_t length,
                       const char *output_path)
{
    printf("%s @ %p (%zu bytes)\n", name, (const void *)bytes, length);
    for (size_t i = 0; i < length; ++i) {
        if ((i % 16) == 0)
            printf("  %04zx:", i);
        printf(" %02x", bytes[i]);
        if ((i % 16) == 15 || i + 1 == length)
            putchar('\n');
    }

    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        if (!output) {
            perror(output_path);
            return;
        }
        if (fwrite(bytes, 1, length, output) != length)
            perror(output_path);
        fclose(output);
    }
}

static const uint8_t *dump_symbol(void *handle, const char *name, size_t length,
                                  const char *output_path)
{
    const uint8_t *bytes = (const uint8_t *)dlsym(handle, name);
    if (!bytes) {
        fprintf(stderr, "%s: %s\n", name, dlerror());
        return NULL;
    }
    dump_bytes(name, bytes, length, output_path);
    return bytes;
}

int main(void)
{
    static const char *framework =
        "/Applications/Pro Tools.app/Contents/Frameworks/Gtr.framework/Versions/A/Gtr";
    void *handle = dlopen(framework, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    dump_symbol(handle, "cElevenRig_ManufactureID", 4, NULL);
    dump_symbol(handle, "cElevenRig_ProductID", 4, NULL);
    dump_symbol(handle, "cElevenRig_PlugInID", 4, NULL);
    const uint8_t *header = dump_symbol(
        handle, "_ZN11Gtr_Message17push_SysEx_HeaderEv", 120,
        "/tmp/gtr_sysex_header.bin");
    dump_symbol(handle, "_ZN11Gtr_Message11push_OSTypeEj", 148,
                "/tmp/gtr_push_ostype.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI22GetEditBufferPatchDataEN13Gtr_TokenPath10EDirectionE",
                64, "/tmp/gtr_get_edit_buffer.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI22GetEditBufferPatchNameEN13Gtr_TokenPath10EDirectionE",
                64, "/tmp/gtr_get_edit_buffer_name.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI22GetEditBufferRigVolumeEN13Gtr_TokenPath10EDirectionE",
                64, "/tmp/gtr_get_edit_buffer_volume.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI12GetPatchDataEN13Gtr_TokenPath10EDirectionEa",
                96, "/tmp/gtr_get_patch_data.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI12GetPatchNameEN13Gtr_TokenPath10EDirectionENSt3__14pairI8EGtrBankaEE",
                96, "/tmp/gtr_get_patch_name.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI8GetTempoEN13Gtr_TokenPath10EDirectionE",
                64, "/tmp/gtr_get_tempo.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI11GetRigInputEN13Gtr_TokenPath10EDirectionE",
                64, "/tmp/gtr_get_rig_input.bin");
    dump_symbol(handle,
                "_ZN15Gtr_Bridge_MIDI10PushBufferER14Cmn_PolyVectorIhLb0EEPhi",
                276, "/tmp/gtr_push_buffer.bin");
    dump_symbol(handle,
                "_ZN17Gtr_Device_Parser9PopBufferER13Gtr_SysexIterRi",
                616, "/tmp/gtr_pop_buffer.bin");
    dump_symbol(handle,
                "_ZN17Gtr_Device_Parser12PopPatchDataER13Gtr_SysexIterR16Gtr_SettingsData",
                180, "/tmp/gtr_pop_patch_data.bin");

    /* These two helper symbols are local, so dlsym cannot resolve them.  Their
       offsets relative to push_SysEx_Header are stable within this image and
       come from its public symbol table. */
    if (header) {
        dump_bytes("Gtr_Message::Gtr_Message", header + (0x21af8 - 0x1e92c),
                   0xac, "/tmp/gtr_message_ctor.bin");
        dump_bytes("Gtr_Message::~Gtr_Message", header + (0x21c04 - 0x1e92c),
                   0x70, "/tmp/gtr_message_dtor.bin");
    }

    dlclose(handle);
    return 0;
}
