#include "burner_gba_patch.h"
#include "burner_file_io.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

#define PATCH_SCAN_BYTES (32U * 1024U)
#define PATCH_ANALYSIS_CHUNK_BYTES PATCH_SCAN_BYTES
#define PATCH_WAITCNT_ADDRESS 0x04000204U
#define BATTERYLESS_MIN_ROM (0x400000U)
#define BATTERYLESS_MAX_ROM (32U * 1024U * 1024U)
#define BATTERYLESS_SAVE_RESERVE 0x40000U
#define BATTERYLESS_SCAN_CHUNK_BYTES (64U * 1024U)

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


/* Snapshot updates never hold a lock across TF I/O or UI callbacks. */
static portMUX_TYPE s_patch_debug_lock = portMUX_INITIALIZER_UNLOCKED;
static burner_gba_patch_debug_t s_patch_debug;
static TaskHandle_t s_patch_debug_owner;
static int64_t s_patch_debug_started;

void burner_gba_patch_debug_snapshot(burner_gba_patch_debug_t *out)
{
    if (out == NULL) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_patch_debug_lock);
    *out = s_patch_debug;
    if (out->running) out->elapsed_ms = (now - s_patch_debug_started) / 1000;
    portEXIT_CRITICAL(&s_patch_debug_lock);
}

bool burner_gba_patch_debug_cancel(void)
{
    portENTER_CRITICAL(&s_patch_debug_lock);
    bool running = s_patch_debug.running;
    if (running) s_patch_debug.cancel_requested = true;
    portEXIT_CRITICAL(&s_patch_debug_lock);
    return running;
}

static void patch_debug_phase(const char *phase, const char *detail)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_patch_debug_lock);
    if (s_patch_debug_owner == current) {
        snprintf(s_patch_debug.phase, sizeof(s_patch_debug.phase), "%s", phase);
        snprintf(s_patch_debug.detail, sizeof(s_patch_debug.detail), "%s", detail ? detail : "");
        s_patch_debug.offset = 0;
        ++s_patch_debug.passes;
    }
    portEXIT_CRITICAL(&s_patch_debug_lock);
}

static size_t patch_debug_read(void *buffer, size_t size, size_t count, FILE *fp)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_patch_debug_lock);
    bool tracked = s_patch_debug_owner == current;
    bool cancelled = tracked && s_patch_debug.cancel_requested;
    portEXIT_CRITICAL(&s_patch_debug_lock);
    if (cancelled) return 0;
    long position = tracked ? ftell(fp) : 0;
    int64_t started = tracked ? esp_timer_get_time() : 0;
    size_t got = burner_file_read(buffer, size, count, fp);
    int64_t elapsed = tracked ? esp_timer_get_time() - started : 0;
    if (tracked) {
        portENTER_CRITICAL(&s_patch_debug_lock);
        s_patch_debug.read_bytes += got * size;
        s_patch_debug.read_us += elapsed;
        s_patch_debug.offset = position < 0 ? 0 : (uint32_t)(position + got * size);
        portEXIT_CRITICAL(&s_patch_debug_lock);
    }
    return got;
}

static uint32_t patch_read_le32(const unsigned char *p);
static void patch_write_le32(unsigned char *p, uint32_t v);

/* Append without moving or overwriting source bytes. Keep the save area on
 * the same 256 KiB boundary as the existing in-ROM placement algorithm. */
static bool batteryless_append_layout(uint32_t source_size, uint32_t payload_len,
    uint32_t *rom_size_out, uint32_t *payload_base_out)
{
    if (source_size > BATTERYLESS_MAX_ROM || payload_len == 0 ||
        payload_len > BURNER_GBA_PATCH_MAX_PAYLOAD) return false;
    uint64_t required = (uint64_t)source_size + payload_len + BATTERYLESS_SAVE_RESERVE;
    uint64_t expanded = (required + BATTERYLESS_SAVE_RESERVE - 1U) &
                        ~((uint64_t)BATTERYLESS_SAVE_RESERVE - 1U);
    if (expanded < BATTERYLESS_MIN_ROM) expanded = BATTERYLESS_MIN_ROM;
    if (expanded > BATTERYLESS_MAX_ROM) return false;
    *rom_size_out = (uint32_t)expanded;
    *payload_base_out = (uint32_t)expanded - BATTERYLESS_SAVE_RESERVE - payload_len;
    return true;
}

static int read_plan_chunk(
    FILE *fp,
    uint32_t total,
    const burner_gba_patch_plan_t *plan,
    uint32_t offset,
    unsigned char *buffer,
    size_t length)
{
    size_t available = 0U;

    if (fp == NULL || buffer == NULL || offset > total) {
        return -1;
    }
    if (length > (size_t)(total - offset)) {
        available = (size_t)(total - offset);
    } else {
        available = length;
    }
    if (available > 0U) {
        if (fseek(fp, (long)offset, SEEK_SET) != 0 || patch_debug_read(buffer, 1U, available, fp) != available) {
            return -1;
        }
        if (plan != NULL) {
            burner_apply_gba_patch_plan(buffer, available, offset, plan);
        }
    }
    if (available < length) {
        memset(buffer + available, 0xFF, length - available);
    }
    return 0;
}

static void patch_scan_progress(burner_gba_patch_progress_cb_t cb, void *ctx,
    burner_gba_patch_progress_kind_t kind, uint32_t done, uint32_t total,
    int begin, int end, const char *message)
{
    if (cb == NULL || total == 0) return;
    int progress = begin + (int)((uint64_t)done * (unsigned)(end - begin) / total);
    if (progress > end) progress = end;
    cb(kind, progress, message, ctx);
}

typedef struct {
    const unsigned char *bytes;
    size_t length;
    uint32_t stride;
} plan_scan_pattern_t;

/* Collect each hook's first occurrence in one patched-ROM pass. The caller
 * still selects in signature-table order, independent of file order. */
static int stream_find_plan_patterns(FILE *fp, uint32_t total,
    const burner_gba_patch_plan_t *plan, const plan_scan_pattern_t *patterns,
    size_t pattern_count, uint32_t *offsets,
    burner_gba_patch_progress_cb_t progress_cb, void *progress_ctx)
{
    size_t max_len = 0, carry = 0;
    uint32_t base = 0;
    for (size_t index = 0; index < pattern_count; ++index) {
        offsets[index] = UINT32_MAX;
        if (patterns[index].length > max_len) max_len = patterns[index].length;
    }
    if (max_len == 0 || fp == NULL) return -1;
    unsigned char *buffer = malloc(BATTERYLESS_SCAN_CHUNK_BYTES + max_len - 1);
    if (buffer == NULL) return -2;
    if (fseek(fp, 0, SEEK_SET) != 0) { free(buffer); return -1; }
    while (base < total) {
        size_t want = total - base;
        if (want > BATTERYLESS_SCAN_CHUNK_BYTES) want = BATTERYLESS_SCAN_CHUNK_BYTES;
        size_t got = patch_debug_read(buffer + carry, 1, want, fp);
        if (got != want) { free(buffer); return -1; }
        if (plan != NULL) burner_apply_gba_patch_plan(buffer + carry, got, base, plan);
        size_t span = carry + got;
        for (size_t index = 0; index < pattern_count; ++index) {
            const plan_scan_pattern_t *pattern = &patterns[index];
            if (offsets[index] != UINT32_MAX || pattern->length > span || pattern->length == 0) continue;
            size_t start = 0;
            while (start + pattern->length <= span) {
                const unsigned char *hit = memchr(buffer + start, pattern->bytes[0], span - pattern->length - start + 1);
                if (hit == NULL) break;
                uint32_t position = base - (uint32_t)carry + (uint32_t)(hit - buffer);
                if ((pattern->stride <= 1 || position % pattern->stride == 0) &&
                    memcmp(hit, pattern->bytes, pattern->length) == 0) {
                    offsets[index] = position;
                    break;
                }
                start = (size_t)(hit - buffer) + 1;
            }
        }
        base += (uint32_t)got;
        patch_scan_progress(progress_cb, progress_ctx, BURNER_GBA_PATCH_PROGRESS_BATTERYLESS,
                            base, total, 45, 95, "finding save hooks");
        if (offsets[0] != UINT32_MAX) break;
        carry = span < max_len - 1 ? span : max_len - 1;
        memmove(buffer, buffer + span - carry, carry);
        if ((base & 0x3FFFFU) == 0) vTaskDelay(1);
    }
    free(buffer);
    return 0;
}

static int stream_collect_plan_pattern(
    FILE *fp,
    uint32_t total,
    const burner_gba_patch_plan_t *plan,
    const unsigned char *pattern,
    size_t pattern_len,
    uint32_t stride,
    uint32_t *offsets,
    size_t max_offsets,
    size_t *count_out,
    burner_gba_patch_progress_cb_t progress_cb, void *progress_ctx)
{
    unsigned char *buffer;
    size_t carry = 0U;
    size_t count = 0U;
    uint32_t base = 0U;

    if (fp == NULL || pattern == NULL || pattern_len == 0U || offsets == NULL || count_out == NULL) return -1;
    *count_out = 0U;
    buffer = (unsigned char *)malloc(BATTERYLESS_SCAN_CHUNK_BYTES + pattern_len - 1U);
    if (buffer == NULL) return -2;
    if (fseek(fp, 0L, SEEK_SET) != 0) { free(buffer); return -1; }
    while (base < total) {
        size_t want = total - base;
        size_t got;
        size_t span;
        if (want > BATTERYLESS_SCAN_CHUNK_BYTES) want = BATTERYLESS_SCAN_CHUNK_BYTES;
        got = patch_debug_read(buffer + carry, 1U, want, fp);
        if (got != want) { free(buffer); return -1; }
        if (plan != NULL) burner_apply_gba_patch_plan(buffer + carry, got, base, plan);
        patch_scan_progress(progress_cb, progress_ctx, BURNER_GBA_PATCH_PROGRESS_BATTERYLESS,
                            base + (uint32_t)got, total, 10, 45, "scanning IRQ literals");
        span = carry + got;
        for (size_t i = 0U; i + pattern_len <= span; ++i) {
            uint32_t position = base - (uint32_t)carry + (uint32_t)i;
            if (stride > 1U && (position % stride) != 0U) continue;
            if (memcmp(buffer + i, pattern, pattern_len) == 0) {
                if (count >= max_offsets) { free(buffer); return -3; }
                offsets[count++] = position;
            }
        }
        if (pattern_len > 1U) {
            size_t keep = span;
            if (keep > pattern_len - 1U) keep = pattern_len - 1U;
            memmove(buffer, buffer + span - keep, keep);
            carry = keep;
        } else carry = 0U;
        base += (uint32_t)got;
    }
    free(buffer);
    *count_out = count;
    return 0;
}

static bool stream_region_blank(
    FILE *fp,
    uint32_t total,
    const burner_gba_patch_plan_t *plan,
    uint32_t offset,
    uint32_t length)
{
    unsigned char *buffer = (unsigned char *)malloc(BATTERYLESS_SCAN_CHUNK_BYTES);
    bool blank = true;

    if (buffer == NULL) return false;
    while (length > 0U) {
        size_t chunk = length > BATTERYLESS_SCAN_CHUNK_BYTES ? BATTERYLESS_SCAN_CHUNK_BYTES : length;
        if (read_plan_chunk(fp, total, plan, offset, buffer, chunk) != 0) { blank = false; break; }
        for (size_t i = 0U; i < chunk; ++i) {
            if (buffer[i] != 0xFFU && buffer[i] != 0x00U) { blank = false; break; }
        }
        if (!blank) break;
        offset += (uint32_t)chunk;
        length -= (uint32_t)chunk;
    }
    free(buffer);
    return blank;
}

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
    uint32_t *offset_out,
    burner_gba_patch_progress_cb_t progress_cb,
    burner_gba_patch_progress_kind_t progress_kind,
    int progress_begin,
    int progress_end)
{
    unsigned char *buffer;
    size_t carry = 0U;
    uint32_t base = 0U;

    if (fp == NULL || pattern == NULL || pattern_len == 0U || offset_out == NULL) {
        return -1;
    }
    if (total < pattern_len) return 1;
    buffer = (unsigned char *)malloc(PATCH_SCAN_BYTES + pattern_len - 1U);
    if (buffer == NULL) {
        return -2;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        free(buffer);
        return -1;
    }

    bool masked = mask != NULL && mask_len == pattern_len;
    size_t anchor = 0;
    while (masked && anchor < pattern_len && mask[anchor] != 0) ++anchor;
    while (base < total) {
        size_t want = total - base;
        size_t got;
        size_t i;
        if (want > PATCH_SCAN_BYTES) {
            want = PATCH_SCAN_BYTES;
        }
        got = patch_debug_read(buffer + carry, 1U, want, fp);
        if (got != want) {
            free(buffer);
            return -1;
        }
        if (progress_cb != NULL) {
            int progress = progress_begin + (int)(((uint64_t)(base + (uint32_t)got) * (uint64_t)(progress_end - progress_begin)) / total);
            if (progress > progress_end) progress = progress_end;
            progress_cb(progress_kind, progress, "scanning", NULL);
        }
        for (i = 0U; i + pattern_len <= carry + got; ++i) {
            if (anchor < pattern_len) {
                const unsigned char *hit = memchr(buffer + i + anchor, pattern[anchor],
                                                  carry + got - pattern_len - i + 1U);
                if (hit == NULL) break;
                i = (size_t)(hit - buffer) - anchor;
            }
            bool matched = !masked ? memcmp(buffer + i, pattern, pattern_len) == 0 : true;
            if (masked) {
                for (size_t pattern_index = 0; pattern_index < pattern_len; ++pattern_index) {
                    if (mask[pattern_index] == 0 && buffer[i + pattern_index] != pattern[pattern_index]) {
                        matched = false;
                        break;
                    }
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

/* One sequential TF pass, with overlap for identifiers crossing a block.
 * Full scans preserve the generated table's priority even if a lower-priority
 * identifier appears earlier in the file. Detection-only callers may stop early. */
static int scan_patch_identifiers(FILE *fp, uint32_t total, bool *found_out,
    bool scan_all, burner_gba_patch_progress_cb_t progress_cb, void *progress_ctx)
{
    const size_t set_count = sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0]);
    size_t max_len = 0, carry = 0;
    uint32_t offset = 0;
    bool any_found = false;
    unsigned char prefixes[sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0])];
    size_t prefix_count = 0;
    if (fp == NULL || found_out == NULL) return -1;
    for (size_t i = 0; i < set_count; ++i) {
        found_out[i] = false;
        unsigned char first = s_generated_patch_sets[i].identifier[0];
        if (memchr(prefixes, first, prefix_count) == NULL) prefixes[prefix_count++] = first;
        if (s_generated_patch_sets[i].identifier_len > max_len)
            max_len = s_generated_patch_sets[i].identifier_len;
    }
    if (max_len == 0 || total == 0) return 0;
    if (fseek(fp, 0, SEEK_SET) != 0) return -1;
    unsigned char *buffer = malloc(PATCH_ANALYSIS_CHUNK_BYTES + max_len - 1);
    if (buffer == NULL) return -2;
    while (offset < total && (scan_all || !any_found)) {
        size_t want = total - offset;
        if (want > PATCH_ANALYSIS_CHUNK_BYTES) want = PATCH_ANALYSIS_CHUNK_BYTES;
        size_t got = patch_debug_read(buffer + carry, 1, want, fp);
        if (got != want) { free(buffer); return -1; }
        size_t span = carry + got;
        for (size_t group = 0; group < prefix_count; ++group) {
            size_t start = 0;
            while (start < span) {
                const unsigned char *hit = memchr(buffer + start, prefixes[group], span - start);
                if (hit == NULL) break;
                size_t position = (size_t)(hit - buffer);
                for (size_t index = 0; index < set_count; ++index) {
                    const sram_patch_set_t *set = &s_generated_patch_sets[index];
                    if (found_out[index] || set->identifier[0] != prefixes[group] ||
                        set->identifier_len > span - position) continue;
                    if (memcmp(hit, set->identifier, set->identifier_len) == 0) {
                        found_out[index] = true;
                        any_found = true;
                    }
                }
                start = position + 1;
            }
        }
        offset += (uint32_t)got;
        if (progress_cb != NULL)
            progress_cb(BURNER_GBA_PATCH_PROGRESS_SRAM, (int)((uint64_t)offset * 35 / total), "identifying save", progress_ctx);
        if (found_out[0]) break; /* No higher-priority match can exist. */
        carry = span < max_len - 1 ? span : max_len - 1;
        memmove(buffer, buffer + span - carry, carry);
        if ((offset & 0x3FFFFU) == 0) vTaskDelay(1);
    }
    free(buffer);
    return 0;
}

static int read_at(FILE *fp, uint32_t offset, void *data, size_t len)
{
    if (fp == NULL || data == NULL) return -1;
    long position = ftell(fp);
    if (position < 0 || ((uint32_t)position != offset && fseek(fp, (long)offset, SEEK_SET) != 0)) return -1;
    return patch_debug_read(data, 1U, len, fp) == len ? 0 : -1;
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

static int collect_waitcnt_offsets(
    FILE *fp,
    uint32_t total,
    uint32_t *offsets,
    size_t capacity,
    size_t *count_out, burner_gba_patch_progress_cb_t progress_cb, void *progress_ctx)
{
    uint32_t offset = 0U;
    size_t count = 0U;

    if (count_out == NULL) {
        return -1;
    }
    unsigned char *chunk = malloc(PATCH_SCAN_BYTES);
    if (chunk == NULL) return -1;
    while (offset + 4U <= total) {
        size_t want = total - offset;
        size_t i;
        if (want > PATCH_SCAN_BYTES) {
            want = PATCH_SCAN_BYTES;
        }
        want &= ~((size_t)3U);
        if (read_at(fp, offset, chunk, want) != 0) {
            free(chunk);
            return -1;
        }
        for (i = 0U; i + 4U <= want; i += 4U) {
            if (read_u32(chunk + i) != PATCH_WAITCNT_ADDRESS) {
                continue;
            }
            if (has_thumb_ldr_pc(fp, (offset + (uint32_t)i) / 4U, total) ||
                has_arm_ldr_pc(fp, (offset + (uint32_t)i) / 4U, total)) {
                if (count >= capacity) {
                    free(chunk);
                    return -2;
                }
                offsets[count++] = offset + (uint32_t)i;
            }
        }
        offset += (uint32_t)want;
        patch_scan_progress(progress_cb, progress_ctx, BURNER_GBA_PATCH_PROGRESS_WAITCNT,
                            offset, total, 0, 99, "scanning literal references");
    }
    free(chunk);
    *count_out = count;
    return 0;
}

static int build_gba_patch_plan_impl(
    const char *input_path,
    bool apply_sram_patch,
    bool apply_waitcnt_patch,
    bool apply_batteryless,
    burner_gba_patch_plan_t *plan,
    burner_gba_patch_report_t *report,
    char *error_msg,
    size_t error_msg_len,
    burner_gba_patch_progress_cb_t progress_cb,
    void *progress_ctx)
{
    FILE *fp = NULL;
    uint32_t total = 0U;
    int selected_set = -1;
    size_t i;

    if (plan == NULL || input_path == NULL) {
        set_error(error_msg, error_msg_len, "invalid patch plan input");
        return ESP_ERR_INVALID_ARG;
    }
    memset(plan, 0, sizeof(*plan));
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    fp = fopen(input_path, "rb");
    if (fp == NULL || file_size(fp, &total) != 0 || total == 0U) {
        if (fp != NULL) fclose(fp);
        set_error(error_msg, error_msg_len, "open rom for patch plan failed");
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_patch_debug_lock);
    s_patch_debug.total = total;
    portEXIT_CRITICAL(&s_patch_debug_lock);
    plan->source_size = total;
    /* Batteryless placement may extend output; source_size remains the TF EOF. */
    plan->output_size = total;

    if (apply_batteryless && total > BATTERYLESS_MAX_ROM) {
        fclose(fp);
        set_error(error_msg, error_msg_len, "batteryless ROM exceeds 32 MiB address space");
        return ESP_ERR_INVALID_SIZE;
    }

    if (apply_sram_patch) {
        bool found[sizeof(s_generated_patch_sets) / sizeof(s_generated_patch_sets[0])] = {0};
        patch_debug_phase("sram_identifier", "all save types (one pass)");
        int scan_result = scan_patch_identifiers(fp, total, found, true, progress_cb, progress_ctx);
        if (scan_result != 0) {
            fclose(fp);
            set_error(error_msg, error_msg_len, "SRAM identifier scan failed");
            return scan_result == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
        for (i = 0; i < sizeof(found) / sizeof(found[0]); ++i) {
            if (found[i]) { selected_set = (int)i; break; }
        }
        if (selected_set >= 0) {
            const sram_patch_set_t *set = &s_generated_patch_sets[selected_set];
            if (set->patch_count > BURNER_GBA_PATCH_MAX_SRAM_OPS) {
                fclose(fp);
                set_error(error_msg, error_msg_len, "too many sram patch operations");
                return ESP_ERR_INVALID_SIZE;
            }
            for (i = 0U; i < set->patch_count; ++i) {
                uint32_t marker_offset = 0U;
                uint32_t replacement_offset = 0U;
                const sram_patch_t *patch = &set->patches[i];
                patch_debug_phase("sram_marker", patch->name);
                int marker_result = find_pattern(fp, total, patch->marker, patch->marker_len,
                    patch->marker_mask, patch->marker_mask_len, &marker_offset, progress_cb,
                    BURNER_GBA_PATCH_PROGRESS_SRAM,
                    (int)(35U + (i * 60U) / set->patch_count),
                    (int)(35U + (i * 60U + 30U) / set->patch_count));
                if (marker_result < 0) {
                    fclose(fp);
                    set_error(error_msg, error_msg_len, "SRAM marker scan failed");
                    return marker_result == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
                }
                if (marker_result != 0) {
                    /* Only look for an already-patched routine when the original
                     * is absent. A normal ROM needs no replacement-code scan. */
                    patch_debug_phase("sram_replacement", patch->name);
                    int replacement_result = find_pattern(fp, total, patch->replace, patch->replace_len,
                        NULL, 0U, &replacement_offset, progress_cb, BURNER_GBA_PATCH_PROGRESS_SRAM,
                        (int)(35U + (i * 60U + 30U) / set->patch_count),
                        (int)(35U + ((i + 1U) * 60U) / set->patch_count));
                    if (replacement_result == 0) continue;
                    fclose(fp);
                    set_error(error_msg, error_msg_len, replacement_result < 0 ?
                        "SRAM replacement scan failed" : "sram patch patterns incomplete");
                    return replacement_result < 0 ? ESP_FAIL : ESP_ERR_NOT_SUPPORTED;
                }
                if (patch->replace_len > BURNER_GBA_PATCH_MAX_REPLACEMENT) {
                    fclose(fp);
                    set_error(error_msg, error_msg_len, "sram replacement is too large");
                    return ESP_ERR_INVALID_SIZE;
                }
                plan->sram[plan->sram_count].offset = marker_offset;
                plan->sram[plan->sram_count].length = (uint16_t)patch->replace_len;
                memcpy(plan->sram[plan->sram_count].data, patch->replace, patch->replace_len);
                plan->sram_count++;
            }
            if (report != NULL) {
                report->sram_patched = plan->sram_count != 0U;
                snprintf(report->patch_name, sizeof(report->patch_name), "%s", set->name);
            }
        }
    }

    if (apply_sram_patch && progress_cb != NULL)
        progress_cb(BURNER_GBA_PATCH_PROGRESS_SRAM, 100, "planned", progress_ctx);

    if (apply_batteryless) {
        static const unsigned char old_irq[] = {0xFC, 0x7F, 0x00, 0x03};
        static const unsigned char write_sram[] = {0x30,0xB5,0x05,0x1C,0x0C,0x1C,0x13,0x1C,0x0B,0x4A,0x10,0x88,0x0B,0x49,0x08,0x40};
        static const unsigned char write_sram2[] = {0x80,0xB5,0x83,0xB0,0x6F,0x46,0x38,0x60,0x79,0x60,0xBA,0x60,0x09,0x48,0x09,0x49};
        static const unsigned char write_sram_arm[] = {0x04,0xC0,0x90,0xE4,0x01,0xC0,0xC1,0xE4,0x2C,0xC4,0xA0,0xE1,0x01,0xC0,0xC1,0xE4};
        static const unsigned char write_eeprom[] = {0x70,0xB5,0x00,0x04,0x0A,0x1C,0x40,0x0B,0xE0,0x21,0x09,0x05,0x41,0x18,0x07,0x31,0x00,0x23,0x10,0x78};
        static const unsigned char write_flash[] = {0x70,0xB5,0x00,0x03,0x0A,0x1C,0xE0,0x21,0x09,0x05,0x41,0x18,0x01,0x23,0x1B,0x03};
        static const unsigned char write_flash2[] = {0x7C,0xB5,0x90,0xB0,0x00,0x03,0x0A,0x1C,0xE0,0x21,0x09,0x05,0x09,0x18,0x01,0x23};
        static const unsigned char write_flash3[] = {0xF0,0xB5,0x90,0xB0,0x0F,0x1C,0x00,0x04,0x04,0x0C,0x03,0x48,0x00,0x68,0x40,0x89};
        static const unsigned char write_eeprom_v111[] = {0x0A,0x88,0x80,0x21,0x09,0x06,0x0A,0x43,0x02,0x60,0x07,0x48,0x00,0x47,0x00,0x00};
        static const unsigned char thumb_thunk[] = {0x00,0x4B,0x18,0x47};
        static const unsigned char arm_thunk[] = {0x00,0x30,0x9F,0xE5,0x13,0xFF,0x2F,0xE1};
        static const unsigned char eeprom_v111_thunk[] = {0x07,0x49,0x08,0x47};
        FILE *payload_fp = NULL;
        uint32_t payload_len = 0U;
        uint32_t rom_size = (total + 0x3FFFFU) & ~0x3FFFFU;
        uint32_t payload_base = 0U;
        bool found_space = false;
        bool found_hook = false;

        if (rom_size < BATTERYLESS_MIN_ROM) rom_size = BATTERYLESS_MIN_ROM;
        payload_fp = fopen("/assets/bl_payload.bin", "rb");
        if (payload_fp == NULL) payload_fp = fopen("assets/bl_payload.bin", "rb");
        if (payload_fp == NULL || file_size(payload_fp, &payload_len) != 0 ||
            payload_len < 32U || payload_len > BURNER_GBA_PATCH_MAX_PAYLOAD) {
            if (payload_fp != NULL) fclose(payload_fp);
            fclose(fp);
            set_error(error_msg, error_msg_len, "batteryless payload is unavailable");
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (patch_debug_read(plan->payload, 1U, payload_len, payload_fp) != payload_len) {
            fclose(payload_fp);
            fclose(fp);
            set_error(error_msg, error_msg_len, "batteryless payload read failed");
            return ESP_FAIL;
        }
        fclose(payload_fp);

        patch_debug_phase("batteryless_space", "payload and save reserve");
        if (progress_cb != NULL) progress_cb(BURNER_GBA_PATCH_PROGRESS_BATTERYLESS, 0, "finding blank space", progress_ctx);
        for (int64_t candidate = (int64_t)rom_size - 0x40000 - payload_len;
             candidate >= 0; candidate -= 0x40000) {
            uint32_t span = 0x40000U + payload_len;
            if (stream_region_blank(fp, total, plan, (uint32_t)candidate, span)) {
                payload_base = (uint32_t)candidate;
                found_space = true;
                break;
            }
        }
        if (!found_space) {
            found_space = batteryless_append_layout(total, payload_len, &rom_size, &payload_base);
            if (found_space) {
                patch_debug_phase("batteryless_expand", "append payload and save area");
                if (progress_cb != NULL)
                    progress_cb(BURNER_GBA_PATCH_PROGRESS_BATTERYLESS, 10, "expanding ROM", progress_ctx);
            }
        }
        {
            unsigned char entry_bytes[4] = {0};
            if (read_plan_chunk(fp, total, plan, 0U, entry_bytes, sizeof(entry_bytes)) != 0) {
                fclose(fp);
                set_error(error_msg, error_msg_len, "batteryless entrypoint read failed");
                return ESP_FAIL;
            }
            if (total < 4U || entry_bytes[3] != 0xEAU || !found_space) {
                fclose(fp);
                set_error(error_msg, error_msg_len, total < 4U || entry_bytes[3] != 0xEAU ?
                    "unsupported batteryless ROM entrypoint" : "batteryless payload and save cannot fit within 32 MiB");
                return ESP_ERR_NOT_SUPPORTED;
            }
            plan->payload_offset = payload_base;
            plan->payload_length = (uint16_t)payload_len;
            patch_write_le32(plan->payload + 8U, 0x8000U);
            patch_write_le32(plan->payload, 0x08000000U + 8U + ((patch_read_le32(entry_bytes) & 0x00FFFFFFU) << 2));
        }
        {
            burner_gba_patch_write_t *entry = &plan->batteryless_writes[plan->batteryless_write_count++];
            uint32_t branch = 0xEA000000U |
                (((0x08000000U + payload_base + patch_read_le32(plan->payload + 12U)) - 0x08000008U) >> 2);
            entry->offset = 0U;
            entry->length = 4U;
            patch_write_le32(entry->data, branch);
        }
        {
            patch_debug_phase("batteryless_irq", "IRQ literals");
            size_t irq_count = 0U;
            int scan_result = stream_collect_plan_pattern(
                fp, total, plan, old_irq, sizeof(old_irq), 4U,
                plan->batteryless_irq_offsets, BURNER_GBA_PATCH_MAX_IRQ_OPS, &irq_count, progress_cb, progress_ctx);
            if (scan_result == -3) {
                fclose(fp);
                set_error(error_msg, error_msg_len, "too many batteryless IRQ patches");
                return ESP_ERR_INVALID_SIZE;
            }
            if (scan_result != 0) {
                fclose(fp);
                set_error(error_msg, error_msg_len, "batteryless IRQ scan failed");
                return ESP_FAIL;
            }
            plan->batteryless_irq_count = irq_count;
        }
        const struct {
            const unsigned char *signature;
            size_t signature_len;
            uint32_t payload_offset;
            uint32_t save_size;
            bool arm;
        } hooks[] = {
            {write_sram, sizeof(write_sram), 16U, 0x8000U, false},
            {write_sram2, sizeof(write_sram2), 16U, 0x8000U, false},
            {write_sram_arm, sizeof(write_sram_arm), 16U, 0x8000U, true},
            {write_eeprom, sizeof(write_eeprom), 20U, 0x2000U, false},
            {write_flash, sizeof(write_flash), 24U, 0x10000U, false},
            {write_flash2, sizeof(write_flash2), 24U, 0x10000U, false},
            {write_flash3, sizeof(write_flash3), 24U, 0x20000U, false},
        };
        enum { HOOK_COUNT = sizeof(hooks) / sizeof(hooks[0]) };
        plan_scan_pattern_t hook_patterns[HOOK_COUNT + 1];
        uint32_t hook_offsets[HOOK_COUNT + 1];
        for (size_t index = 0; index < HOOK_COUNT; ++index) {
            hook_patterns[index] = (plan_scan_pattern_t){hooks[index].signature,
                hooks[index].signature_len, hooks[index].arm ? 4U : 2U};
        }
        hook_patterns[HOOK_COUNT] = (plan_scan_pattern_t){write_eeprom_v111, sizeof(write_eeprom_v111), 2U};
        patch_debug_phase("batteryless_hook", "all save hooks (one pass)");
        int hook_result = stream_find_plan_patterns(fp, total, plan, hook_patterns,
            HOOK_COUNT + 1, hook_offsets, progress_cb, progress_ctx);
        if (hook_result != 0) {
            fclose(fp);
            set_error(error_msg, error_msg_len, "batteryless hook scan failed");
            return hook_result == -2 ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
        for (size_t hook_index = 0U; hook_index < sizeof(hooks) / sizeof(hooks[0]) && !found_hook; ++hook_index) {
            uint32_t pos = hook_offsets[hook_index];
            if (pos != UINT32_MAX) {
                burner_gba_patch_write_t *hook = &plan->batteryless_writes[plan->batteryless_write_count++];
                const unsigned char *thunk = hooks[hook_index].arm ? arm_thunk : thumb_thunk;
                size_t thunk_len = hooks[hook_index].arm ? sizeof(arm_thunk) : sizeof(thumb_thunk);
                uint32_t target_offset = hooks[hook_index].arm ? 8U : 4U;
                hook->offset = pos;
                hook->length = (uint16_t)(target_offset + 4U);
                memcpy(hook->data, thunk, thunk_len);
                patch_write_le32(hook->data + target_offset,
                                 0x08000000U + payload_base + patch_read_le32(plan->payload + hooks[hook_index].payload_offset));
                plan->batteryless_save_size = hooks[hook_index].save_size;
                patch_write_le32(plan->payload + 8U, hooks[hook_index].save_size);
                found_hook = true;
            }
        }
        if (!found_hook) {
            uint32_t pos = hook_offsets[HOOK_COUNT];
            if (pos != UINT32_MAX) {
                burner_gba_patch_write_t *thunk = &plan->batteryless_writes[plan->batteryless_write_count++];
                burner_gba_patch_write_t *target = &plan->batteryless_writes[plan->batteryless_write_count++];
                thunk->offset = pos + 12U;
                thunk->length = sizeof(eeprom_v111_thunk);
                memcpy(thunk->data, eeprom_v111_thunk, sizeof(eeprom_v111_thunk));
                target->offset = pos + 44U;
                target->length = 4U;
                patch_write_le32(target->data, 0x08000000U + payload_base + patch_read_le32(plan->payload + 28U));
                plan->batteryless_save_size = 0x2000U;
                patch_write_le32(plan->payload + 8U, 0x2000U);
                found_hook = true;
            }
        }
        if (plan->batteryless_irq_count == 0U || !found_hook) {
            fclose(fp);
            set_error(error_msg, error_msg_len, "ROM save hook is unsupported for batteryless patch");
            return ESP_ERR_NOT_SUPPORTED;
        }
        plan->output_size = rom_size;
        if (report != NULL) {
            report->batteryless_patched = true;
            report->batteryless_save_size = plan->batteryless_save_size;
        }
    }

    if (apply_batteryless && progress_cb != NULL)
        progress_cb(BURNER_GBA_PATCH_PROGRESS_BATTERYLESS, 100, "planned", progress_ctx);

    if (apply_waitcnt_patch) {
        patch_debug_phase("waitcnt", "literal references");
        int result = collect_waitcnt_offsets(fp, total, plan->waitcnt_offsets,
                                             BURNER_GBA_PATCH_MAX_WAITCNT_OPS,
                                             &plan->waitcnt_count, progress_cb, progress_ctx);
        if (result != 0) {
            fclose(fp);
            set_error(error_msg, error_msg_len, result == -2 ? "too many waitcnt patch operations" : "waitcnt patch scan failed");
            return result == -2 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
        }
        if (report != NULL) {
            report->waitcnt_count = (uint32_t)plan->waitcnt_count;
            report->waitcnt_patched = plan->waitcnt_count != 0U;
        }
    }
    if (report != NULL) {
        report->output_size = plan->output_size;
    }
    fclose(fp);
    if (progress_cb != NULL) {
        if (apply_sram_patch) progress_cb(BURNER_GBA_PATCH_PROGRESS_SRAM, 100, "planned", progress_ctx);
        if (apply_waitcnt_patch) progress_cb(BURNER_GBA_PATCH_PROGRESS_WAITCNT, 100, "planned", progress_ctx);
        if (apply_batteryless) progress_cb(BURNER_GBA_PATCH_PROGRESS_BATTERYLESS, 100, "planned", progress_ctx);
    }
    return ESP_OK;
}


static bool patch_debug_begin(void)
{
    int64_t started = esp_timer_get_time();
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_patch_debug_lock);
    bool busy = s_patch_debug.running;
    if (!busy) {
        memset(&s_patch_debug, 0, sizeof(s_patch_debug));
        s_patch_debug.running = true;
        s_patch_debug_started = started;
        s_patch_debug_owner = current;
    }
    portEXIT_CRITICAL(&s_patch_debug_lock);
    return !busy;
}

static bool patch_debug_cancelled(void)
{
    portENTER_CRITICAL(&s_patch_debug_lock);
    bool cancelled = s_patch_debug.cancel_requested;
    portEXIT_CRITICAL(&s_patch_debug_lock);
    return cancelled;
}

static int patch_debug_finish(int result, char *error_msg, size_t error_msg_len)
{
    int64_t elapsed = (esp_timer_get_time() - s_patch_debug_started) / 1000;
    portENTER_CRITICAL(&s_patch_debug_lock);
    bool cancelled = s_patch_debug.cancel_requested;
    if (cancelled) result = ESP_ERR_INVALID_STATE;
    s_patch_debug.result = result;
    s_patch_debug.elapsed_ms = elapsed;
    s_patch_debug.running = false;
    s_patch_debug_owner = NULL;
    portEXIT_CRITICAL(&s_patch_debug_lock);
    if (cancelled) set_error(error_msg, error_msg_len, "patch analysis cancelled");
    return result;
}

int burner_build_gba_patch_plan(
    const char *input_path, bool apply_sram_patch, bool apply_waitcnt_patch,
    bool apply_batteryless, burner_gba_patch_plan_t *plan,
    burner_gba_patch_report_t *report, char *error_msg, size_t error_msg_len,
    burner_gba_patch_progress_cb_t progress_cb, void *progress_ctx)
{
    if (!patch_debug_begin()) {
        set_error(error_msg, error_msg_len, "another patch analysis is running");
        return ESP_ERR_INVALID_STATE;
    }
    patch_debug_phase("open", input_path);
    int result = build_gba_patch_plan_impl(input_path, apply_sram_patch, apply_waitcnt_patch,
        apply_batteryless, plan, report, error_msg, error_msg_len, progress_cb, progress_ctx);
    return patch_debug_finish(result, error_msg, error_msg_len);
}

void burner_apply_gba_patch_plan(
    uint8_t *buffer,
    size_t buffer_size,
    uint32_t base_offset,
    const burner_gba_patch_plan_t *plan)
{
    size_t i;
    if (buffer == NULL || plan == NULL) {
        return;
    }
    for (i = 0U; i < plan->sram_count; ++i) {
        const burner_gba_patch_write_t *op = &plan->sram[i];
        uint32_t end = op->offset + op->length;
        uint32_t window_end = base_offset + (uint32_t)buffer_size;
        if (end <= base_offset || op->offset >= window_end) continue;
        {
            uint32_t begin = op->offset > base_offset ? op->offset : base_offset;
            uint32_t copy_end = end < window_end ? end : window_end;
            memcpy(buffer + (begin - base_offset), op->data + (begin - op->offset), copy_end - begin);
        }
    }
    if (plan->payload_length != 0U) {
        uint32_t payload_end = plan->payload_offset + plan->payload_length;
        uint32_t window_end = base_offset + (uint32_t)buffer_size;
        if (plan->payload_offset < window_end && payload_end > base_offset) {
            uint32_t begin = plan->payload_offset > base_offset ? plan->payload_offset : base_offset;
            uint32_t end = payload_end < window_end ? payload_end : window_end;
            memcpy(buffer + begin - base_offset,
                   plan->payload + begin - plan->payload_offset,
                   end - begin);
        }
    }
    for (i = 0U; i < plan->batteryless_irq_count; ++i) {
        uint32_t offset = plan->batteryless_irq_offsets[i];
        if (offset >= base_offset && offset + 4U <= base_offset + buffer_size) {
            buffer[offset - base_offset + 0U] = 0xF4U;
            buffer[offset - base_offset + 1U] = 0x7FU;
            buffer[offset - base_offset + 2U] = 0x00U;
            buffer[offset - base_offset + 3U] = 0x03U;
        }
    }
    for (i = 0U; i < plan->batteryless_write_count; ++i) {
        const burner_gba_patch_write_t *op = &plan->batteryless_writes[i];
        uint32_t end = op->offset + op->length;
        uint32_t window_end = base_offset + (uint32_t)buffer_size;
        if (op->offset < window_end && end > base_offset) {
            uint32_t begin = op->offset > base_offset ? op->offset : base_offset;
            uint32_t copy_end = end < window_end ? end : window_end;
            memcpy(buffer + begin - base_offset,
                   op->data + begin - op->offset,
                   copy_end - begin);
        }
    }
    for (i = 0U; i < plan->waitcnt_count; ++i) {
        uint32_t offset = plan->waitcnt_offsets[i];
        if (offset >= base_offset && offset + 4U <= base_offset + buffer_size) {
            memset(buffer + (offset - base_offset), 0, 4U);
        }
    }
}

int burner_save_gba_patch_file(
    const char *input_path, bool apply_sram_patch, bool apply_waitcnt, bool apply_batteryless,
    char *output_path, size_t output_path_len, burner_gba_patch_report_t *report,
    char *error_msg, size_t error_msg_len, burner_gba_patch_progress_cb_t progress_cb,
    burner_gba_patch_save_progress_cb_t save_progress_cb, void *progress_ctx)
{
    FILE *in = NULL, *out = NULL;
    unsigned char *buffer = NULL;
    burner_gba_patch_plan_t *plan = NULL;
    char destination[BURNER_FILE_PATH_LEN] = {0};
    bool owns_output = false;
    int result = ESP_FAIL;
    if (output_path != NULL && output_path_len) output_path[0] = '\0';
    if (report != NULL) memset(report, 0, sizeof(*report));
    if (input_path == NULL || output_path == NULL || output_path_len == 0 ||
        (!apply_sram_patch && !apply_waitcnt && !apply_batteryless)) {
        set_error(error_msg, error_msg_len, "select at least one patch");
        return ESP_ERR_INVALID_ARG;
    }
    if (!patch_debug_begin()) {
        set_error(error_msg, error_msg_len, "another patch task is running");
        return ESP_ERR_INVALID_STATE;
    }
    plan = malloc(sizeof(*plan));
    buffer = malloc(PATCH_SCAN_BYTES);
    if (plan == NULL || buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        set_error(error_msg, error_msg_len, "no memory for patch export");
        goto done;
    }
    patch_debug_phase("open", input_path);
    result = build_gba_patch_plan_impl(input_path, apply_sram_patch, apply_waitcnt, apply_batteryless,
        plan, report, error_msg, error_msg_len, progress_cb, progress_ctx);
    if (result != ESP_OK || patch_debug_cancelled()) goto done;
    result = ESP_FAIL;
    in = fopen(input_path, "rb");
    uint32_t source_size = 0;
    if (in == NULL || file_size(in, &source_size) != 0 || source_size != plan->source_size) {
        set_error(error_msg, error_msg_len, "source ROM changed or cannot be read");
        goto done;
    }
    const char *slash = strrchr(input_path, '/');
    const char *extension = strrchr(input_path, '.');
    size_t stem_len = extension != NULL && (slash == NULL || extension > slash)
        ? (size_t)(extension - input_path) : strlen(input_path);
    if (stem_len >= sizeof(destination)) {
        set_error(error_msg, error_msg_len, "output path too long");
        goto done;
    }
    for (unsigned index = 0; index < 1000; ++index) {
        int length = index == 0
            ? snprintf(destination, sizeof(destination), "%.*s.patched.gba", (int)stem_len, input_path)
            : snprintf(destination, sizeof(destination), "%.*s.patched-%u.gba", (int)stem_len, input_path, index);
        if (length < 0 || (size_t)length >= sizeof(destination) || (size_t)length >= output_path_len) {
            set_error(error_msg, error_msg_len, "output path too long");
            goto done;
        }
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_BINARY
        flags |= O_BINARY;
#endif
        int fd = open(destination, flags, 0666);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            set_error(error_msg, error_msg_len, "cannot create patched ROM");
            goto done;
        }
        owns_output = true;
        out = fdopen(fd, "wb");
        if (out == NULL) { close(fd); break; }
        break;
    }
    if (out == NULL) {
        set_error(error_msg, error_msg_len, "cannot create unique patched ROM");
        goto done;
    }
    patch_debug_phase("saving_rom", destination);
    portENTER_CRITICAL(&s_patch_debug_lock);
    s_patch_debug.total = plan->output_size;
    portEXIT_CRITICAL(&s_patch_debug_lock);
    for (uint32_t offset = 0; offset < plan->output_size;) {
        if (patch_debug_cancelled()) goto done;
        if (save_progress_cb != NULL && !save_progress_cb(offset, plan->output_size, progress_ctx)) {
            burner_gba_patch_debug_cancel();
            goto done;
        }
        size_t count = plan->output_size - offset;
        if (count > PATCH_SCAN_BYTES) count = PATCH_SCAN_BYTES;
        size_t available = offset < plan->source_size ? plan->source_size - offset : 0;
        if (available > count) available = count;
        if (available && patch_debug_read(buffer, 1, available, in) != available) {
            set_error(error_msg, error_msg_len, "read source ROM failed");
            goto done;
        }
        if (available < count) memset(buffer + available, 0xFF, count - available);
        burner_apply_gba_patch_plan(buffer, count, offset, plan);
        if (fwrite(buffer, 1, count, out) != count) {
            set_error(error_msg, error_msg_len, "write patched ROM failed (check TF free space)");
            goto done;
        }
        offset += (uint32_t)count;
        portENTER_CRITICAL(&s_patch_debug_lock);
        s_patch_debug.offset = offset;
        portEXIT_CRITICAL(&s_patch_debug_lock);
    }
    if (fflush(out) != 0) {
        set_error(error_msg, error_msg_len, "flush patched ROM failed");
        goto done;
    }
    int close_result = fclose(out);
    out = NULL;
    if (close_result != 0) {
        set_error(error_msg, error_msg_len, "close patched ROM failed");
        goto done;
    }
    if (patch_debug_cancelled()) goto done;
    if (save_progress_cb != NULL && !save_progress_cb(plan->output_size, plan->output_size, progress_ctx)) {
        burner_gba_patch_debug_cancel();
        goto done;
    }
    result = ESP_OK;
done:
    if (in != NULL) fclose(in);
    if (out != NULL) fclose(out);
    /* Keep the guard through cancellation/result finalization. */
    result = patch_debug_finish(result, error_msg, error_msg_len);
    if (result != ESP_OK && owns_output) unlink(destination);
    if (result == ESP_OK) {
        snprintf(output_path, output_path_len, "%s", destination);
        if (report != NULL) report->created_copy = true;
    }
    free(buffer);
    free(plan);
    return result;
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
    (void)scan_patch_identifiers(fp, total, found, false, NULL, NULL);
    fclose(fp);
    for (size_t i = 0U; i < sizeof(found) / sizeof(found[0]); ++i) {
        if (found[i]) {
            any_found = true;
            break;
        }
    }
    return any_found;
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
