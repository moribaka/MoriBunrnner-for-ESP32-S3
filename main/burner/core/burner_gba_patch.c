#include "burner_gba_patch.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PATCH_SCAN_BYTES (32U * 1024U)
#define PATCH_ANALYSIS_CHUNK_BYTES (4U * 1024U * 1024U)
#define PATCH_WAITCNT_ADDRESS 0x04000204U
#define BATTERYLESS_MARKER "<3 from Maniac"
#define BATTERYLESS_MIN_ROM (0x400000U)

#ifndef BURNER_FILE_PATH_LEN
#define BURNER_FILE_PATH_LEN 304U
#endif

typedef struct {
    const char *name;
    const unsigned char *marker;
    size_t marker_len;
    const unsigned char *replace;
    size_t replace_len;
    const unsigned char *marker_mask;
    size_t marker_mask_len;
} sram_patch_t;

typedef struct {
    const char *name;
    const unsigned char *identifier;
    size_t identifier_len;
    const sram_patch_t *patches;
    size_t patch_count;
} sram_patch_set_t;

#include "burner_gba_patch_generated.h"

static void set_error(char *error_msg, size_t error_msg_len, const char *message)
{
    if (error_msg != NULL && error_msg_len > 0U) {
        snprintf(error_msg, error_msg_len, "%s", message != NULL ? message : "patch failed");
    }
}

static int file_size(FILE *fp, uint32_t *size_out)
{
    long end;

    if (fp == NULL || size_out == NULL || fseek(fp, 0L, SEEK_END) != 0) {
        return -1;
    }
    end = ftell(fp);
    if (end < 0L || (uint64_t)end > UINT32_MAX || fseek(fp, 0L, SEEK_SET) != 0) {
        return -1;
    }
    *size_out = (uint32_t)end;
    return 0;
}

static int find_pattern(
    FILE *fp,
    uint32_t total,
    const unsigned char *pattern,
    size_t pattern_len,
    const unsigned char *mask,
    size_t mask_len,
    uint32_t *offset_out)
{
    unsigned char *buffer;
    size_t carry = 0U;
    uint32_t base = 0U;

    if (fp == NULL || pattern == NULL || pattern_len == 0U || offset_out == NULL || total < pattern_len) {
        return -1;
    }
    buffer = (unsigned char *)malloc(PATCH_SCAN_BYTES + pattern_len - 1U);
    if (buffer == NULL) {
        return -2;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        free(buffer);
        return -1;
    }

    while (base < total) {
        size_t want = total - base;
        size_t got;
        size_t i;
        if (want > PATCH_SCAN_BYTES) {
            want = PATCH_SCAN_BYTES;
        }
        got = fread(buffer + carry, 1U, want, fp);
        if (got != want) {
            free(buffer);
            return -1;
        }
        for (i = 0U; i + pattern_len <= carry + got; ++i) {
            bool matched = true;
            size_t pattern_index;
            for (pattern_index = 0U; pattern_index < pattern_len; ++pattern_index) {
                if (mask != NULL && mask_len == pattern_len && mask[pattern_index] != 0U) {
                    continue;
                }
                if (buffer[i + pattern_index] != pattern[pattern_index]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                *offset_out = base - (uint32_t)carry + (uint32_t)i;
                free(buffer);
                return 0;
            }
        }
        if (pattern_len > 1U) {
            size_t keep = carry + got;
            if (keep > pattern_len - 1U) {
                keep = pattern_len - 1U;
            }
            memmove(buffer, buffer + carry + got - keep, keep);
            carry = keep;
        } else {
            carry = 0U;
        }
        base += (uint32_t)got;
    }
    free(buffer);
    return 1;
}

/* Scan every known patch identifier during one sequential TF read. */
static int scan_patch_identifiers(FILE *fp, uint32_t total, bool *found_out)
{
    const size_t set_count = sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0]);
    unsigned char *buffer = NULL;
    size_t max_len = 0U;
    size_t carry = 0U;
    uint32_t offset = 0U;
    bool any_found = false;
    bool psram_buffer = false;

    if (fp == NULL || found_out == NULL) {
        return -1;
    }
    for (size_t i = 0U; i < set_count; ++i) {
        found_out[i] = false;
        if (s_generated_patch_sets[i].identifier_len > max_len) {
            max_len = s_generated_patch_sets[i].identifier_len;
        }
    }
    if (max_len == 0U || total < max_len || fseek(fp, 0L, SEEK_SET) != 0) {
        return 0;
    }

    buffer = (unsigned char *)heap_caps_malloc(
        PATCH_ANALYSIS_CHUNK_BYTES + max_len - 1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psram_buffer = buffer != NULL;
    if (buffer == NULL) {
        buffer = (unsigned char *)malloc(PATCH_ANALYSIS_CHUNK_BYTES + max_len - 1U);
    }
    if (buffer == NULL) {
        return -1;
    }

    while (offset < total && !any_found) {
        size_t want = total - offset;
        size_t got;
        size_t span;
        if (want > PATCH_ANALYSIS_CHUNK_BYTES) {
            want = PATCH_ANALYSIS_CHUNK_BYTES;
        }
        got = fread(buffer + carry, 1U, want, fp);
        if (got != want) {
            if (psram_buffer) heap_caps_free(buffer); else free(buffer);
            return -1;
        }
        span = carry + got;
        for (size_t set_index = 0U; set_index < set_count; ++set_index) {
            const unsigned char *pattern = s_generated_patch_sets[set_index].identifier;
            size_t pattern_len = s_generated_patch_sets[set_index].identifier_len;
            if (found_out[set_index] || pattern_len > span) {
                continue;
            }
            for (size_t start = 0U; start + pattern_len <= span; ++start) {
                if ((start & 0xFFFFU) == 0U) {
                    vTaskDelay(1);
                }
                if (buffer[start] != pattern[0]) {
                    continue;
                }
                if (memcmp(buffer + start, pattern, pattern_len) == 0) {
                    found_out[set_index] = true;
                    any_found = true;
                    break;
                }
            }
        }
        if (max_len > 1U) {
            size_t keep = span;
            if (keep > max_len - 1U) {
                keep = max_len - 1U;
            }
            memmove(buffer, buffer + span - keep, keep);
            carry = keep;
        } else {
            carry = 0U;
        }
        offset += (uint32_t)got;
    }
    if (psram_buffer) heap_caps_free(buffer); else free(buffer);
    return 0;
}

static int write_pattern(FILE *fp, uint32_t total, uint32_t offset, const unsigned char *data, size_t len)
{
    if (fp == NULL || data == NULL || offset > total || len > (size_t)(total - offset) || fseek(fp, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    return fwrite(data, 1U, len, fp) == len ? 0 : -1;
}

static int read_at(FILE *fp, uint32_t offset, void *data, size_t len)
{
    if (fp == NULL || data == NULL || fseek(fp, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    return fread(data, 1U, len, fp) == len ? 0 : -1;
}

static uint16_t read_u16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool has_thumb_ldr_pc(FILE *fp, uint32_t target_word, uint32_t total)
{
    uint32_t target_halfword = target_word * 2U;
    uint32_t start = target_halfword > 256U ? target_halfword - 256U : 0U;
    uint32_t count = target_halfword - start;
    unsigned char *buf;
    uint32_t i;

    if (count == 0U) {
        return false;
    }
    if ((uint64_t)start * 2U + (uint64_t)count * 2U > total) {
        return false;
    }
    buf = (unsigned char *)malloc((size_t)count * 2U);
    if (buf == NULL || read_at(fp, start * 2U, buf, (size_t)count * 2U) != 0) {
        free(buf);
        return false;
    }
    for (i = 0U; i < count; ++i) {
        uint16_t instruction = read_u16(buf + i * 2U);
        uint32_t target = ((start + i) & ~1U) + ((uint32_t)instruction & 0xFFU) * 2U + 2U;
        if ((instruction >> 11) == 0x09U && target == target_halfword) {
            free(buf);
            return true;
        }
    }
    free(buf);
    return false;
}

static bool has_arm_ldr_pc(FILE *fp, uint32_t target_word, uint32_t total)
{
    uint32_t start = target_word > 1024U ? target_word - 1024U : 0U;
    uint32_t count = target_word - start;
    unsigned char *buf;
    uint32_t i;

    if (count == 0U || (uint64_t)start * 4U + (uint64_t)count * 4U > total) {
        return false;
    }
    buf = (unsigned char *)malloc((size_t)count * 4U);
    if (buf == NULL || read_at(fp, start * 4U, buf, (size_t)count * 4U) != 0) {
        free(buf);
        return false;
    }
    for (i = 0U; i < count; ++i) {
        uint32_t instruction = read_u32(buf + i * 4U);
        uint32_t opcode = (instruction >> 20) & 0xFFU;
        uint32_t rn = (instruction >> 16) & 0x0FU;
        uint32_t imm12 = instruction & 0xFFFU;
        if (opcode == 0x59U && rn == 15U && (imm12 & 3U) == 0U && start + i + (imm12 >> 2) + 2U == target_word) {
            free(buf);
            return true;
        }
    }
    free(buf);
    return false;
}

static int apply_waitcnt(FILE *fp, uint32_t total, uint32_t *count_out)
{
    unsigned char chunk[PATCH_SCAN_BYTES];
    uint32_t offset = 0U;
    uint32_t count = 0U;

    while (offset + 4U <= total) {
        size_t want = total - offset;
        size_t i;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        want &= ~((size_t)3U);
        if (read_at(fp, offset, chunk, want) != 0) {
            return -1;
        }
        for (i = 0U; i + 4U <= want; i += 4U) {
            if (read_u32(chunk + i) != PATCH_WAITCNT_ADDRESS) {
                continue;
            }
            if (has_thumb_ldr_pc(fp, (offset + (uint32_t)i) / 4U, total) ||
                has_arm_ldr_pc(fp, (offset + (uint32_t)i) / 4U, total)) {
                static const unsigned char zero[4] = {0, 0, 0, 0};
                if (write_pattern(fp, total, offset + (uint32_t)i, zero, sizeof(zero)) != 0) {
                    return -1;
                }
                ++count;
            }
        }
        offset += (uint32_t)want;
    }
    if (count_out != NULL) {
        *count_out = count;
    }
    return 0;
}

static int copy_file(const char *input_path, const char *tmp_path, uint32_t *size_out)
{
    FILE *in = NULL;
    FILE *out = NULL;
    unsigned char *buffer = NULL;
    uint32_t total = 0U;
    size_t got;
    int result = -1;

    in = fopen(input_path, "rb");
    out = fopen(tmp_path, "wb");
    if (in == NULL || out == NULL) {
        goto done;
    }
    if (file_size(in, &total) != 0 || total == 0U) {
        goto done;
    }
    buffer = (unsigned char *)malloc(PATCH_SCAN_BYTES);
    if (buffer == NULL) {
        goto done;
    }
    while ((got = fread(buffer, 1U, PATCH_SCAN_BYTES, in)) > 0U) {
        if (fwrite(buffer, 1U, got, out) != got) {
            goto done;
        }
    }
    if (ferror(in) != 0 || fflush(out) != 0) {
        goto done;
    }
    if (size_out != NULL) {
        *size_out = total;
    }
    result = 0;
done:
    free(buffer);
    if (in != NULL) fclose(in);
    if (out != NULL) fclose(out);
    return result;
}

static int apply_sram_set(FILE *fp, uint32_t total, const sram_patch_set_t *set)
{
    uint32_t offsets[16] = {0};
    bool all_markers = true;
    bool all_replacements = true;
    size_t i;

    if (set == NULL || set->patch_count > sizeof(offsets) / sizeof(offsets[0])) {
        return -1;
    }
    for (i = 0U; i < set->patch_count; ++i) {
        int marker_result = find_pattern(
            fp,
            total,
            set->patches[i].marker,
            set->patches[i].marker_len,
            set->patches[i].marker_mask,
            set->patches[i].marker_mask_len,
            &offsets[i]);
        uint32_t replacement_offset = 0U;
        int replacement_result = find_pattern(
            fp,
            total,
            set->patches[i].replace,
            set->patches[i].replace_len,
            NULL,
            0U,
            &replacement_offset);
        if (marker_result != 0) all_markers = false;
        if (replacement_result != 0) all_replacements = false;
    }
    if (!all_markers && all_replacements) {
        return 2; /* already patched */
    }
    if (!all_markers) {
        return 1;
    }
    for (i = 0U; i < set->patch_count; ++i) {
        if (write_pattern(fp, total, offsets[i], set->patches[i].replace, set->patches[i].replace_len) != 0) {
            return -1;
        }
    }
    return 0;
}

bool burner_gba_rom_has_sram_patch_target(const char *input_path)
{
    FILE *fp;
    uint32_t total = 0U;
    bool found[sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0])] = {0};
    bool any_found = false;

    if (input_path == NULL) {
        return false;
    }
    fp = fopen(input_path, "rb");
    if (fp == NULL || file_size(fp, &total) != 0) {
        if (fp != NULL) fclose(fp);
        return false;
    }
    (void)scan_patch_identifiers(fp, total, found);
    fclose(fp);
    for (size_t i = 0U; i < sizeof(found) / sizeof(found[0]); ++i) {
        if (found[i]) {
            any_found = true;
            break;
        }
    }
    return any_found;
}

bool burner_gba_rom_has_batteryless_patch_target(const char *input_path)
{
    FILE *fp;
    uint32_t total = 0U;
    unsigned char *buffer = NULL;
    size_t marker_len = sizeof(BATTERYLESS_MARKER) - 1U;
    bool found = false;

    if (input_path == NULL) {
        return false;
    }
    fp = fopen(input_path, "rb");
    if (fp == NULL || file_size(fp, &total) != 0 || total < marker_len) {
        if (fp != NULL) fclose(fp);
        return false;
    }
    buffer = (unsigned char *)malloc(total);
    if (buffer != NULL && fread(buffer, 1U, total, fp) == total) {
        for (size_t i = 0U; i + marker_len <= total; ++i) {
            if (memcmp(buffer + i, BATTERYLESS_MARKER, marker_len) == 0) {
                found = true;
                break;
            }
        }
    }
    free(buffer);
    fclose(fp);
    return found;
}

static uint32_t patch_read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void patch_write_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int apply_batteryless_patch(FILE *fp, uint32_t *total_io, uint32_t *save_size_out, char *error_msg, size_t error_msg_len)
{
    static const unsigned char old_irq[] = {0xFC, 0x7F, 0x00, 0x03};
    static const unsigned char new_irq[] = {0xF4, 0x7F, 0x00, 0x03};
    static const unsigned char write_sram[] = {0x30,0xB5,0x05,0x1C,0x0C,0x1C,0x13,0x1C,0x0B,0x4A,0x10,0x88,0x0B,0x49,0x08,0x40};
    static const unsigned char write_eeprom[] = {0x70,0xB5,0x00,0x04,0x0A,0x1C,0x40,0x0B,0xE0,0x21,0x09,0x05,0x41,0x18,0x07,0x31,0x00,0x23,0x10,0x78};
    static const unsigned char write_flash[] = {0x70,0xB5,0x00,0x03,0x0A,0x1C,0xE0,0x21,0x09,0x05,0x41,0x18,0x01,0x23,0x1B,0x03};
    static const unsigned char thumb_thunk[] = {0x00,0x4B,0x18,0x47};
    FILE *payload_fp = NULL;
    unsigned char *rom = NULL;
    unsigned char *payload = NULL;
    uint32_t total;
    uint32_t rom_size;
    uint32_t payload_len;
    uint32_t payload_base = 0;
    bool found_space = false;
    bool found_irq = false;
    bool found_sram = false;

    if (fp == NULL || total_io == NULL) return -1;
    total = *total_io;
    payload_fp = fopen("/assets/bl_payload.bin", "rb");
    if (payload_fp == NULL) payload_fp = fopen("assets/bl_payload.bin", "rb");
    if (payload_fp == NULL || file_size(payload_fp, &payload_len) != 0 || payload_len < 32U) {
        set_error(error_msg, error_msg_len, "batteryless payload is unavailable");
        if (payload_fp != NULL) fclose(payload_fp);
        return -1;
    }
    payload = (unsigned char *)malloc(payload_len);
    rom_size = (total + 0x3FFFFU) & ~0x3FFFFU;
    if (rom_size < BATTERYLESS_MIN_ROM) rom_size = BATTERYLESS_MIN_ROM;
    /* The batteryless patch needs a full-ROM working copy. Keep it in PSRAM so
     * combining it with SRAM and waitcnt patches does not exhaust internal RAM. */
    rom = (unsigned char *)heap_caps_malloc(rom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL || rom == NULL) {
        set_error(error_msg, error_msg_len, "not enough memory for batteryless patch");
        goto fail;
    }
    if (fread(payload, 1U, payload_len, payload_fp) != payload_len || fseek(fp, 0L, SEEK_SET) != 0 || fread(rom, 1U, total, fp) != total) {
        set_error(error_msg, error_msg_len, "batteryless patch read failed");
        goto fail;
    }
    for (uint32_t i = 0U; i + (sizeof(BATTERYLESS_MARKER) - 1U) <= total; ++i) {
        if (memcmp(rom + i, BATTERYLESS_MARKER, sizeof(BATTERYLESS_MARKER) - 1U) == 0) {
            fclose(payload_fp);
            free(payload);
            free(rom);
            *total_io = total;
            if (save_size_out != NULL) *save_size_out = 0x8000U;
            return 0;
        }
    }
    memset(rom + total, 0xFF, rom_size - total);
    for (int64_t candidate = (int64_t)rom_size - 0x40000 - payload_len; candidate >= 0; candidate -= 0x40000) {
        bool blank = true;
        for (uint32_t i = 0; i < 0x40000U + payload_len; ++i) {
            if (rom[(uint32_t)candidate + i] != 0xFFU && rom[(uint32_t)candidate + i] != 0x00U) { blank = false; break; }
        }
        if (blank) { payload_base = (uint32_t)candidate; found_space = true; break; }
    }
    if (!found_space || rom_size < 4U || rom[3] != 0xEAU) {
        set_error(error_msg, error_msg_len, "ROM has no batteryless payload space or unsupported entrypoint");
        goto fail;
    }
    memcpy(rom + payload_base, payload, payload_len);
    patch_write_le32(rom + payload_base + 8U, 0x8000U);
    patch_write_le32(rom + payload_base, 0x08000000U + 8U + ((patch_read_le32(rom) & 0x00FFFFFFU) << 2));
    patch_write_le32(rom, 0xEA000000U | (((0x08000000U + payload_base + patch_read_le32(payload + 12U)) - 0x08000008U) >> 2));
    for (uint32_t i = 0; i + sizeof(old_irq) <= rom_size; i += 4U) {
        if (memcmp(rom + i, old_irq, sizeof(old_irq)) == 0) { memcpy(rom + i, new_irq, sizeof(new_irq)); found_irq = true; }
    }
    for (uint32_t i = 0; i + sizeof(write_sram) <= total; i += 2U) {
        if (memcmp(rom + i, write_sram, sizeof(write_sram)) == 0) {
            memcpy(rom + i, thumb_thunk, sizeof(thumb_thunk));
            patch_write_le32(rom + i + 4U, 0x08000000U + payload_base + patch_read_le32(payload + 16U));
            found_sram = true;
            break;
        }
    }
    if (!found_sram) {
        for (uint32_t i = 0U; i + sizeof(write_eeprom) <= total; i += 2U) {
            if (memcmp(rom + i, write_eeprom, sizeof(write_eeprom)) == 0) {
                memcpy(rom + i, thumb_thunk, sizeof(thumb_thunk));
                patch_write_le32(rom + i + 4U, 0x08000000U + payload_base + patch_read_le32(payload + 20U));
                patch_write_le32(rom + payload_base + 8U, 0x2000U);
                found_sram = true;
                break;
            }
        }
    }
    if (!found_sram) {
        for (uint32_t i = 0U; i + sizeof(write_flash) <= total; i += 2U) {
            if (memcmp(rom + i, write_flash, sizeof(write_flash)) == 0) {
                memcpy(rom + i, thumb_thunk, sizeof(thumb_thunk));
                patch_write_le32(rom + i + 4U, 0x08000000U + payload_base + patch_read_le32(payload + 24U));
                patch_write_le32(rom + payload_base + 8U, 0x10000U);
                found_sram = true;
                break;
            }
        }
    }
    if (!found_irq || !found_sram) {
        set_error(error_msg, error_msg_len, "ROM save hook is unsupported for batteryless patch");
        goto fail;
    }
    if (fclose(payload_fp) != 0) {
        payload_fp = NULL;
        set_error(error_msg, error_msg_len, "batteryless payload close failed");
        goto fail;
    }
    payload_fp = NULL;
    if (fseek(fp, 0L, SEEK_SET) != 0 ||
        ftruncate(fileno(fp), (off_t)rom_size) != 0 ||
        fwrite(rom, 1U, rom_size, fp) != rom_size || fflush(fp) != 0) {
        set_error(error_msg, error_msg_len, "batteryless patch write failed");
        goto fail;
    }
    *total_io = rom_size;
    if (save_size_out != NULL) *save_size_out = 0x8000U;
    free(payload); free(rom);
    return 0;
fail:
    if (payload_fp != NULL) fclose(payload_fp);
    free(payload); free(rom);
    return -1;
}

int burner_prepare_gba_patch_file(
    const char *input_path,
    bool apply_sram_patch,
    bool apply_waitcnt_patch,
    bool apply_batteryless,
    char *output_path,
    size_t output_path_len,
    burner_gba_patch_report_t *report,
    char *error_msg,
    size_t error_msg_len)
{
    char tmp_path[BURNER_FILE_PATH_LEN + 32U] = {0};
    char patched_path[BURNER_FILE_PATH_LEN + 32U] = {0};
    FILE *fp = NULL;
    FILE *input_fp = NULL;
    uint32_t total = 0U;
    bool did_sram = false;
    bool did_batteryless = false;
    bool did_normalize = false;
    uint32_t waitcnt_count = 0U;
    int selected_set = -1;
    size_t i;

    if (report != NULL) memset(report, 0, sizeof(*report));
    if (input_path == NULL || output_path == NULL || output_path_len == 0U) {
        set_error(error_msg, error_msg_len, "invalid patch input");
        return ESP_ERR_INVALID_ARG;
    }
    if (apply_sram_patch && apply_batteryless) {
        set_error(error_msg, error_msg_len, "sram and batteryless patches cannot be combined");
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.patching", input_path) >= (int)sizeof(tmp_path) ||
        snprintf(patched_path, sizeof(patched_path), "%s.patched.gba", input_path) >= (int)sizeof(patched_path)) {
        set_error(error_msg, error_msg_len, "patch path too long");
        return ESP_ERR_INVALID_SIZE;
    }
    input_fp = fopen(input_path, "rb");
    if (input_fp == NULL || file_size(input_fp, &total) != 0 || total == 0U) {
        if (input_fp != NULL) fclose(input_fp);
        set_error(error_msg, error_msg_len, "open rom for patch scan failed");
        return ESP_FAIL;
    }
    if (!apply_sram_patch && !apply_waitcnt_patch && !apply_batteryless && (total & 1U) == 0U) {
        fclose(input_fp);
        input_fp = NULL;
        snprintf(output_path, output_path_len, "%s", input_path);
        if (report != NULL) report->output_size = total;
        return ESP_OK;
    }
    if (apply_sram_patch && !apply_waitcnt_patch && !apply_batteryless) {
        bool has_sram_identifier = false;
        for (i = 0U; i < sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0]); ++i) {
            uint32_t identifier_offset = 0U;
            if (find_pattern(
                    input_fp,
                    total,
                    s_generated_patch_sets[i].identifier,
                    s_generated_patch_sets[i].identifier_len,
                    NULL,
                    0U,
                    &identifier_offset) == 0) {
                has_sram_identifier = true;
                break;
            }
        }
        fclose(input_fp);
        input_fp = NULL;
        if (!has_sram_identifier && (total & 1U) == 0U) {
            snprintf(output_path, output_path_len, "%s", input_path);
            if (report != NULL) report->output_size = total;
            return ESP_OK;
        }
    } else {
        fclose(input_fp);
        input_fp = NULL;
    }
    if (copy_file(input_path, tmp_path, &total) != 0) {
        unlink(tmp_path);
        set_error(error_msg, error_msg_len, "copy rom for patch failed");
        return ESP_FAIL;
    }
    fp = fopen(tmp_path, "r+b");
    if (fp == NULL || file_size(fp, &total) != 0) {
        if (fp != NULL) fclose(fp);
        unlink(tmp_path);
        set_error(error_msg, error_msg_len, "open patch copy failed");
        return ESP_FAIL;
    }

    if (apply_batteryless) {
        uint32_t batteryless_size = 0U;
        if (apply_batteryless_patch(fp, &total, &batteryless_size, error_msg, error_msg_len) != 0) {
            fclose(fp);
            unlink(tmp_path);
            return ESP_ERR_NOT_SUPPORTED;
        }
        did_batteryless = true;
        if (report != NULL) report->batteryless_save_size = batteryless_size;
    }

    for (i = 0U; apply_sram_patch && i < sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0]); ++i) {
        uint32_t identifier_offset = 0U;
        if (find_pattern(
                fp,
                total,
                s_generated_patch_sets[i].identifier,
                s_generated_patch_sets[i].identifier_len,
                NULL,
                0U,
                &identifier_offset) == 0) {
            selected_set = (int)i;
            break;
        }
    }
    if (selected_set >= 0) {
        int patch_result = apply_sram_set(fp, total, &s_generated_patch_sets[selected_set]);
        if (patch_result < 0) {
            set_error(error_msg, error_msg_len, "sram patch write failed");
            fclose(fp);
            unlink(tmp_path);
            return ESP_FAIL;
        }
        if (patch_result == 1) {
            set_error(error_msg, error_msg_len, "sram patch patterns incomplete");
            fclose(fp);
            unlink(tmp_path);
            return ESP_ERR_NOT_SUPPORTED;
        }
        did_sram = patch_result == 0;
        if (report != NULL) snprintf(report->patch_name, sizeof(report->patch_name), "%s", s_generated_patch_sets[selected_set].name);
    }
    if ((total & 1U) != 0U) {
        static const unsigned char padding = 0U;
        if (total == UINT32_MAX) {
            set_error(error_msg, error_msg_len, "rom file too large");
            fclose(fp);
            unlink(tmp_path);
            return ESP_ERR_INVALID_SIZE;
        }
        if (fseek(fp, 0L, SEEK_END) != 0 || fwrite(&padding, 1U, 1U, fp) != 1U) {
            set_error(error_msg, error_msg_len, "append gba padding to patch copy failed");
            fclose(fp);
            unlink(tmp_path);
            return ESP_FAIL;
        }
        ++total;
        did_normalize = true;
    }
    if (apply_waitcnt_patch && apply_waitcnt(fp, total, &waitcnt_count) != 0) {
        set_error(error_msg, error_msg_len, "waitcnt patch failed");
        fclose(fp);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    if (fflush(fp) != 0 || fclose(fp) != 0) {
        unlink(tmp_path);
        set_error(error_msg, error_msg_len, "flush patch copy failed");
        return ESP_FAIL;
    }
    fp = NULL;

    if (!did_sram && !did_batteryless && waitcnt_count == 0U && !did_normalize) {
        unlink(tmp_path);
        snprintf(output_path, output_path_len, "%s", input_path);
        if (report != NULL) {
            report->output_size = total;
            report->waitcnt_count = 0U;
        }
        return ESP_OK;
    }
    unlink(patched_path);
    if (rename(tmp_path, patched_path) != 0) {
        unlink(tmp_path);
        set_error(error_msg, error_msg_len, "commit patched rom failed");
        return ESP_FAIL;
    }
    if (strlen(patched_path) + 1U > output_path_len) {
        unlink(patched_path);
        set_error(error_msg, error_msg_len, "patched path buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(output_path, output_path_len, "%s", patched_path);
    if (report != NULL) {
        report->created_copy = true;
        report->sram_patched = did_sram;
        report->batteryless_patched = did_batteryless;
        report->waitcnt_patched = waitcnt_count > 0U;
        report->waitcnt_count = waitcnt_count;
        report->output_size = total;
    }
    return ESP_OK;
}
