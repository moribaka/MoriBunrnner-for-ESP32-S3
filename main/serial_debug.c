#include "serial_debug.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "burner/core/ws_server_internal.h"
#include "burner/core/burner_gba_patch.h"
#include "usb_msc_tf.h"
#include "ui.h"
#include "reader/epub_native.h"
#include "music/music_player.h"
#include "ag32_batch_programmer.h"
#include "ag32_mcu_transport.h"
#include "mcu_debug.h"

static TaskHandle_t s_console;
static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_job_running;

typedef struct {
    char path[304];
    bool sram, waitcnt, batteryless, save_output;
} debug_patch_job_t;

static void reply(cJSON *json)
{
    if (json == NULL) return;
    char *line = cJSON_PrintUnformatted(json);
    if (line != NULL) {
        printf("\n@mori %s\n", line);
        fflush(stdout);
        free(line);
    }
    cJSON_Delete(json);
}

static cJSON *event(const char *type)
{
    cJSON *json = cJSON_CreateObject();
    if (json != NULL) cJSON_AddStringToObject(json, "event", type);
    return json;
}

static void message(const char *type, const char *text)
{
    cJSON *json = event(type);
    if (json != NULL) cJSON_AddStringToObject(json, "message", text);
    reply(json);
}

static void status(void)
{
    burner_gba_patch_debug_t patch;
    burner_gba_patch_debug_snapshot(&patch);
    cJSON *json = event("status");
    if (json == NULL) return;
    cJSON_AddStringToObject(json, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(json, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(json, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(json, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(json, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(json, "burn_running", burner_task_is_running_snapshot());
    cJSON_AddBoolToObject(json, "patch_running", patch.running);
    cJSON_AddBoolToObject(json, "cancel_requested", patch.cancel_requested);
    cJSON_AddStringToObject(json, "phase", patch.phase);
    cJSON_AddStringToObject(json, "detail", patch.detail);
    cJSON_AddNumberToObject(json, "offset", patch.offset);
    cJSON_AddNumberToObject(json, "total", patch.total);
    cJSON_AddNumberToObject(json, "passes", patch.passes);
    cJSON_AddNumberToObject(json, "read_bytes", (double)patch.read_bytes);
    cJSON_AddNumberToObject(json, "read_ms", patch.read_us / 1000);
    cJSON_AddNumberToObject(json, "elapsed_ms", patch.elapsed_ms);
    cJSON_AddNumberToObject(json, "result", patch.result);
    cJSON_AddStringToObject(json, "ag32_link",
        ag32_mcu_link_preference_name(ag32_mcu_link_get_preference()));
    cJSON_AddStringToObject(json, "ag32_link_active",
        ag32_mcu_link_active_name(ag32_mcu_link_get_active()));
    cJSON_AddNumberToObject(json, "ag32_capabilities", ag32_mcu_link_capabilities());
    cJSON_AddBoolToObject(json, "ag32_capabilities_known",
        ag32_mcu_link_capabilities_known());
    reply(json);
}

static bool save_progress(uint32_t done, uint32_t total, void *ctx)
{
    int *last_percent = ctx;
    if (usb_msc_tf_in_use_by_host()) return false;
    int percent = total ? (int)((uint64_t)done * 100 / total) : 0;
    if (percent != *last_percent) {
        *last_percent = percent;
        cJSON *json = event("save_progress");
        if (json != NULL) {
            cJSON_AddNumberToObject(json, "percent", percent);
            cJSON_AddNumberToObject(json, "done", done);
            cJSON_AddNumberToObject(json, "total", total);
        }
        reply(json);
    }
    return true;
}

static void patch_worker(void *arg)
{
    debug_patch_job_t *job = arg;
    burner_gba_patch_plan_t *plan = job->save_output ? NULL :
        heap_caps_calloc(1, sizeof(*plan), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    burner_gba_patch_report_t report = {0};
    char error[128] = "no memory for patch plan";
    int64_t start = esp_timer_get_time();
    int result = ESP_ERR_NO_MEM;
    char saved_path[304] = {0};
    int last_percent = -1;
    if (job->save_output) {
        result = burner_save_gba_patch_file(job->path, job->sram, job->waitcnt, job->batteryless,
            saved_path, sizeof(saved_path), &report, error, sizeof(error), NULL, save_progress, &last_percent);
    } else if (plan != NULL) {
        result = burner_build_gba_patch_plan(job->path, job->sram, job->waitcnt, job->batteryless,
                                            plan, &report, error, sizeof(error), NULL, NULL);
    }
    cJSON *json = event("patch_done");
    if (json != NULL) {
        cJSON_AddNumberToObject(json, "result", result);
        cJSON_AddStringToObject(json, "error", result == ESP_OK ? "" : error);
        cJSON_AddNumberToObject(json, "elapsed_ms", (esp_timer_get_time() - start) / 1000);
        cJSON_AddStringToObject(json, "save_type", report.patch_name);
        cJSON_AddBoolToObject(json, "sram", report.sram_patched);
        cJSON_AddBoolToObject(json, "batteryless", report.batteryless_patched);
        cJSON_AddNumberToObject(json, "waitcnt_count", report.waitcnt_count);
        cJSON_AddNumberToObject(json, "output_size", report.output_size);
        if (saved_path[0]) cJSON_AddStringToObject(json, "output_path", saved_path);
        if (plan != NULL && result == ESP_OK) {
            cJSON_AddNumberToObject(json, "sram_ops", plan->sram_count);
            cJSON_AddNumberToObject(json, "irq_ops", plan->batteryless_irq_count);
            cJSON_AddNumberToObject(json, "payload_offset", plan->payload_offset);
        }
    }
    free(plan);
    free(job);
    reply(json);
    portENTER_CRITICAL(&s_job_lock);
    s_job_running = false;
    portEXIT_CRITICAL(&s_job_lock);
    vTaskDelete(NULL);
}

static void epub_worker(void *arg)
{
    char *path = arg;
    ui_epub_book_t *book = NULL;
    uint32_t bytes = 0, hash = 2166136261U, sections = 0;
    int64_t start = esp_timer_get_time();
    bool ok = ui_epub_book_open(path, &book);
    if (ok) {
        sections = ui_epub_book_section_count(book);
        if (sections > 32) sections = 32;
        for (unsigned pass = 0; pass < 2 && ok; ++pass) {
            for (uint32_t i = 0; i < sections && ok; ++i) {
                uint8_t *text = NULL;
                size_t length = 0;
                uint32_t section = pass == 0 ? i : sections - 1 - i;
                ok = ui_epub_book_load_section_text(book, section, &text, &length);
                if (ok) {
                    for (size_t j = 0; j < length; ++j) hash = (hash ^ text[j]) * 16777619U;
                    bytes += (uint32_t)length;
                }
                ui_epub_book_free_buffer(text);
                vTaskDelay(1);
            }
        }
    }
    cJSON *json = event("epub_done");
    if (json != NULL) {
        cJSON_AddNumberToObject(json, "result", ok ? ESP_OK : ESP_FAIL);
        cJSON_AddNumberToObject(json, "sections", sections);
        cJSON_AddNumberToObject(json, "bytes", bytes);
        cJSON_AddNumberToObject(json, "hash", hash);
        cJSON_AddNumberToObject(json, "index_builds", ui_epub_book_index_build_count(book));
        cJSON_AddNumberToObject(json, "elapsed_ms", (esp_timer_get_time() - start) / 1000);
    }
    ui_epub_book_close(book);
    free(path);
    reply(json);
    portENTER_CRITICAL(&s_job_lock);
    s_job_running = false;
    portEXIT_CRITICAL(&s_job_lock);
    vTaskDelete(NULL);
}

static bool local_path(const char *path)
{
    return path != NULL && strlen(path) < 304 &&
        (strcmp(path, "/sdcard") == 0 || strncmp(path, "/sdcard/", 8) == 0) &&
        strstr(path, "..") == NULL;
}

static void dispatch(char *line)
{
    if (strcmp(line, "ag32-test") == 0 || strcmp(line, "ag32-stream-test") == 0) {
        bool stream = strcmp(line, "ag32-stream-test") == 0;
        if (burner_task_is_running_snapshot() || ag32_batch_program_is_running()) {
            message("error", "burner or AG32 batch job is running");
            return;
        }
        uint8_t *tx = heap_caps_malloc(AG32_MCU_MAX_PAYLOAD_SIZE, MALLOC_CAP_SPIRAM);
        uint8_t *rx = heap_caps_malloc(AG32_MCU_MAX_PAYLOAD_SIZE, MALLOC_CAP_SPIRAM);
        esp_err_t err = tx && rx ? ESP_OK : ESP_ERR_NO_MEM;
        uint32_t bytes = 0, passes = 0, random = 0x8147ab33;
        static const size_t sizes[] = {1, 3, 4, 31, 32, 33, 255, 512, 4096, 8192};
        ag32_link_preference_t previous = ag32_mcu_link_get_preference();
        burner_spi_lock_take();
        if (err == ESP_OK) err = burner_spi_init();
        ag32_mcu_link_set_preference(AG32_LINK_PREFERENCE_MCU);
        int64_t start = esp_timer_get_time();
        for (unsigned iteration = 0; err == ESP_OK && iteration < 100; ++iteration) {
            size_t size = sizes[iteration % (sizeof(sizes) / sizeof(sizes[0]))];
            for (size_t i = 0; i < size; ++i) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                tx[i] = (uint8_t)random;
            }
            size_t received = 0;
            bool used = false;
            if (stream) {
                err = ag32_mcu_try_stream_locked(0, AG32_MCU_CMD_ECHO, 0,
                    0, tx, size, 2000, &used);
                if (err == ESP_OK && !used) err = ESP_ERR_INVALID_RESPONSE;
            } else {
                err = ag32_mcu_try_command_locked(0, AG32_MCU_CMD_ECHO, 0,
                    tx, size, rx, size, &received, 2000, NULL, &used);
                if (err == ESP_OK && (!used || received != size || memcmp(tx, rx, size)))
                    err = ESP_ERR_INVALID_RESPONSE;
            }
            if (err == ESP_OK) { ++passes; bytes += size; }
        }
        int64_t elapsed = esp_timer_get_time() - start;
        ag32_mcu_link_set_preference(previous);
        burner_spi_lock_give();
        free(tx); free(rx);
        cJSON *json = event(stream ? "ag32_stream_test" : "ag32_test");
        if (json) {
            cJSON_AddBoolToObject(json, "ok", err == ESP_OK);
            cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
            cJSON_AddNumberToObject(json, "passes", passes);
            cJSON_AddNumberToObject(json, "bytes_each_direction", bytes);
            cJSON_AddNumberToObject(json, "elapsed_ms", elapsed / 1000.0);
        }
        reply(json);
        return;
    }
    if (strcmp(line, "ag32-ping") == 0) {
        if (burner_task_is_running_snapshot() || ag32_batch_program_is_running()) {
            message("error", "burner or AG32 batch job is running");
            return;
        }
        uint8_t payload[16] = {0};
        size_t payload_size = 0u;
        uint32_t capabilities = 0u;
        bool used_mcu = false;
        burner_spi_lock_take();
        esp_err_t err = burner_spi_init();
        if (err == ESP_OK) err = ag32_mcu_try_command_locked(
            0u, AG32_MCU_CMD_PING, 0u,
            NULL, 0u, payload, sizeof(payload), &payload_size,
            100u, &capabilities, &used_mcu);
        burner_spi_lock_give();
        cJSON *json = event("ag32_ping");
        if (json != NULL) {
            cJSON_AddBoolToObject(json, "ok", err == ESP_OK && used_mcu);
            cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
            cJSON_AddBoolToObject(json, "used_mcu", used_mcu);
            cJSON_AddStringToObject(json, "preference",
                ag32_mcu_link_preference_name(ag32_mcu_link_get_preference()));
            cJSON_AddStringToObject(json, "active",
                ag32_mcu_link_active_name(ag32_mcu_link_get_active()));
            cJSON_AddNumberToObject(json, "capabilities", capabilities);
            cJSON_AddNumberToObject(json, "protocol_version",
                payload_size >= 4u ? ag32_mcu_read_le32(payload) : 0u);
            cJSON_AddNumberToObject(json, "max_payload",
                payload_size >= 8u ? ag32_mcu_read_le32(payload + 4u) : 0u);
            cJSON_AddNumberToObject(json, "ag32_clock_hz",
                payload_size >= 12u ? ag32_mcu_read_le32(payload + 8u) : 0u);
            cJSON_AddNumberToObject(json, "identity",
                payload_size >= 16u ? ag32_mcu_read_le32(payload + 12u) : 0u);
        }
        reply(json);
        return;
    }
    if (strcmp(line, "ag32-link") == 0) {
        cJSON *json = event("ag32_link");
        if (json != NULL) {
            cJSON_AddStringToObject(json, "preference",
                ag32_mcu_link_preference_name(ag32_mcu_link_get_preference()));
            cJSON_AddStringToObject(json, "active",
                ag32_mcu_link_active_name(ag32_mcu_link_get_active()));
            cJSON_AddNumberToObject(json, "capabilities", ag32_mcu_link_capabilities());
            cJSON_AddBoolToObject(json, "capabilities_known",
                ag32_mcu_link_capabilities_known());
        }
        reply(json);
        return;
    }
    if (strncmp(line, "ag32-link ", 10) == 0) {
        ag32_link_preference_t next;
        ag32_link_preference_t previous = ag32_mcu_link_get_preference();
        if (!ag32_mcu_link_parse_preference(line + 10, &next)) {
            message("error", "ag32-link must be auto/legacy/mcu");
            return;
        }
        if (burner_task_is_running_snapshot() || ag32_batch_program_is_running()) {
            message("error", "burner or AG32 batch job is running");
            return;
        }
        ag32_mcu_link_set_preference(next);
        esp_err_t err = burner_save_burn_config();
        if (err != ESP_OK) {
            ag32_mcu_link_set_preference(previous);
            message("error", esp_err_to_name(err));
            return;
        }
        cJSON *json = event("ag32_link");
        if (json != NULL) {
            cJSON_AddBoolToObject(json, "ok", true);
            cJSON_AddStringToObject(json, "preference",
                ag32_mcu_link_preference_name(next));
            cJSON_AddStringToObject(json, "active",
                ag32_mcu_link_active_name(ag32_mcu_link_get_active()));
        }
        reply(json);
        return;
    }
    if (strcmp(line, "ag32-probe") == 0 || strcmp(line, "ag32-regs") == 0) {
        if (burner_task_is_running_snapshot() || ag32_batch_program_is_running()) {
            message("error", "burner or AG32 batch job is running");
            return;
        }
        bool snapshot = strcmp(line, "ag32-regs") == 0;
        uint32_t regs[7] = {0}, request[12] = {0}, response[12] = {0};
        mcu_debug_probe_result_t probe = {0};
        uint32_t device_id = 0u;
        bool spi_mode = false;
        bool session_open = false;
        bool halted = false;
        esp_err_t err = burner_spi_swd_restore_blocked()
            ? ESP_ERR_INVALID_STATE : burner_spi_enter_swd_mode();
        if (err == ESP_OK) {
            spi_mode = true;
            err = mcu_debug_session_begin(&probe.idcode);
            session_open = err == ESP_OK;
        }
        if (err == ESP_OK) {
            err = mcu_debug_halt();
            halted = err == ESP_OK;
        }
        if (err == ESP_OK) {
            err = mcu_debug_read_memory32(0x03000100u, &device_id);
        }
        if (snapshot) {
            for (unsigned i = 0; err == ESP_OK && i < 7; ++i)
                err = mcu_debug_read_memory32(0x60020000u + i * 4u, &regs[i]);
            if (err == ESP_OK && (regs[1] < 0x20000000u || regs[1] > 0x2001ffd0u ||
                                 regs[4] < 0x20000000u || regs[4] > 0x2001ffd0u))
                err = ESP_ERR_INVALID_RESPONSE;
            for (unsigned i = 0; err == ESP_OK && i < 12; ++i) {
                err = mcu_debug_read_memory32(regs[1] + i * 4u, &request[i]);
                if (err == ESP_OK) err = mcu_debug_read_memory32(regs[4] + i * 4u, &response[i]);
            }
        }
        if (halted) {
            esp_err_t resume_err = mcu_debug_resume();
            if (err == ESP_OK) err = resume_err;
        }
        if (session_open) {
            mcu_debug_session_end();
        }
        if (spi_mode) {
            esp_err_t restore_err = burner_spi_leave_swd_mode(true);
            if (err == ESP_OK) err = restore_err;
        }
        probe.status = err == ESP_OK ? MCU_DEBUG_PROBE_OK : MCU_DEBUG_PROBE_IO_ERROR;
        cJSON *json = event(snapshot ? "ag32_regs" : "ag32_probe");
        if (json != NULL) {
            cJSON_AddBoolToObject(json, "ok", err == ESP_OK);
            cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
            cJSON_AddStringToObject(json, "status", mcu_debug_probe_status_str(probe.status));
            cJSON_AddNumberToObject(json, "dp_idcode", probe.idcode);
            cJSON_AddNumberToObject(json, "device_id", device_id);
            if (snapshot) {
                cJSON *arrays[] = {cJSON_AddArrayToObject(json, "registers"),
                    cJSON_AddArrayToObject(json, "request_words"),
                    cJSON_AddArrayToObject(json, "response_words")};
                for (unsigned a = 0; a < 3; ++a) {
                    uint32_t *words = a == 0 ? regs : (a == 1 ? request : response);
                    for (unsigned i = 0; i < (a == 0 ? 7u : 12u); ++i) {
                        char hex[11]; snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)words[i]);
                        cJSON_AddItemToArray(arrays[a], cJSON_CreateString(hex));
                    }
                }
            }
        }
        reply(json);
        return;
    }
    if (strncmp(line, "ag32-batch ", 11) == 0) {
        esp_err_t err = ag32_batch_program_start(line + 11);
        message(err == ESP_OK ? "ag32_batch_started" : "error", esp_err_to_name(err));
        return;
    }
    if (strncmp(line, "ag32-batch-check ", 17) == 0) {
        uint32_t records = 0u;
        uint32_t bytes = 0u;
        char validation_error[160] = {0};
        esp_err_t err = ag32_batch_validate_path(
            line + 17, &records, &bytes, validation_error, sizeof(validation_error));
        cJSON *json = event("ag32_batch_check");
        if (json != NULL) {
            cJSON_AddBoolToObject(json, "ok", err == ESP_OK);
            cJSON_AddStringToObject(json, "error", err == ESP_OK ? "" : validation_error);
            cJSON_AddNumberToObject(json, "records", records);
            cJSON_AddNumberToObject(json, "payload_bytes", bytes);
        }
        reply(json);
        return;
    }
    if (strcmp(line, "ag32-batch-status") == 0) {
        ag32_batch_job_status_t status;
        ag32_batch_program_status(&status);
        cJSON *json = event("ag32_batch_status");
        if (json != NULL) {
            cJSON_AddStringToObject(json, "state", ag32_batch_job_state_name(status.state));
            cJSON_AddStringToObject(json, "phase", status.phase);
            cJSON_AddStringToObject(json, "message", status.message);
            cJSON_AddNumberToObject(json, "processed", status.processed);
            cJSON_AddNumberToObject(json, "total", status.total);
            cJSON_AddNumberToObject(json, "device_id", status.report.device_id);
            cJSON_AddBoolToObject(json, "recovery_required", burner_spi_swd_restore_blocked());
        }
        reply(json);
        return;
    }
    if (strncmp(line, "tf-bench ", 9) == 0) {
        burner_gba_patch_debug_t patch;
        burner_gba_patch_debug_snapshot(&patch);
        if (!local_path(line + 9) || usb_msc_tf_in_use_by_host() ||
            ag32_batch_program_is_running() || burner_task_is_running_snapshot() || patch.running) {
            message("error", "invalid path or TF busy"); return;
        }
        uint8_t *buf = heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!buf) { message("error", "no DMA memory"); return; }
        for (int mode = 0; mode < 3; ++mode) {
            FILE *fp = mode < 2 ? fopen(line + 9, "rb") : NULL;
            int fd = mode == 2 ? open(line + 9, O_RDONLY) : -1;
            if ((mode < 2 && !fp) || (mode == 2 && fd < 0)) break;
            if (mode == 1) setvbuf(fp, NULL, _IOFBF, 16384);
            size_t total = 0;
            uint32_t sum = 0;
            int64_t started = esp_timer_get_time();
            while (total < 1024u * 1024u && !usb_msc_tf_in_use_by_host()
                && !ag32_batch_program_is_running()) {
                int got = mode < 2 ? (int)fread(buf, 1, 16384, fp) : (int)read(fd, buf, 16384);
                if (got <= 0) break;
                for (int i = 0; i < got; ++i) sum += buf[i];
                total += got;
                vTaskDelay(1);
            }
            int64_t elapsed = esp_timer_get_time() - started;
            if (fp) fclose(fp);
            if (fd >= 0) close(fd);
            cJSON *json = event("tf_bench_sample");
            cJSON_AddStringToObject(json, "mode", mode == 0 ? "buffered" : mode == 1 ? "buffered16k" : "posix");
            cJSON_AddNumberToObject(json, "bytes", total);
            cJSON_AddNumberToObject(json, "sum", sum);
            cJSON_AddNumberToObject(json, "elapsed_ms", elapsed / 1000);
            reply(json);
        }
        heap_caps_free(buf);
        message("tf_bench_done", "read-only comparison finished");
        return;
    }
    if (strncmp(line, "epub ", 5) == 0) {
        if (!local_path(line + 5) || usb_msc_tf_in_use_by_host()
            || ag32_batch_program_is_running()) { message("error", "invalid path or TF busy"); return; }
        char *path = strdup(line + 5);
        if (path == NULL) { message("error", "no memory"); return; }
        portENTER_CRITICAL(&s_job_lock);
        bool busy = s_job_running;
        if (!busy) s_job_running = true;
        portEXIT_CRITICAL(&s_job_lock);
        if (busy) { free(path); message("error", "debug worker busy"); return; }
        if (xTaskCreatePinnedToCore(epub_worker, "debug_epub", 16384, path, 2, NULL, 1) != pdPASS) {
            free(path);
            portENTER_CRITICAL(&s_job_lock);
            s_job_running = false;
            portEXIT_CRITICAL(&s_job_lock);
            message("error", "cannot start EPUB worker");
        }
        return;
    }
    if (strncmp(line, "play ", 5) == 0) {
        struct stat st;
        if (!local_path(line + 5) || usb_msc_tf_in_use_by_host()
            || ag32_batch_program_is_running() || stat(line + 5, &st) != 0 ||
            !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX) {
            message("error", "invalid audio path or TF busy"); return;
        }
        esp_err_t err = music_player_play(line + 5, (uint32_t)st.st_size);
        message(err == ESP_OK ? "play" : "error", esp_err_to_name(err));
        return;
    }
    if (strcmp(line, "ui") == 0) {
        ui_runtime_stats_t stats;
        ui_get_runtime_stats(&stats);
        cJSON *json = event("ui");
        if (json != NULL) {
            cJSON_AddNumberToObject(json, "process_calls", stats.process_calls);
            cJSON_AddNumberToObject(json, "render_calls", stats.render_calls);
            cJSON_AddNumberToObject(json, "music_polls", stats.music_polls);
            cJSON_AddNumberToObject(json, "model_bytes", stats.model_bytes);
            cJSON_AddNumberToObject(json, "directory_scans", stats.directory_scans);
            cJSON_AddNumberToObject(json, "cache_hits", stats.directory_cache_hits);
            cJSON_AddNumberToObject(json, "page", stats.page);
            cJSON_AddNumberToObject(json, "selected", stats.selected);
            cJSON_AddNumberToObject(json, "count", stats.item_count);
            cJSON_AddStringToObject(json, "selection", stats.selection);
            cJSON_AddStringToObject(json, "status", stats.status);
        }
        reply(json);
        return;
    }
    if (strncmp(line, "key ", 4) == 0) {
        const char *names[] = {"left", "right", "up", "down", "a", "panel", "b", "menu", "vol+", "vol-"};
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (strcmp(line + 4, names[i]) == 0) {
                ui_post_button((ui_button_t)i, true);
                vTaskDelay(pdMS_TO_TICKS(40));
                ui_post_button((ui_button_t)i, false);
                message("key", names[i]);
                return;
            }
        }
        message("error", "unknown key");
        return;
    }
    if (strcmp(line, "status") == 0) { status(); return; }
    if (strcmp(line, "cancel") == 0) {
        message("cancel", burner_gba_patch_debug_cancel() ? "requested" : "no active patch");
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        message("reboot", "restarting");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    if (strcmp(line, "help") == 0) {
        message("help", "status | ui | key up/down/left/right/a/b/menu | ls PATH | tf-bench PATH | ag32-link [auto|legacy|mcu] | ag32-ping | ag32-test | ag32-stream-test | ag32-probe | ag32-regs | ag32-batch-check PATH | ag32-batch PATH | ag32-batch-status | patch FLAGS PATH | patch-save FLAGS PATH | epub PATH | play PATH | cancel | reboot; FLAGS: s=SRAM b=batteryless w=WAITCNT");
        return;
    }
    if (strncmp(line, "ls ", 3) == 0) {
        const char *path = line + 3;
        if (!local_path(path) || usb_msc_tf_in_use_by_host()
            || ag32_batch_program_is_running()) { message("error", "invalid path or TF busy"); return; }
        DIR *dir = opendir(path);
        if (dir == NULL) { message("error", "cannot open directory"); return; }
        struct dirent *entry;
        unsigned count = 0;
        while ((entry = readdir(dir)) != NULL && count < 256) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            cJSON *json = event("file");
            if (json != NULL) {
                cJSON_AddStringToObject(json, "name", entry->d_name);
                cJSON_AddBoolToObject(json, "directory", entry->d_type == DT_DIR);
            }
            reply(json);
            ++count;
        }
        closedir(dir);
        message("ls_done", count == 256 ? "limit reached (256)" : "complete");
        return;
    }
    if (strncmp(line, "patch ", 6) == 0 || strncmp(line, "patch-save ", 11) == 0) {
        bool export_file = strncmp(line, "patch-save ", 11) == 0;
        char *flags = line + (export_file ? 11 : 6);
        char *path = strchr(flags, ' ');
        if (path == NULL) { message("error", "usage: patch sbw /sdcard/file.gba"); return; }
        *path++ = '\0';
        if (*flags == '\0' || strspn(flags, "sbw") != strlen(flags) || !local_path(path)) {
            message("error", "invalid flags/path"); return;
        }
        if (usb_msc_tf_in_use_by_host() || ag32_batch_program_is_running()
            || burner_task_is_running_snapshot()) {
            message("error", "TF or burner busy"); return;
        }
        debug_patch_job_t *job = calloc(1, sizeof(*job));
        if (job == NULL) { message("error", "no memory"); return; }
        snprintf(job->path, sizeof(job->path), "%s", path);
        job->save_output = export_file;
        job->batteryless = strchr(flags, 'b') != NULL;
        job->sram = strchr(flags, 's') != NULL || job->batteryless;
        job->waitcnt = strchr(flags, 'w') != NULL;
        portENTER_CRITICAL(&s_job_lock);
        bool busy = s_job_running;
        if (!busy) s_job_running = true;
        portEXIT_CRITICAL(&s_job_lock);
        if (busy) { free(job); message("error", "debug patch already running"); return; }
        /* Separate worker keeps status/cancel responsive during TF scans. */
        if (xTaskCreatePinnedToCore(patch_worker, "debug_patch", 16 * 1024, job, 2, NULL, 1) != pdPASS) {
            free(job);
            portENTER_CRITICAL(&s_job_lock);
            s_job_running = false;
            portEXIT_CRITICAL(&s_job_lock);
            message("error", "cannot start patch worker");
        } else message("patch_started", export_file ? "patch and save new ROM" : "analysis only");
        return;
    }
    message("error", "unknown command; use help");
}

static void console_task(void *arg)
{
    (void)arg;
    char line[384];
    size_t used = 0;
    bool overflow = false;
    message("ready", "Mori serial debug v1; type help");
    for (;;) {
        char input[64];
        int count = usb_serial_jtag_read_bytes(input, sizeof(input), 0);
        for (int i = 0; i < count; ++i) {
            char c = input[i];
            if (c == '\r' || c == '\n') {
                if (overflow) message("error", "command too long");
                else if (used) { line[used] = '\0'; dispatch(line); }
                used = 0;
                overflow = false;
            } else if (!overflow) {
                if (c == '\b' || c == 127) { if (used) --used; }
                else if (c != '\0') {
                    if (used + 1 < sizeof(line)) line[used++] = c;
                    else overflow = true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t serial_debug_start(void)
{
    if (s_console != NULL) return ESP_OK;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .rx_buffer_size = 1024,
            .tx_buffer_size = 2048,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK) return err;
    }
    usb_serial_jtag_vfs_use_driver();
    return xTaskCreatePinnedToCore(console_task, "serial_debug", 6144, NULL, 3, &s_console, 0) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
