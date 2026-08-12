#define ERRIG_READ_LIBRARY 1
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "errig_read.c"
#pragma clang diagnostic pop

#include <sys/stat.h>

static void csv_text(FILE *output, const char *text)
{
    fputc('"', output);
    for (; *text; ++text) {
        if (*text == '"')
            fputc('"', output);
        fputc(*text, output);
    }
    fputc('"', output);
}

static void program_label(unsigned rig, char label[4])
{
    snprintf(label, 4, "%02u%c", rig / 4 + 1,
             (char)('A' + rig % 4));
}

static UInt8 *load_file(const char *path, size_t *length)
{
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    UInt8 *bytes = malloc((size_t)size);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    size_t read_size = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        free(bytes);
        return NULL;
    }
    *length = read_size;
    return bytes;
}

static void extract_name(const UInt8 *raw, size_t raw_length, char name[29])
{
    name[0] = '\0';
    if (raw_length < 36)
        return;
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

static void write_row(FILE *output, const char *bank, const char *program,
                      const char *rig_name, const char *slot,
                      int32_t category, int have_category, int32_t effect_id,
                      int have_effect, const char *effect, int enabled,
                      int have_enabled, const ERRigSetting *setting,
                      const char *parameter, const char *display)
{
    csv_text(output, bank); fputc(',', output);
    csv_text(output, program); fputc(',', output);
    csv_text(output, rig_name); fputc(',', output);
    csv_text(output, slot); fputc(',', output);
    if (have_category) fprintf(output, "%d", category);
    fputc(',', output);
    csv_text(output, have_category ? category_name(category) : "");
    fputc(',', output);
    if (have_effect) fprintf(output, "%d", effect_id);
    fputc(',', output);
    csv_text(output, effect); fputc(',', output);
    if (have_enabled) fputs(enabled ? "1" : "0", output);
    fputc(',', output);
    csv_text(output, setting->key); fputc(',', output);
    csv_text(output, parameter); fputc(',', output);
    fprintf(output, "%d,0x%08X,", setting->value,
            (uint32_t)setting->value);
    csv_text(output, display);
    fputc(',', output);
    csv_text(output, parameter_unit(effect_id, setting->key));
    fputc(',', output);
    csv_text(output, parameter_range(effect_id, setting->key));
    fputc(',', output);
    csv_text(output, parameter_curve(effect_id, setting->key));
    fputc('\n', output);
}

static int export_rig(FILE *output, const char *bank, const char *program,
                      const UInt8 *sysex, size_t sysex_length)
{
    if (sysex_length < 8 || sysex[0] != 0xf0 || sysex[1] != 0x13 ||
        sysex[2] != 0x0b || sysex[4] != 0x12 || sysex[5] != 0x01 ||
        sysex[sysex_length - 1] != 0xf7)
        return 0;
    size_t packed_length = sysex_length - 7;
    size_t capacity = packed_length * 7 / 8 + 1;
    UInt8 *raw = malloc(capacity);
    if (!raw)
        return 0;
    size_t raw_length = unpack_7bit(sysex + 6, packed_length, raw, capacity);
    char rig_name[29];
    extract_name(raw, raw_length, rig_name);
    ERRigSection toc;
    if (!parse_rig_section(raw, raw_length, 36, &toc) || toc.id != 'A') {
        free(raw);
        return 0;
    }
    int32_t effect_ids[10] = { 0 }, categories[10] = { 0 };
    for (size_t index = 0; index < toc.count; ++index) {
        ERRigSetting setting = rig_setting_at(&toc, index);
        if (!memcmp(setting.key, "Wor", 3) && setting.key[3] >= 'C' &&
            setting.key[3] <= 'L')
            effect_ids[setting.key[3] - 'C'] = setting.value;
        if (!memcmp(setting.key, "Wst", 3) && setting.key[3] >= 'C' &&
            setting.key[3] <= 'L')
            categories[setting.key[3] - 'C'] = setting.value;
        char display[96];
        format_setting_value(display, sizeof(display), -1, setting.key,
                             setting.value);
        write_row(output, bank, program, rig_name, "Global", 16, 1, 0, 0,
                  "", 0, 0, &setting, global_parameter_name(setting.key),
                  display);
    }

    size_t offset = toc.end_offset;
    while (offset + 12 <= raw_length) {
        ERRigSection section;
        if (!parse_rig_section(raw, raw_length, offset, &section))
            break;
        offset = section.end_offset;
        if (section.id < 'C' || section.id > 'L')
            continue;
        size_t slot_index = (size_t)(section.id - 'C');
        int32_t effect_id = effect_ids[slot_index];
        int32_t amp_code = 0, bypass = 0;
        find_rig_setting(&section, "sld6", &amp_code);
        int have_enabled = find_rig_setting(&section, "bypa", &bypass);
        if (!have_enabled && effect_id == 12)
            have_enabled = find_rig_setting(&section, "sld5", &bypass);
        char slot[2] = { section.id, '\0' };
        for (size_t index = 0; index < section.count; ++index) {
            ERRigSetting setting = rig_setting_at(&section, index);
            char display[96];
            format_setting_value(display, sizeof(display), effect_id,
                                 setting.key, setting.value);
            write_row(output, bank, program, rig_name, slot,
                      categories[slot_index], 1, effect_id, 1,
                      effect_name(effect_id), !bypass, have_enabled,
                      &setting, parameter_name(effect_id, amp_code,
                                               setting.key), display);
        }
    }
    free(raw);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s INVENTORY_DIRECTORY\n", argv[0]);
        return 64;
    }
    char output_path[1024];
    if (snprintf(output_path, sizeof(output_path), "%s/settings.csv", argv[1])
        >= (int)sizeof(output_path))
        return 1;
    FILE *output = fopen(output_path, "w");
    if (!output) {
        perror(output_path);
        return 1;
    }
    fputs("bank,program,rig_name,slot,category_id,category,effect_id,effect,enabled,key,parameter,raw,hex,display,unit,range,curve\n",
          output);
    unsigned exported = 0, missing = 0;
    const char *banks[2] = { "factory", "user" };
    for (size_t bank_index = 0; bank_index < 2; ++bank_index) {
        for (unsigned rig = 0; rig < 104; ++rig) {
            char label[4], path[1024];
            program_label(rig, label);
            if (snprintf(path, sizeof(path), "%s/%s_%s.syx", argv[1],
                         banks[bank_index], label) >= (int)sizeof(path)) {
                ++missing;
                continue;
            }
            size_t length = 0;
            UInt8 *sysex = load_file(path, &length);
            if (!sysex || !export_rig(output, banks[bank_index], label,
                                      sysex, length)) {
                ++missing;
                free(sysex);
                continue;
            }
            free(sysex);
            ++exported;
        }
    }
    if (fclose(output) != 0)
        return 1;
    printf("exported %u rigs to %s (%u missing or invalid)\n",
           exported, output_path, missing);
    return missing ? 2 : 0;
}
