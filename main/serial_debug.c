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

static TaskHandle_t s_console;
static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_job_running;

typedef struct {
    char path[304];
    bool sram, waitcnt, batteryless;
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
    reply(json);
}

static void patch_worker(void *arg)
{
    debug_patch_job_t *job = arg;
    burner_gba_patch_plan_t *plan = heap_caps_calloc(1, sizeof(*plan), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    burner_gba_patch_report_t report = {0};
    char error[128] = "no memory for patch plan";
    int64_t start = esp_timer_get_time();
    int result = ESP_ERR_NO_MEM;
    if (plan != NULL) {
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

static bool local_path(const char *path)
{
    return path != NULL && strlen(path) < 304 &&
        (strcmp(path, "/sdcard") == 0 || strncmp(path, "/sdcard/", 8) == 0) &&
        strstr(path, "..") == NULL;
}

static void dispatch(char *line)
{
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
        message("help", "status | ls /sdcard/path | patch FLAGS /sdcard/file.gba | cancel | reboot; FLAGS: s=SRAM b=batteryless w=WAITCNT; patch only analyzes, does not write ROM/cart");
        return;
    }
    if (strncmp(line, "ls ", 3) == 0) {
        const char *path = line + 3;
        if (!local_path(path) || usb_msc_tf_in_use_by_host()) { message("error", "invalid path or TF owned by USB host"); return; }
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
    if (strncmp(line, "patch ", 6) == 0) {
        char *flags = line + 6;
        char *path = strchr(flags, ' ');
        if (path == NULL) { message("error", "usage: patch sbw /sdcard/file.gba"); return; }
        *path++ = '\0';
        if (*flags == '\0' || strspn(flags, "sbw") != strlen(flags) || !local_path(path)) {
            message("error", "invalid flags/path"); return;
        }
        if (usb_msc_tf_in_use_by_host() || burner_task_is_running_snapshot()) {
            message("error", "TF or burner busy"); return;
        }
        debug_patch_job_t *job = calloc(1, sizeof(*job));
        if (job == NULL) { message("error", "no memory"); return; }
        snprintf(job->path, sizeof(job->path), "%s", path);
        job->batteryless = strchr(flags, 'b') != NULL;
        job->sram = strchr(flags, 's') != NULL || job->batteryless;
        job->waitcnt = strchr(flags, 'w') != NULL;
        portENTER_CRITICAL(&s_job_lock);
        bool busy = s_job_running;
        if (!busy) s_job_running = true;
        portEXIT_CRITICAL(&s_job_lock);
        if (busy) { free(job); message("error", "debug patch already running"); return; }
        /* Separate worker keeps status/cancel responsive during TF scans. */
        if (xTaskCreatePinnedToCore(patch_worker, "debug_patch", 48 * 1024, job, 2, NULL, 1) != pdPASS) {
            free(job);
            portENTER_CRITICAL(&s_job_lock);
            s_job_running = false;
            portEXIT_CRITICAL(&s_job_lock);
            message("error", "cannot start patch worker");
        } else message("patch_started", "analysis only");
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
