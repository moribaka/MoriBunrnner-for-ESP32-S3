#include "music_player.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "smb_client.h"

#define MUSIC_PLAYER_TAG "music_player"
#define MUSIC_PLAYER_TASK_STACK_SIZE 8192
#define MUSIC_PLAYER_TASK_PRIORITY 4
#define MUSIC_PLAYER_OUTPUT_TASK_STACK_SIZE 4096
#define MUSIC_PLAYER_OUTPUT_TASK_PRIORITY 6
#define MUSIC_PLAYER_QUEUE_LEN 8
#define MUSIC_PLAYER_CMD_WAIT_MS 50
#define MUSIC_PLAYER_DEFAULT_VOLUME 60
#define MUSIC_PLAYER_INPUT_BUF_SIZE 8192
#define MUSIC_PLAYER_INPUT_LOW_WATER 2048
#define MUSIC_PLAYER_DECODE_BUF_SIZE 8192
#define MUSIC_PLAYER_I2S_BUF_SIZE 8192
#define MUSIC_PLAYER_PCM_RING_SIZE (192U * 1024U)
#define MUSIC_PLAYER_WRITE_TIMEOUT_MS 1000
#define MUSIC_PLAYER_ERROR_BUDGET 64
#define MUSIC_PLAYER_PROGRESS_UPDATE_MS 250U
#define MUSIC_PLAYER_MP3_SCAN_LIMIT 262144U

typedef enum {
    MUSIC_PLAYER_CMD_PLAY = 0,
    MUSIC_PLAYER_CMD_STOP,
    MUSIC_PLAYER_CMD_TOGGLE_PAUSE,
    MUSIC_PLAYER_CMD_SET_VOLUME,
    MUSIC_PLAYER_CMD_SEEK_RELATIVE,
} music_player_cmd_type_t;

typedef struct {
    music_player_cmd_type_t type;
    music_player_source_t source;
    char path[MUSIC_PLAYER_PATH_MAX];
    uint32_t file_size;
    uint32_t start_position;
    uint8_t volume_percent;
    int32_t seek_delta_bytes;
} music_player_cmd_t;

typedef struct {
    music_player_source_t source;
    FILE *tf_fp;
    smb_client_file_t *smb_fp;
} music_player_input_t;

typedef struct {
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint32_t bitrate;
    uint8_t channels;
    uint8_t bits_per_sample;
} music_player_track_info_t;

typedef struct {
    uint8_t *buf;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t fill;
    bool stop;
    bool paused;
    bool finished;
    uint8_t volume_percent;
    esp_err_t output_err;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t can_read;
    SemaphoreHandle_t can_write;
    TaskHandle_t task;
} music_player_pcm_ring_t;

typedef struct {
    bool stop_requested;
    bool switch_track;
    bool paused;
    bool volume_changed;
    bool seek_requested;
    uint8_t volume_percent;
    int32_t seek_delta_bytes;
    music_player_cmd_t next_track;
} music_player_runtime_cmd_t;

static SemaphoreHandle_t s_music_lock = NULL;
static QueueHandle_t s_music_queue = NULL;
static TaskHandle_t s_music_task = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static bool s_i2s_ready = false;
static bool s_i2s_enabled = false;
static uint32_t s_i2s_sample_rate = 0;
static bool s_music_inited = false;
static bool s_decoder_registry_ready = false;
static music_player_snapshot_t s_snapshot = {
    .state = MUSIC_PLAYER_STATE_IDLE,
    .volume_percent = MUSIC_PLAYER_DEFAULT_VOLUME,
};

static void music_player_task(void *arg);
static void music_player_poll_runtime_cmds(music_player_runtime_cmd_t *runtime);
static esp_err_t music_player_input_seek(music_player_input_t *input, uint32_t target_position);
static uint32_t music_player_estimate_ms_from_bytes(uint32_t bytes, uint32_t bitrate);

static void music_player_snapshot_lock(void)
{
    if (s_music_lock != NULL) {
        (void)xSemaphoreTake(s_music_lock, portMAX_DELAY);
    }
}

static void music_player_snapshot_unlock(void)
{
    if (s_music_lock != NULL) {
        xSemaphoreGive(s_music_lock);
    }
}

static void music_player_copy_name_from_path(char *name, size_t name_len, const char *path)
{
    const char *base = path;

    if (name == NULL || name_len == 0U) {
        return;
    }
    if (path == NULL) {
        name[0] = '\0';
        return;
    }
    base = strrchr(path, '/');
    if (base != NULL && base[1] != '\0') {
        base++;
    } else {
        base = path;
    }
    snprintf(name, name_len, "%s", base);
}

static void music_player_set_snapshot(
    music_player_state_t state,
    music_player_source_t source,
    const char *path,
    uint32_t file_size,
    uint32_t position,
    uint32_t elapsed_ms,
    uint32_t duration_ms,
    uint32_t sample_rate,
    uint32_t bitrate,
    uint8_t channels,
    uint8_t bits_per_sample,
    const char *message)
{
    music_player_snapshot_lock();
    s_snapshot.state = state;
    s_snapshot.source = source;
    if (path != NULL) {
        snprintf(s_snapshot.path, sizeof(s_snapshot.path), "%s", path);
        music_player_copy_name_from_path(s_snapshot.name, sizeof(s_snapshot.name), path);
    }
    s_snapshot.file_size = file_size;
    s_snapshot.position = position;
    s_snapshot.elapsed_ms = elapsed_ms;
    s_snapshot.duration_ms = duration_ms;
    s_snapshot.sample_rate = sample_rate;
    s_snapshot.bitrate = bitrate;
    s_snapshot.channels = channels;
    s_snapshot.bits_per_sample = bits_per_sample;
    if (message != NULL) {
        snprintf(s_snapshot.message, sizeof(s_snapshot.message), "%s", message);
    } else {
        s_snapshot.message[0] = '\0';
    }
    music_player_snapshot_unlock();
}

static void music_player_set_volume_locked(uint8_t volume_percent)
{
    s_snapshot.volume_percent = volume_percent;
}

static uint8_t music_player_get_volume_locked(void)
{
    return s_snapshot.volume_percent;
}

static esp_audio_simple_dec_type_t music_player_decoder_type_from_path(const char *path)
{
    const char *ext = NULL;

    if (path == NULL) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
    ext = strrchr(path, '.');
    if (ext == NULL || ext[1] == '\0') {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
    ext++;
    if (strcasecmp(ext, "aac") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (strcasecmp(ext, "mp3") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (strcasecmp(ext, "flac") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (strcasecmp(ext, "wav") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static esp_err_t music_player_i2s_ensure_ready(void)
{
    esp_err_t err;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MORI_PIN_I2S_BCK,
            .ws = MORI_PIN_I2S_WS,
            .dout = MORI_PIN_I2S_SD,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (s_i2s_ready) {
        return ESP_OK;
    }

    err = i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(MUSIC_PLAYER_TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(MUSIC_PLAYER_TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        return err;
    }

    s_i2s_sample_rate = 44100;
    s_i2s_ready = true;
    return ESP_OK;
}

static void music_player_i2s_disable(void)
{
    if (s_i2s_ready && s_i2s_enabled) {
        if (i2s_channel_disable(s_i2s_tx) == ESP_OK) {
            s_i2s_enabled = false;
        }
    }
}

static esp_err_t music_player_i2s_enable(void)
{
    esp_err_t err;

    if (!s_i2s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_i2s_enabled) {
        return ESP_OK;
    }

    err = i2s_channel_enable(s_i2s_tx);
    if (err == ESP_OK) {
        s_i2s_enabled = true;
    }
    return err;
}

static esp_err_t music_player_i2s_apply_rate(uint32_t sample_rate)
{
    esp_err_t err;
    i2s_std_clk_config_t clk_cfg;
    bool was_enabled;

    if (sample_rate == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    err = music_player_i2s_ensure_ready();
    if (err != ESP_OK) {
        return err;
    }

    if (s_i2s_sample_rate == sample_rate) {
        return music_player_i2s_enable();
    }

    was_enabled = s_i2s_enabled;
    if (was_enabled) {
        music_player_i2s_disable();
    }
    clk_cfg = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    err = i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(
            MUSIC_PLAYER_TAG,
            "i2s reconfig sample_rate=%" PRIu32 " failed: %s",
            sample_rate,
            esp_err_to_name(err));
        if (was_enabled) {
            (void)music_player_i2s_enable();
        }
        return err;
    }
    s_i2s_sample_rate = sample_rate;
    return music_player_i2s_enable();
}

static size_t music_player_ring_free_locked(const music_player_pcm_ring_t *ring)
{
    return (ring != NULL && ring->capacity >= ring->fill) ? (ring->capacity - ring->fill) : 0U;
}

static bool music_player_pcm_ring_init(music_player_pcm_ring_t *ring, size_t capacity, uint8_t volume_percent)
{
    if (ring == NULL || capacity == 0U) {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    ring->buf = (uint8_t *)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring->buf == NULL) {
        ring->buf = (uint8_t *)heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
    }
    ring->mutex = xSemaphoreCreateMutex();
    ring->can_read = xSemaphoreCreateBinary();
    ring->can_write = xSemaphoreCreateBinary();
    if (ring->buf == NULL || ring->mutex == NULL || ring->can_read == NULL || ring->can_write == NULL) {
        if (ring->buf != NULL) {
            free(ring->buf);
        }
        if (ring->mutex != NULL) {
            vSemaphoreDelete(ring->mutex);
        }
        if (ring->can_read != NULL) {
            vSemaphoreDelete(ring->can_read);
        }
        if (ring->can_write != NULL) {
            vSemaphoreDelete(ring->can_write);
        }
        memset(ring, 0, sizeof(*ring));
        return false;
    }
    ring->capacity = capacity;
    ring->volume_percent = volume_percent;
    ring->output_err = ESP_OK;
    xSemaphoreGive(ring->can_write);
    return true;
}

static void music_player_pcm_ring_signal_stop(music_player_pcm_ring_t *ring)
{
    if (ring == NULL || ring->mutex == NULL) {
        return;
    }
    xSemaphoreTake(ring->mutex, portMAX_DELAY);
    ring->stop = true;
    xSemaphoreGive(ring->mutex);
    if (ring->can_read != NULL) {
        xSemaphoreGive(ring->can_read);
    }
    if (ring->can_write != NULL) {
        xSemaphoreGive(ring->can_write);
    }
}

static void music_player_pcm_ring_destroy(music_player_pcm_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }
    music_player_pcm_ring_signal_stop(ring);
    while (ring->task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ring->buf != NULL) {
        free(ring->buf);
    }
    if (ring->mutex != NULL) {
        vSemaphoreDelete(ring->mutex);
    }
    if (ring->can_read != NULL) {
        vSemaphoreDelete(ring->can_read);
    }
    if (ring->can_write != NULL) {
        vSemaphoreDelete(ring->can_write);
    }
    memset(ring, 0, sizeof(*ring));
}

static void music_player_pcm_ring_set_finished(music_player_pcm_ring_t *ring)
{
    if (ring == NULL || ring->mutex == NULL) {
        return;
    }
    xSemaphoreTake(ring->mutex, portMAX_DELAY);
    ring->finished = true;
    xSemaphoreGive(ring->mutex);
    if (ring->can_read != NULL) {
        xSemaphoreGive(ring->can_read);
    }
}

static void music_player_pcm_ring_set_paused(music_player_pcm_ring_t *ring, bool paused)
{
    if (ring == NULL || ring->mutex == NULL) {
        return;
    }
    xSemaphoreTake(ring->mutex, portMAX_DELAY);
    ring->paused = paused;
    xSemaphoreGive(ring->mutex);
    if (!paused && ring->can_read != NULL) {
        xSemaphoreGive(ring->can_read);
    }
}

static void music_player_pcm_ring_set_volume(music_player_pcm_ring_t *ring, uint8_t volume_percent)
{
    if (ring == NULL || ring->mutex == NULL) {
        return;
    }
    xSemaphoreTake(ring->mutex, portMAX_DELAY);
    ring->volume_percent = volume_percent;
    xSemaphoreGive(ring->mutex);
}

static void music_player_pcm_ring_flush(music_player_pcm_ring_t *ring)
{
    if (ring == NULL || ring->mutex == NULL) {
        return;
    }
    xSemaphoreTake(ring->mutex, portMAX_DELAY);
    ring->read_pos = 0U;
    ring->write_pos = 0U;
    ring->fill = 0U;
    ring->finished = false;
    xSemaphoreGive(ring->mutex);
    if (ring->can_write != NULL) {
        xSemaphoreGive(ring->can_write);
    }
}

static bool music_player_pcm_ring_has_error(music_player_pcm_ring_t *ring)
{
    bool has_error;

    if (ring == NULL || ring->mutex == NULL) {
        return false;
    }
    xSemaphoreTake(ring->mutex, portMAX_DELAY);
    has_error = ring->output_err != ESP_OK;
    xSemaphoreGive(ring->mutex);
    return has_error;
}

static esp_err_t music_player_pcm_ring_get_error(music_player_pcm_ring_t *ring)
{
    esp_err_t err;

    if (ring == NULL || ring->mutex == NULL) {
        return ESP_OK;
    }
    xSemaphoreTake(ring->mutex, portMAX_DELAY);
    err = ring->output_err;
    xSemaphoreGive(ring->mutex);
    return err;
}

static size_t music_player_pcm_ring_read(music_player_pcm_ring_t *ring, uint8_t *dst, size_t dst_len, uint8_t *volume_out)
{
    size_t read_len = 0U;

    if (ring == NULL || dst == NULL || dst_len == 0U) {
        return 0U;
    }
    for (;;) {
        bool should_wait = false;

        xSemaphoreTake(ring->mutex, portMAX_DELAY);
        if (ring->stop) {
            xSemaphoreGive(ring->mutex);
            return 0U;
        }
        if (ring->paused) {
            xSemaphoreGive(ring->mutex);
            music_player_i2s_disable();
            xSemaphoreTake(ring->can_read, portMAX_DELAY);
            continue;
        }
        if (!ring->paused && ring->fill > 0U) {
            size_t chunk = ring->capacity - ring->read_pos;

            read_len = (ring->fill < dst_len) ? ring->fill : dst_len;
            if (read_len > chunk) {
                read_len = chunk;
            }
            read_len &= ~(size_t)0x03U;
            if (read_len > 0U) {
                memcpy(dst, ring->buf + ring->read_pos, read_len);
                ring->read_pos = (ring->read_pos + read_len) % ring->capacity;
                ring->fill -= read_len;
                if (volume_out != NULL) {
                    *volume_out = ring->volume_percent;
                }
                xSemaphoreGive(ring->can_write);
                xSemaphoreGive(ring->mutex);
                return read_len;
            }
        }
        if (ring->finished && ring->fill == 0U) {
            xSemaphoreGive(ring->mutex);
            return 0U;
        }
        should_wait = true;
        xSemaphoreGive(ring->mutex);
        if (should_wait) {
            xSemaphoreTake(ring->can_read, pdMS_TO_TICKS(20));
        }
    }
}

static bool music_player_pcm_ring_write(
    music_player_pcm_ring_t *ring,
    const uint8_t *src,
    size_t src_len,
    music_player_runtime_cmd_t *runtime)
{
    size_t written = 0U;

    if (ring == NULL || src == NULL || src_len == 0U) {
        return false;
    }
    while (written < src_len) {
        size_t write_len = 0U;

        if (runtime != NULL) {
            music_player_poll_runtime_cmds(runtime);
            if (runtime->volume_changed) {
                music_player_snapshot_lock();
                music_player_set_volume_locked(runtime->volume_percent);
                music_player_snapshot_unlock();
                music_player_pcm_ring_set_volume(ring, runtime->volume_percent);
                runtime->volume_changed = false;
            }
            if (runtime->stop_requested || runtime->switch_track || runtime->paused || runtime->seek_requested) {
                return false;
            }
        }

        xSemaphoreTake(ring->mutex, portMAX_DELAY);
        if (ring->stop || ring->output_err != ESP_OK) {
            xSemaphoreGive(ring->mutex);
            return false;
        }
        if (music_player_ring_free_locked(ring) > 0U) {
            size_t free_space = music_player_ring_free_locked(ring);
            size_t contiguous = ring->capacity - ring->write_pos;

            write_len = src_len - written;
            if (write_len > free_space) {
                write_len = free_space;
            }
            if (write_len > contiguous) {
                write_len = contiguous;
            }
            write_len &= ~(size_t)0x03U;
            if (write_len > 0U) {
                memcpy(ring->buf + ring->write_pos, src + written, write_len);
                ring->write_pos = (ring->write_pos + write_len) % ring->capacity;
                ring->fill += write_len;
                written += write_len;
                xSemaphoreGive(ring->can_read);
            }
        }
        xSemaphoreGive(ring->mutex);

        if (write_len == 0U) {
            xSemaphoreTake(ring->can_write, pdMS_TO_TICKS(10));
        }
    }
    return true;
}

static void music_player_apply_volume_stereo16(uint8_t *buf, size_t len, uint8_t volume_percent)
{
    int16_t *samples = (int16_t *)buf;
    size_t sample_count = len / sizeof(int16_t);

    if (buf == NULL || len == 0U || volume_percent >= 100U) {
        return;
    }
    if (volume_percent == 0U) {
        memset(buf, 0, len);
        return;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        samples[i] = (int16_t)(((int32_t)samples[i] * (int32_t)volume_percent) / 100);
    }
}

static void music_player_output_task(void *arg)
{
    music_player_pcm_ring_t *ring = (music_player_pcm_ring_t *)arg;
    uint8_t *i2s_buf = NULL;

    if (ring == NULL) {
        vTaskDelete(NULL);
        return;
    }
    i2s_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_I2S_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (i2s_buf == NULL) {
        xSemaphoreTake(ring->mutex, portMAX_DELAY);
        ring->output_err = ESP_ERR_NO_MEM;
        xSemaphoreGive(ring->mutex);
        ring->task = NULL;
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        uint8_t volume_percent = 100U;
        size_t read_len = music_player_pcm_ring_read(ring, i2s_buf, MUSIC_PLAYER_I2S_BUF_SIZE, &volume_percent);
        size_t wrote_bytes = 0U;
        esp_err_t err;

        if (read_len == 0U) {
            break;
        }
        if (!s_i2s_enabled) {
            err = music_player_i2s_enable();
            if (err != ESP_OK) {
                xSemaphoreTake(ring->mutex, portMAX_DELAY);
                ring->output_err = err;
                ring->stop = true;
                xSemaphoreGive(ring->mutex);
                xSemaphoreGive(ring->can_write);
                break;
            }
        }
        music_player_apply_volume_stereo16(i2s_buf, read_len, volume_percent);
        err = i2s_channel_write(s_i2s_tx, i2s_buf, read_len, &wrote_bytes, MUSIC_PLAYER_WRITE_TIMEOUT_MS);
        if (err != ESP_OK || wrote_bytes != read_len) {
            ESP_LOGW(
                MUSIC_PLAYER_TAG,
                "i2s write failed: err=%s wrote=%u len=%u enabled=%d rate=%" PRIu32,
                esp_err_to_name(err),
                (unsigned)wrote_bytes,
                (unsigned)read_len,
                s_i2s_enabled ? 1 : 0,
                s_i2s_sample_rate);
            xSemaphoreTake(ring->mutex, portMAX_DELAY);
            ring->output_err = (err != ESP_OK) ? err : ESP_FAIL;
            ring->stop = true;
            xSemaphoreGive(ring->mutex);
            xSemaphoreGive(ring->can_write);
            break;
        }
    }

    free(i2s_buf);
    ring->task = NULL;
    vTaskDelete(NULL);
}

static bool music_player_pcm_ring_start_output(music_player_pcm_ring_t *ring)
{
    if (ring == NULL) {
        return false;
    }
    if (ring->task != NULL) {
        return true;
    }
    return xTaskCreate(
               music_player_output_task,
               "music_pcm_out",
               MUSIC_PLAYER_OUTPUT_TASK_STACK_SIZE,
               ring,
               MUSIC_PLAYER_OUTPUT_TASK_PRIORITY,
               &ring->task) == pdPASS;
}

static bool music_player_pcm_ring_finish_and_wait(
    music_player_pcm_ring_t *ring,
    music_player_runtime_cmd_t *runtime)
{
    if (ring == NULL) {
        return true;
    }
    music_player_pcm_ring_set_finished(ring);
    while (ring->task != NULL) {
        if (runtime != NULL) {
            music_player_poll_runtime_cmds(runtime);
            if (runtime->volume_changed) {
                music_player_snapshot_lock();
                music_player_set_volume_locked(runtime->volume_percent);
                music_player_snapshot_unlock();
                music_player_pcm_ring_set_volume(ring, runtime->volume_percent);
                runtime->volume_changed = false;
            }
            if (runtime->stop_requested || runtime->switch_track) {
                music_player_pcm_ring_signal_stop(ring);
                return false;
            }
            music_player_pcm_ring_set_paused(ring, runtime->paused);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !music_player_pcm_ring_has_error(ring);
}

static void music_player_update_progress(
    music_player_source_t source,
    const char *path,
    uint32_t file_size,
    uint32_t position,
    uint32_t elapsed_ms,
    uint32_t duration_ms,
    uint32_t sample_rate,
    uint32_t bitrate,
    uint8_t channels,
    uint8_t bits_per_sample)
{
    music_player_snapshot_lock();
    s_snapshot.state = MUSIC_PLAYER_STATE_PLAYING;
    s_snapshot.source = source;
    snprintf(s_snapshot.path, sizeof(s_snapshot.path), "%s", path);
    music_player_copy_name_from_path(s_snapshot.name, sizeof(s_snapshot.name), path);
    s_snapshot.file_size = file_size;
    s_snapshot.position = position;
    s_snapshot.elapsed_ms = elapsed_ms;
    s_snapshot.duration_ms = duration_ms;
    s_snapshot.sample_rate = sample_rate;
    s_snapshot.bitrate = bitrate;
    s_snapshot.channels = channels;
    s_snapshot.bits_per_sample = bits_per_sample;
    s_snapshot.message[0] = '\0';
    music_player_snapshot_unlock();
}

static void music_player_apply_start_position(
    music_player_input_t *input,
    esp_audio_simple_dec_handle_t *decoder,
    esp_audio_simple_dec_type_t decoder_type,
    uint32_t target_position)
{
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = decoder_type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };

    if (input == NULL || decoder == NULL || *decoder == NULL || target_position == 0U) {
        return;
    }
    if (music_player_input_seek(input, target_position) != ESP_OK) {
        return;
    }
    esp_audio_simple_dec_close(*decoder);
    *decoder = NULL;
    if (esp_audio_simple_dec_open(&dec_cfg, decoder) != ESP_AUDIO_ERR_OK) {
        *decoder = NULL;
    }
}

static int16_t music_player_read_sample_as_s16(const uint8_t *src, uint8_t bits_per_sample)
{
    switch (bits_per_sample) {
        case 8:
            return (int16_t)(((int16_t)((int32_t)(*src) - 128)) << 8);
        case 16:
            return (int16_t)((int16_t)src[0] | ((int16_t)src[1] << 8));
        case 24: {
            int32_t value = ((int32_t)src[0]) |
                            ((int32_t)src[1] << 8) |
                            ((int32_t)src[2] << 16);
            if ((value & 0x00800000L) != 0) {
                value |= ~0x00FFFFFFL;
            }
            return (int16_t)(value >> 8);
        }
        case 32: {
            int32_t value = ((int32_t)src[0]) |
                            ((int32_t)src[1] << 8) |
                            ((int32_t)src[2] << 16) |
                            ((int32_t)src[3] << 24);
            return (int16_t)(value >> 16);
        }
        default:
            return 0;
    }
}

static size_t music_player_convert_frame_to_stereo16(
    const uint8_t *src,
    size_t src_size,
    uint8_t bits_per_sample,
    uint8_t channels,
    uint8_t volume_percent,
    uint8_t *dst,
    size_t dst_size)
{
    size_t bytes_per_sample = (size_t)((bits_per_sample + 7U) / 8U);
    size_t frame_bytes;
    size_t frame_count;
    int16_t *out = (int16_t *)dst;

    if (src == NULL || dst == NULL || channels == 0U || bytes_per_sample == 0U) {
        return 0U;
    }
    frame_bytes = bytes_per_sample * channels;
    if (frame_bytes == 0U) {
        return 0U;
    }
    frame_count = src_size / frame_bytes;
    if (frame_count * sizeof(int16_t) * 2U > dst_size) {
        frame_count = dst_size / (sizeof(int16_t) * 2U);
    }

    if (bits_per_sample == 16U && channels == 1U) {
        for (size_t i = 0; i < frame_count; ++i) {
            const uint8_t *frame = src + (i * 2U);
            int16_t sample = (int16_t)((int16_t)frame[0] | ((int16_t)frame[1] << 8));

            out[i * 2U] = sample;
            out[i * 2U + 1U] = sample;
        }
        return frame_count * sizeof(int16_t) * 2U;
    }

    if (bits_per_sample == 16U && channels == 2U) {
        for (size_t i = 0; i < frame_count; ++i) {
            const uint8_t *frame = src + (i * 4U);
            int32_t left = (int16_t)((int16_t)frame[0] | ((int16_t)frame[1] << 8));
            int32_t right = (int16_t)((int16_t)frame[2] | ((int16_t)frame[3] << 8));
            int16_t sample = (int16_t)((left + right) / 2);

            out[i * 2U] = sample;
            out[i * 2U + 1U] = sample;
        }
        return frame_count * sizeof(int16_t) * 2U;
    }

    for (size_t i = 0; i < frame_count; ++i) {
        const uint8_t *frame = src + (i * frame_bytes);
        int32_t sample = 0;

        if (channels == 1U) {
            sample = music_player_read_sample_as_s16(frame, bits_per_sample);
        } else {
            int32_t left = music_player_read_sample_as_s16(frame, bits_per_sample);
            int32_t right = music_player_read_sample_as_s16(frame + bytes_per_sample, bits_per_sample);
            sample = (left + right) / 2;
        }
        sample = (sample * (int32_t)volume_percent) / 100;
        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        out[i * 2U] = (int16_t)sample;
        out[i * 2U + 1U] = (int16_t)sample;
    }

    return frame_count * sizeof(int16_t) * 2U;
}

static void music_player_shift_input_buffer(uint8_t *buf, size_t *buf_len, uint32_t consumed)
{
    if (buf == NULL || buf_len == NULL || consumed == 0U || *buf_len == 0U) {
        return;
    }
    if (consumed >= *buf_len) {
        *buf_len = 0U;
        return;
    }
    memmove(buf, buf + consumed, *buf_len - consumed);
    *buf_len -= consumed;
}

static bool music_player_build_full_path(const char *rel_path, char *full_path, size_t full_path_len)
{
    int n;
    size_t mount_len;

    if (full_path == NULL || full_path_len == 0U) {
        return false;
    }
    if (rel_path == NULL || rel_path[0] == '\0') {
        n = snprintf(full_path, full_path_len, "%s", mount_point);
    } else if (rel_path[0] == '/') {
        mount_len = strlen(mount_point);
        if (strncmp(rel_path, mount_point, mount_len) == 0 &&
            (rel_path[mount_len] == '\0' || rel_path[mount_len] == '/')) {
            n = snprintf(full_path, full_path_len, "%s", rel_path);
        } else {
            n = snprintf(full_path, full_path_len, "%s%s", mount_point, rel_path);
        }
    } else {
        n = snprintf(full_path, full_path_len, "%s/%s", mount_point, rel_path);
    }
    return n > 0 && n < (int)full_path_len;
}

static void music_player_update_paused_state(music_player_source_t source, bool paused, const char *path)
{
    music_player_snapshot_lock();
    s_snapshot.state = paused ? MUSIC_PLAYER_STATE_PAUSED : MUSIC_PLAYER_STATE_PLAYING;
    s_snapshot.source = source;
    if (path != NULL) {
        snprintf(s_snapshot.path, sizeof(s_snapshot.path), "%s", path);
        music_player_copy_name_from_path(s_snapshot.name, sizeof(s_snapshot.name), path);
    }
    s_snapshot.message[0] = '\0';
    music_player_snapshot_unlock();
}

static void music_player_handle_runtime_cmd(const music_player_cmd_t *cmd, music_player_runtime_cmd_t *runtime)
{
    if (cmd == NULL || runtime == NULL) {
        return;
    }
    switch (cmd->type) {
        case MUSIC_PLAYER_CMD_STOP:
            runtime->stop_requested = true;
            break;
        case MUSIC_PLAYER_CMD_PLAY:
            runtime->switch_track = true;
            runtime->next_track = *cmd;
            break;
        case MUSIC_PLAYER_CMD_TOGGLE_PAUSE:
            runtime->paused = !runtime->paused;
            break;
        case MUSIC_PLAYER_CMD_SET_VOLUME:
            runtime->volume_percent = cmd->volume_percent;
            runtime->volume_changed = true;
            break;
        case MUSIC_PLAYER_CMD_SEEK_RELATIVE:
            runtime->seek_delta_bytes = cmd->seek_delta_bytes;
            runtime->seek_requested = true;
            break;
        default:
            break;
    }
}

static void music_player_poll_runtime_cmds(music_player_runtime_cmd_t *runtime)
{
    music_player_cmd_t cmd;

    if (runtime == NULL || s_music_queue == NULL) {
        return;
    }
    while (xQueueReceive(s_music_queue, &cmd, 0) == pdTRUE) {
        music_player_handle_runtime_cmd(&cmd, runtime);
    }
}

static esp_err_t music_player_wait_while_paused(
    music_player_source_t source,
    const char *path,
    music_player_runtime_cmd_t *runtime)
{
    music_player_cmd_t cmd;

    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    music_player_update_paused_state(source, true, path);

    while (runtime->paused && !runtime->stop_requested && !runtime->switch_track && !runtime->seek_requested) {
        if (xQueueReceive(s_music_queue, &cmd, pdMS_TO_TICKS(MUSIC_PLAYER_CMD_WAIT_MS)) == pdTRUE) {
            music_player_handle_runtime_cmd(&cmd, runtime);
        }
    }

    if (!runtime->paused && !runtime->stop_requested && !runtime->switch_track) {
        music_player_update_paused_state(source, false, path);
    }
    return ESP_OK;
}

static void music_player_input_close(music_player_input_t *input)
{
    if (input == NULL) {
        return;
    }
    if (input->tf_fp != NULL) {
        fclose(input->tf_fp);
        input->tf_fp = NULL;
    }
    if (input->smb_fp != NULL) {
        smb_client_file_close(input->smb_fp);
        input->smb_fp = NULL;
    }
}

static size_t music_player_input_read(music_player_input_t *input, uint8_t *buf, size_t len, bool *error_out)
{
    if (error_out != NULL) {
        *error_out = false;
    }
    if (input == NULL || buf == NULL || len == 0U) {
        if (error_out != NULL) {
            *error_out = true;
        }
        return 0U;
    }
    if (input->source == MUSIC_PLAYER_SOURCE_SMB) {
        int ret = smb_client_file_read(input->smb_fp, buf, len);
        if (ret < 0) {
            if (error_out != NULL) {
                *error_out = true;
            }
            return 0U;
        }
        return (size_t)ret;
    }

    size_t read_len = fread(buf, 1U, len, input->tf_fp);
    if (read_len == 0U && ferror(input->tf_fp)) {
        if (error_out != NULL) {
            *error_out = true;
        }
    }
    return read_len;
}

static esp_err_t music_player_input_seek(music_player_input_t *input, uint32_t target_position)
{
    if (input == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (input->source == MUSIC_PLAYER_SOURCE_SMB) {
        return smb_client_file_seek(input->smb_fp, target_position);
    }
    return (fseek(input->tf_fp, (long)target_position, SEEK_SET) == 0) ? ESP_OK : ESP_FAIL;
}

static bool music_player_read_at(music_player_input_t *input, uint32_t position, uint8_t *buf, size_t len)
{
    size_t done = 0U;

    if (input == NULL || buf == NULL) {
        return false;
    }
    if (music_player_input_seek(input, position) != ESP_OK) {
        return false;
    }
    while (done < len) {
        bool read_error = false;
        size_t read_now = music_player_input_read(input, buf + done, len - done, &read_error);

        if (read_error || read_now == 0U) {
            return false;
        }
        done += read_now;
    }
    return true;
}

static uint16_t music_player_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t music_player_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t music_player_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t music_player_read_synchsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7FU) << 21) |
           ((uint32_t)(p[1] & 0x7FU) << 14) |
           ((uint32_t)(p[2] & 0x7FU) << 7) |
           (uint32_t)(p[3] & 0x7FU);
}

static uint32_t music_player_duration_from_samples(uint32_t samples, uint32_t sample_rate)
{
    uint64_t ms;

    if (samples == 0U || sample_rate == 0U) {
        return 0U;
    }
    ms = (((uint64_t)samples * 1000ULL) + (uint64_t)(sample_rate / 2U)) / (uint64_t)sample_rate;
    return (ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
}

static bool music_player_sample_rate_plausible(uint32_t sample_rate)
{
    return sample_rate >= 8000U && sample_rate <= 192000U;
}

static bool music_player_parse_wav_info(
    music_player_input_t *input,
    uint32_t file_size,
    music_player_track_info_t *info)
{
    uint8_t header[40];
    uint32_t offset = 12U;
    uint32_t byte_rate = 0U;
    uint32_t data_size = 0U;
    bool have_fmt = false;

    if (file_size < 44U || info == NULL || !music_player_read_at(input, 0U, header, 12U)) {
        return false;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    while (offset + 8U <= file_size && offset < MUSIC_PLAYER_MP3_SCAN_LIMIT) {
        uint8_t chunk[8];
        uint32_t chunk_size;
        uint32_t next_offset;

        if (!music_player_read_at(input, offset, chunk, sizeof(chunk))) {
            break;
        }
        chunk_size = music_player_read_le32(chunk + 4);
        next_offset = offset + 8U + chunk_size + (chunk_size & 1U);
        if (next_offset <= offset || next_offset > file_size + 1U) {
            break;
        }

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16U) {
            size_t fmt_len = (chunk_size < sizeof(header)) ? chunk_size : sizeof(header);

            if (!music_player_read_at(input, offset + 8U, header, fmt_len)) {
                break;
            }
            info->channels = (uint8_t)music_player_read_le16(header + 2);
            info->sample_rate = music_player_read_le32(header + 4);
            byte_rate = music_player_read_le32(header + 8);
            info->bits_per_sample = (uint8_t)music_player_read_le16(header + 14);
            if (byte_rate > 0U) {
                info->bitrate = byte_rate * 8U;
            }
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_size = chunk_size;
            if (have_fmt) {
                break;
            }
        }
        offset = next_offset;
    }

    if (have_fmt && data_size > 0U && byte_rate > 0U) {
        uint64_t ms = (((uint64_t)data_size * 1000ULL) + (uint64_t)(byte_rate / 2U)) / (uint64_t)byte_rate;

        info->duration_ms = (ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
        return info->duration_ms > 0U;
    }
    return have_fmt;
}

static bool music_player_parse_mp3_frame_header(
    const uint8_t *h,
    uint32_t *bitrate_out,
    uint32_t *sample_rate_out,
    uint16_t *samples_per_frame_out,
    uint8_t *channels_out,
    uint8_t *version_id_out)
{
    static const uint16_t bitrate_mpeg1_l1[16] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0};
    static const uint16_t bitrate_mpeg1_l2[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    static const uint16_t bitrate_mpeg1_l3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t bitrate_mpeg2_l1[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0};
    static const uint16_t bitrate_mpeg2_l23[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint16_t sample_rates_mpeg1[3] = {44100, 48000, 32000};
    static const uint16_t sample_rates[3][3] = {
        {11025, 12000, 8000},
        {0, 0, 0},
        {22050, 24000, 16000},
    };
    uint32_t header = music_player_read_be32(h);
    uint8_t version_id = (uint8_t)((header >> 19) & 0x03U);
    uint8_t layer = (uint8_t)((header >> 17) & 0x03U);
    uint8_t bitrate_index = (uint8_t)((header >> 12) & 0x0FU);
    uint8_t sample_rate_index = (uint8_t)((header >> 10) & 0x03U);
    uint8_t channel_mode = (uint8_t)((header >> 6) & 0x03U);
    uint32_t sample_rate;
    uint32_t bitrate_kbps;

    if ((header & 0xFFE00000U) != 0xFFE00000U || version_id == 1U || layer == 0U ||
        bitrate_index == 0U || bitrate_index == 0x0FU || sample_rate_index == 0x03U) {
        return false;
    }

    if (version_id == 3U) {
        sample_rate = sample_rates_mpeg1[sample_rate_index];
        if (layer == 3U) {
            bitrate_kbps = bitrate_mpeg1_l1[bitrate_index];
            *samples_per_frame_out = 384U;
        } else if (layer == 2U) {
            bitrate_kbps = bitrate_mpeg1_l2[bitrate_index];
            *samples_per_frame_out = 1152U;
        } else {
            bitrate_kbps = bitrate_mpeg1_l3[bitrate_index];
            *samples_per_frame_out = 1152U;
        }
    } else {
        sample_rate = sample_rates[version_id][sample_rate_index];
        if (layer == 3U) {
            bitrate_kbps = bitrate_mpeg2_l1[bitrate_index];
            *samples_per_frame_out = 384U;
        } else {
            bitrate_kbps = bitrate_mpeg2_l23[bitrate_index];
            *samples_per_frame_out = (layer == 1U) ? 576U : 1152U;
        }
    }

    if (sample_rate == 0U || bitrate_kbps == 0U) {
        return false;
    }
    *bitrate_out = bitrate_kbps * 1000U;
    *sample_rate_out = sample_rate;
    *channels_out = (channel_mode == 3U) ? 1U : 2U;
    *version_id_out = version_id;
    return true;
}

static bool music_player_parse_mp3_info(
    music_player_input_t *input,
    uint32_t file_size,
    music_player_track_info_t *info)
{
    uint8_t header[64];
    uint32_t offset = 0U;
    uint32_t scan_end;

    if (file_size < 4U || info == NULL) {
        return false;
    }
    if (music_player_read_at(input, 0U, header, 10U) && memcmp(header, "ID3", 3) == 0) {
        offset = 10U + music_player_read_synchsafe32(header + 6);
        if ((header[5] & 0x10U) != 0U) {
            offset += 10U;
        }
    }
    scan_end = offset + MUSIC_PLAYER_MP3_SCAN_LIMIT;
    if (scan_end < offset || scan_end > file_size) {
        scan_end = file_size;
    }

    while (offset + 4U <= scan_end) {
        uint32_t bitrate = 0U;
        uint32_t sample_rate = 0U;
        uint16_t samples_per_frame = 0U;
        uint8_t channels = 0U;
        uint8_t version_id = 0U;

        if (!music_player_read_at(input, offset, header, 4U)) {
            return false;
        }
        if (!music_player_parse_mp3_frame_header(
                header,
                &bitrate,
                &sample_rate,
                &samples_per_frame,
                &channels,
                &version_id)) {
            offset++;
            continue;
        }

        info->bitrate = bitrate;
        info->sample_rate = sample_rate;
        info->channels = channels;
        info->bits_per_sample = 16U;

        {
            uint32_t side_info = (version_id == 3U) ? ((channels == 1U) ? 17U : 32U) : ((channels == 1U) ? 9U : 17U);
            uint32_t xing_offset = offset + 4U + side_info;
            uint32_t vbri_offset = offset + 36U;

            if (xing_offset + 16U <= file_size && music_player_read_at(input, xing_offset, header, 16U) &&
                (memcmp(header, "Xing", 4) == 0 || memcmp(header, "Info", 4) == 0)) {
                uint32_t flags = music_player_read_be32(header + 4);

                if ((flags & 0x01U) != 0U) {
                    uint32_t frames = music_player_read_be32(header + 8);
                    uint64_t total_samples = (uint64_t)frames * (uint64_t)samples_per_frame;

                    info->duration_ms = music_player_duration_from_samples(
                        (total_samples > UINT32_MAX) ? UINT32_MAX : (uint32_t)total_samples,
                        sample_rate);
                }
            }
            if (info->duration_ms == 0U && vbri_offset + 18U <= file_size &&
                music_player_read_at(input, vbri_offset, header, 18U) && memcmp(header, "VBRI", 4) == 0) {
                uint32_t frames = music_player_read_be32(header + 14);
                uint64_t total_samples = (uint64_t)frames * (uint64_t)samples_per_frame;

                info->duration_ms = music_player_duration_from_samples(
                    (total_samples > UINT32_MAX) ? UINT32_MAX : (uint32_t)total_samples,
                    sample_rate);
            }
        }

        if (info->duration_ms == 0U && bitrate > 0U && file_size > offset) {
            info->duration_ms = music_player_estimate_ms_from_bytes(file_size - offset, bitrate);
        }
        return true;
    }
    return false;
}

static void music_player_probe_track_info(
    music_player_input_t *input,
    esp_audio_simple_dec_type_t decoder_type,
    uint32_t file_size,
    music_player_track_info_t *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (input == NULL || file_size == 0U) {
        return;
    }
    if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) {
        (void)music_player_parse_wav_info(input, file_size, info);
    } else if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3) {
        (void)music_player_parse_mp3_info(input, file_size, info);
    }
    if (info->duration_ms > 0U && file_size > 0U) {
        uint64_t bitrate = ((uint64_t)file_size * 8000ULL) / (uint64_t)info->duration_ms;

        info->bitrate = (bitrate > UINT32_MAX) ? UINT32_MAX : (uint32_t)bitrate;
    }
    (void)music_player_input_seek(input, 0U);
}

static void music_player_cleanup_track(music_player_input_t *input, esp_audio_simple_dec_handle_t decoder)
{
    music_player_input_close(input);
    if (decoder != NULL) {
        esp_audio_simple_dec_close(decoder);
    }
    music_player_i2s_disable();
}

static bool music_player_bitrate_plausible(uint32_t bitrate)
{
    return bitrate >= 8000U && bitrate <= 2000000U;
}

static uint32_t music_player_estimate_ms_from_bytes(uint32_t bytes, uint32_t bitrate)
{
    uint64_t ms;

    if (bytes == 0U || !music_player_bitrate_plausible(bitrate)) {
        return 0U;
    }
    ms = (((uint64_t)bytes * 8000ULL) + (uint64_t)(bitrate / 2U)) / (uint64_t)bitrate;
    return (ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
}

static uint32_t music_player_now_ms(void)
{
    uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000LL);

    return (ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)ms;
}

static uint32_t music_player_elapsed_ms_from_clock(uint32_t anchor_ms, uint32_t anchor_time_ms)
{
    uint32_t now_ms = music_player_now_ms();
    uint64_t elapsed_ms = (uint64_t)anchor_ms + (uint64_t)(uint32_t)(now_ms - anchor_time_ms);

    return (elapsed_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)elapsed_ms;
}

static uint32_t music_player_clamp_elapsed_ms(uint32_t elapsed_ms, uint32_t duration_ms)
{
    return (duration_ms > 0U && elapsed_ms > duration_ms) ? duration_ms : elapsed_ms;
}

static uint32_t music_player_finished_elapsed_ms(uint32_t elapsed_ms, uint32_t duration_ms)
{
    return (duration_ms > 0U) ? duration_ms : elapsed_ms;
}

static void music_player_track_error(
    music_player_source_t source,
    const char *path,
    uint32_t file_size,
    uint32_t position,
    const char *message)
{
    uint32_t elapsed_ms = 0U;
    uint32_t duration_ms = 0U;
    uint32_t sample_rate = 0U;
    uint32_t bitrate = 0U;
    uint8_t channels = 0U;
    uint8_t bits_per_sample = 0U;

    ESP_LOGW(
        MUSIC_PLAYER_TAG,
        "track error: source=%d path=%s size=%" PRIu32 " pos=%" PRIu32 " msg=%s",
        (int)source,
        (path != NULL) ? path : "",
        file_size,
        position,
        (message != NULL) ? message : "");
    music_player_snapshot_lock();
    if (path != NULL && s_snapshot.source == source && strcmp(s_snapshot.path, path) == 0) {
        elapsed_ms = s_snapshot.elapsed_ms;
        duration_ms = s_snapshot.duration_ms;
        sample_rate = s_snapshot.sample_rate;
        bitrate = s_snapshot.bitrate;
        channels = s_snapshot.channels;
        bits_per_sample = s_snapshot.bits_per_sample;
    }
    music_player_snapshot_unlock();
    music_player_set_snapshot(
        MUSIC_PLAYER_STATE_ERROR,
        source,
        path,
        file_size,
        position,
        elapsed_ms,
        duration_ms,
        sample_rate,
        bitrate,
        channels,
        bits_per_sample,
        message);
}

static void music_player_track_finished(
    music_player_source_t source,
    const char *path,
    uint32_t file_size,
    uint32_t position,
    uint32_t elapsed_ms,
    uint32_t duration_ms,
    uint32_t sample_rate,
    uint32_t bitrate,
    uint8_t channels,
    uint8_t bits_per_sample)
{
    music_player_set_snapshot(
        MUSIC_PLAYER_STATE_FINISHED,
        source,
        path,
        file_size,
        position,
        elapsed_ms,
        duration_ms,
        sample_rate,
        bitrate,
        channels,
        bits_per_sample,
        NULL);
}

static uint32_t music_player_seek_target_position(uint32_t position, uint32_t file_size, int32_t delta_bytes)
{
    int64_t target = (int64_t)position + (int64_t)delta_bytes;

    if (target < 0) {
        target = 0;
    }
    if (file_size > 0U && (uint64_t)target >= (uint64_t)file_size) {
        target = (file_size > 1U) ? (int64_t)(file_size - 1U) : 0;
    }
    return (uint32_t)target;
}

static esp_err_t music_player_write_pcm_to_ring(
    const uint8_t *src,
    size_t src_size,
    uint8_t bits_per_sample,
    uint8_t channels,
    uint8_t *convert_buf,
    size_t convert_buf_size,
    music_player_pcm_ring_t *ring,
    music_player_runtime_cmd_t *runtime)
{
    size_t bytes_per_sample = (size_t)((bits_per_sample + 7U) / 8U);
    size_t frame_bytes = bytes_per_sample * channels;
    size_t src_offset = 0U;

    if (src == NULL || convert_buf == NULL || ring == NULL || src_size == 0U || frame_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    while (src_offset + frame_bytes <= src_size) {
        size_t converted_size;
        size_t consumed_frames;
        size_t consumed_bytes;

        converted_size = music_player_convert_frame_to_stereo16(
            src + src_offset,
            src_size - src_offset,
            bits_per_sample,
            channels,
            100U,
            convert_buf,
            convert_buf_size);
        if (converted_size == 0U) {
            return ESP_FAIL;
        }

        if (!music_player_pcm_ring_write(ring, convert_buf, converted_size, runtime)) {
            return ESP_ERR_INVALID_STATE;
        }

        consumed_frames = converted_size / (sizeof(int16_t) * 2U);
        consumed_bytes = consumed_frames * frame_bytes;
        if (consumed_bytes == 0U) {
            return ESP_FAIL;
        }
        src_offset += consumed_bytes;
    }

    return ESP_OK;
}

static esp_err_t music_player_run_track(
    const music_player_cmd_t *play_cmd,
    music_player_cmd_t *next_track_out,
    bool *has_next_track)
{
    char full_path[MUSIC_PLAYER_PATH_MAX + 32] = {0};
    music_player_input_t input = {0};
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_type_t decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    uint8_t *input_buf = NULL;
    uint8_t *decode_buf = NULL;
    uint8_t *i2s_buf = NULL;
    music_player_pcm_ring_t pcm_ring = {0};
    bool pcm_ring_ready = false;
    size_t decode_buf_size = MUSIC_PLAYER_DECODE_BUF_SIZE;
    size_t input_len = 0U;
    size_t read_total = 0U;
    bool eof = false;
    bool output_ready = false;
    uint32_t file_size = 0U;
    uint32_t position = 0U;
    uint32_t start_position = 0U;
    uint32_t anchor_position = 0U;
    uint32_t anchor_elapsed_ms = 0U;
    uint32_t anchor_time_ms = 0U;
    bool playback_clock_started = false;
    uint32_t current_elapsed_ms = 0U;
    uint32_t current_duration_ms = 0U;
    uint32_t current_sample_rate = 0U;
    uint32_t current_bitrate = 0U;
    uint32_t locked_sample_rate = 0U;
    uint32_t last_progress_update_ms = 0U;
    uint8_t current_channels = 0U;
    uint8_t current_bits_per_sample = 0U;
    music_player_track_info_t header_info = {0};
    int error_budget = MUSIC_PLAYER_ERROR_BUDGET;
    music_player_runtime_cmd_t runtime = {0};
    esp_err_t final_err = ESP_OK;

    if (has_next_track != NULL) {
        *has_next_track = false;
    }
    if (play_cmd == NULL || play_cmd->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    decoder_type = music_player_decoder_type_from_path(play_cmd->path);
    if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        music_player_track_error(play_cmd->source, play_cmd->path, 0U, 0U, "unsupported file");
        return ESP_ERR_NOT_SUPPORTED;
    }

    runtime.volume_percent = play_cmd->volume_percent;
    ESP_LOGI(
        MUSIC_PLAYER_TAG,
        "play request: source=%d path=%s size=%" PRIu32 " start=%" PRIu32 " volume=%u",
        (int)play_cmd->source,
        play_cmd->path,
        play_cmd->file_size,
        play_cmd->start_position,
        (unsigned)play_cmd->volume_percent);
    music_player_set_snapshot(
        MUSIC_PLAYER_STATE_LOADING,
        play_cmd->source,
        play_cmd->path,
        play_cmd->file_size,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        NULL);

    input.source = play_cmd->source;
    if (play_cmd->source == MUSIC_PLAYER_SOURCE_SMB) {
        uint64_t smb_size = 0;

        if (smb_client_open_file(play_cmd->path, &input.smb_fp, &smb_size) != ESP_OK) {
            music_player_track_error(play_cmd->source, play_cmd->path, 0U, 0U, "open smb file failed");
            return ESP_FAIL;
        }
        if (smb_size > 0U && smb_size <= UINT32_MAX) {
            file_size = (uint32_t)smb_size;
        }
    } else {
        if (!music_player_build_full_path(play_cmd->path, full_path, sizeof(full_path))) {
            music_player_track_error(play_cmd->source, play_cmd->path, 0U, 0U, "path too long");
            return ESP_FAIL;
        }

        input.tf_fp = fopen(full_path, "rb");
        if (input.tf_fp == NULL) {
            ESP_LOGW(MUSIC_PLAYER_TAG, "open failed: rel=%s full=%s", play_cmd->path, full_path);
            music_player_track_error(play_cmd->source, play_cmd->path, 0U, 0U, "open file failed");
            return ESP_FAIL;
        }
        ESP_LOGI(MUSIC_PLAYER_TAG, "opened TF file: %s", full_path);
    }

    input_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_INPUT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input_buf == NULL) {
        input_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_INPUT_BUF_SIZE, MALLOC_CAP_8BIT);
    }
    decode_buf = (uint8_t *)heap_caps_malloc(decode_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decode_buf == NULL) {
        decode_buf = (uint8_t *)heap_caps_malloc(decode_buf_size, MALLOC_CAP_8BIT);
    }
    i2s_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_I2S_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (i2s_buf == NULL) {
        i2s_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_I2S_BUF_SIZE, MALLOC_CAP_8BIT);
    }
    if (input_buf == NULL || decode_buf == NULL || i2s_buf == NULL ||
        !music_player_pcm_ring_init(&pcm_ring, MUSIC_PLAYER_PCM_RING_SIZE, runtime.volume_percent)) {
        music_player_track_error(play_cmd->source, play_cmd->path, 0U, 0U, "no memory");
        final_err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    pcm_ring_ready = true;

    if (play_cmd->file_size != 0U) {
        file_size = play_cmd->file_size;
    } else if (play_cmd->source == MUSIC_PLAYER_SOURCE_TF) {
        struct stat st;

        if (stat(full_path, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= UINT32_MAX) {
            file_size = (uint32_t)st.st_size;
        }
    }

    start_position = play_cmd->start_position;
    if (file_size > 0U && start_position >= file_size) {
        start_position = (file_size > 1U) ? (file_size - 1U) : 0U;
    }
    anchor_position = start_position;

    music_player_probe_track_info(&input, decoder_type, file_size, &header_info);
    current_duration_ms = header_info.duration_ms;
    current_sample_rate = header_info.sample_rate;
    current_bitrate = header_info.bitrate;
    current_channels = header_info.channels;
    current_bits_per_sample = header_info.bits_per_sample;
    if (current_duration_ms > 0U || current_bitrate > 0U || current_sample_rate > 0U) {
        music_player_set_snapshot(
            MUSIC_PLAYER_STATE_LOADING,
            play_cmd->source,
            play_cmd->path,
            file_size,
            0U,
            0U,
            current_duration_ms,
            current_sample_rate,
            current_bitrate,
            current_channels,
            current_bits_per_sample,
            NULL);
    }

    if (music_player_i2s_ensure_ready() != ESP_OK) {
        music_player_track_error(play_cmd->source, play_cmd->path, file_size, 0U, "i2s init failed");
        final_err = ESP_FAIL;
        goto cleanup;
    }

    if (esp_audio_simple_check_audio_type(decoder_type) != ESP_AUDIO_ERR_OK) {
        music_player_track_error(play_cmd->source, play_cmd->path, file_size, 0U, "decoder unavailable");
        final_err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    {
        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = decoder_type,
            .dec_cfg = NULL,
            .cfg_size = 0,
            .use_frame_dec = false,
        };

        if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK || decoder == NULL) {
            music_player_track_error(play_cmd->source, play_cmd->path, file_size, 0U, "decoder open failed");
            final_err = ESP_FAIL;
            goto cleanup;
        }
    }

    if (start_position > 0U) {
        music_player_apply_start_position(&input, &decoder, decoder_type, start_position);
        if (decoder == NULL) {
            music_player_track_error(play_cmd->source, play_cmd->path, file_size, 0U, "decoder reopen failed");
            final_err = ESP_FAIL;
            goto cleanup;
        }
        read_total = start_position;
        position = start_position;
        current_elapsed_ms = music_player_estimate_ms_from_bytes(anchor_position, current_bitrate);
        current_elapsed_ms = music_player_clamp_elapsed_ms(current_elapsed_ms, current_duration_ms);
        music_player_set_snapshot(
            MUSIC_PLAYER_STATE_LOADING,
            play_cmd->source,
            play_cmd->path,
            file_size,
            position,
            current_elapsed_ms,
            current_duration_ms,
            current_sample_rate,
            current_bitrate,
            current_channels,
            current_bits_per_sample,
            NULL);
    }

    while (!runtime.stop_requested && !runtime.switch_track) {
        esp_audio_simple_dec_raw_t raw = {0};
        esp_audio_simple_dec_out_t frame = {0};
        esp_audio_simple_dec_info_t dec_info = {0};
        esp_audio_err_t dec_ret;
        bool made_progress = false;
        uint32_t sample_rate;
        uint8_t bits_per_sample;
        uint8_t channels;

        music_player_poll_runtime_cmds(&runtime);
        if (runtime.volume_changed) {
            music_player_snapshot_lock();
            music_player_set_volume_locked(runtime.volume_percent);
            music_player_snapshot_unlock();
            music_player_pcm_ring_set_volume(&pcm_ring, runtime.volume_percent);
            runtime.volume_changed = false;
        }
        if (runtime.seek_requested) {
            uint32_t target_position = music_player_seek_target_position(position, file_size, runtime.seek_delta_bytes);

            runtime.seek_requested = false;
            runtime.seek_delta_bytes = 0;
            eof = false;
            input_len = 0U;
            read_total = target_position;
            position = target_position;
            anchor_position = target_position;
            anchor_elapsed_ms = music_player_estimate_ms_from_bytes(anchor_position, current_bitrate);
            anchor_time_ms = music_player_now_ms();
            playback_clock_started = false;
            current_elapsed_ms = anchor_elapsed_ms;
            current_elapsed_ms = music_player_clamp_elapsed_ms(current_elapsed_ms, current_duration_ms);
            last_progress_update_ms = 0U;
            music_player_pcm_ring_flush(&pcm_ring);
            if (music_player_input_seek(&input, target_position) != ESP_OK) {
                music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "seek failed");
                final_err = ESP_FAIL;
                break;
            }
            if (decoder != NULL) {
                esp_audio_simple_dec_close(decoder);
                decoder = NULL;
            }
            {
                esp_audio_simple_dec_cfg_t dec_cfg = {
                    .dec_type = decoder_type,
                    .dec_cfg = NULL,
                    .cfg_size = 0,
                    .use_frame_dec = false,
                };

                if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK || decoder == NULL) {
                    music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "decoder reopen failed");
                    final_err = ESP_FAIL;
                    break;
                }
            }
            music_player_set_snapshot(
                runtime.paused ? MUSIC_PLAYER_STATE_PAUSED : MUSIC_PLAYER_STATE_LOADING,
                play_cmd->source,
                play_cmd->path,
                file_size,
                position,
                current_elapsed_ms,
                current_duration_ms,
                current_sample_rate,
                current_bitrate,
                current_channels,
                current_bits_per_sample,
                NULL);
            continue;
        }
        if (runtime.paused) {
            if (playback_clock_started) {
                current_elapsed_ms = music_player_elapsed_ms_from_clock(anchor_elapsed_ms, anchor_time_ms);
                current_elapsed_ms = music_player_clamp_elapsed_ms(current_elapsed_ms, current_duration_ms);
                anchor_elapsed_ms = current_elapsed_ms;
            }
            music_player_pcm_ring_set_paused(&pcm_ring, true);
            if (music_player_wait_while_paused(play_cmd->source, play_cmd->path, &runtime) != ESP_OK) {
                music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "resume failed");
                final_err = ESP_FAIL;
                break;
            }
            if (!runtime.paused && !runtime.stop_requested && !runtime.switch_track) {
                anchor_time_ms = music_player_now_ms();
                playback_clock_started = true;
                music_player_pcm_ring_set_paused(&pcm_ring, false);
            }
            continue;
        }

        if (!eof && input_len < MUSIC_PLAYER_INPUT_LOW_WATER) {
            size_t free_space = MUSIC_PLAYER_INPUT_BUF_SIZE - input_len;
            if (free_space > 0U) {
                bool read_error = false;
                size_t read_now = music_player_input_read(&input, input_buf + input_len, free_space, &read_error);
                if (read_error) {
                    music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "read failed");
                    final_err = ESP_FAIL;
                    break;
                }
                if (read_now > 0U) {
                    input_len += read_now;
                    read_total += read_now;
                }
                if (read_now == 0U) {
                    eof = true;
                }
            }
        }

        raw.buffer = input_buf;
        raw.len = (uint32_t)input_len;
        raw.eos = eof;
        raw.consumed = 0U;
        frame.buffer = decode_buf;
        frame.len = (uint32_t)decode_buf_size;
        frame.needed_size = 0U;
        frame.decoded_size = 0U;

        dec_ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
        if (raw.consumed > 0U) {
            music_player_shift_input_buffer(input_buf, &input_len, raw.consumed);
            position = (uint32_t)(read_total - input_len);
            made_progress = true;
        }

        if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > decode_buf_size) {
            uint8_t *new_decode_buf =
                (uint8_t *)heap_caps_realloc(decode_buf, frame.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (new_decode_buf == NULL) {
                new_decode_buf =
                    (uint8_t *)heap_caps_realloc(decode_buf, frame.needed_size, MALLOC_CAP_8BIT);
            }
            if (new_decode_buf == NULL) {
                music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "decode buffer grow failed");
                final_err = ESP_ERR_NO_MEM;
                break;
            }
            decode_buf = new_decode_buf;
            decode_buf_size = frame.needed_size;
            continue;
        }

        if (frame.decoded_size > 0U) {
            if (esp_audio_simple_dec_get_info(decoder, &dec_info) != ESP_AUDIO_ERR_OK) {
                memset(&dec_info, 0, sizeof(dec_info));
            }
            if (locked_sample_rate == 0U && music_player_sample_rate_plausible(dec_info.sample_rate)) {
                locked_sample_rate = dec_info.sample_rate;
            }
            if (locked_sample_rate == 0U && music_player_sample_rate_plausible(current_sample_rate)) {
                locked_sample_rate = current_sample_rate;
            }
            if (locked_sample_rate == 0U) {
                locked_sample_rate = 44100U;
            }
            sample_rate = locked_sample_rate;
            bits_per_sample = (dec_info.bits_per_sample != 0U) ? dec_info.bits_per_sample : 16U;
            channels = (dec_info.channel != 0U) ? dec_info.channel : 2U;
            current_sample_rate = sample_rate;
            if (current_bitrate == 0U && music_player_bitrate_plausible(dec_info.bitrate)) {
                current_bitrate = dec_info.bitrate;
            } else if (current_bitrate == 0U && decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) {
                current_bitrate = sample_rate * (uint32_t)channels * (uint32_t)bits_per_sample;
            }
            if (current_duration_ms == 0U) {
                current_duration_ms = music_player_estimate_ms_from_bytes(file_size, current_bitrate);
            }
            current_channels = channels;
            current_bits_per_sample = bits_per_sample;

            if (!output_ready || s_i2s_sample_rate != sample_rate) {
                if (music_player_i2s_apply_rate(sample_rate) != ESP_OK) {
                    music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "i2s start failed");
                    final_err = ESP_FAIL;
                    break;
                }
                output_ready = true;
                if (!music_player_pcm_ring_start_output(&pcm_ring)) {
                    music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "audio output task failed");
                    final_err = ESP_ERR_NO_MEM;
                    break;
                }
            }

            if (music_player_write_pcm_to_ring(
                    frame.buffer,
                    frame.decoded_size,
                    bits_per_sample,
                    channels,
                    i2s_buf,
                    MUSIC_PLAYER_I2S_BUF_SIZE,
                    &pcm_ring,
                    &runtime) != ESP_OK) {
                if (runtime.stop_requested || runtime.switch_track || runtime.paused || runtime.seek_requested) {
                    made_progress = true;
                    continue;
                }
                {
                    esp_err_t output_err = music_player_pcm_ring_get_error(&pcm_ring);

                    if (output_err != ESP_OK) {
                        ESP_LOGW(MUSIC_PLAYER_TAG, "pcm ring output error: %s", esp_err_to_name(output_err));
                    }
                }
                music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "audio output failed");
                final_err = ESP_FAIL;
                break;
            }

            if (music_player_pcm_ring_has_error(&pcm_ring)) {
                esp_err_t output_err = music_player_pcm_ring_get_error(&pcm_ring);

                ESP_LOGW(MUSIC_PLAYER_TAG, "pcm ring output error: %s", esp_err_to_name(output_err));
                music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "audio output failed");
                final_err = ESP_FAIL;
                break;
            }

            if (!playback_clock_started) {
                anchor_elapsed_ms = music_player_estimate_ms_from_bytes(anchor_position, current_bitrate);
                anchor_elapsed_ms = music_player_clamp_elapsed_ms(anchor_elapsed_ms, current_duration_ms);
                anchor_time_ms = music_player_now_ms();
                playback_clock_started = true;
                current_elapsed_ms = anchor_elapsed_ms;
            } else {
                current_elapsed_ms = music_player_elapsed_ms_from_clock(anchor_elapsed_ms, anchor_time_ms);
            }
            current_elapsed_ms = music_player_clamp_elapsed_ms(current_elapsed_ms, current_duration_ms);

            {
                uint32_t now_ms = music_player_now_ms();

                if (last_progress_update_ms == 0U ||
                    (uint32_t)(now_ms - last_progress_update_ms) >= MUSIC_PLAYER_PROGRESS_UPDATE_MS) {
                    music_player_update_progress(
                        play_cmd->source,
                        play_cmd->path,
                        file_size,
                        position,
                        current_elapsed_ms,
                        current_duration_ms,
                        sample_rate,
                        current_bitrate,
                        channels,
                        bits_per_sample);
                    last_progress_update_ms = now_ms;
                }
            }
            error_budget = MUSIC_PLAYER_ERROR_BUDGET;
            made_progress = true;
        }

        if (dec_ret == ESP_AUDIO_ERR_DATA_LACK) {
            if (eof && input_len == 0U && frame.decoded_size == 0U) {
                if (!music_player_pcm_ring_finish_and_wait(&pcm_ring, &runtime)) {
                    break;
                }
                current_elapsed_ms = music_player_finished_elapsed_ms(current_elapsed_ms, current_duration_ms);
                music_player_track_finished(
                    play_cmd->source,
                    play_cmd->path,
                    file_size,
                    position,
                    current_elapsed_ms,
                    current_duration_ms,
                    current_sample_rate,
                    current_bitrate,
                    current_channels,
                    current_bits_per_sample);
                break;
            }
            continue;
        }

        if (dec_ret != ESP_AUDIO_ERR_OK) {
            if (eof && input_len == 0U && frame.decoded_size == 0U) {
                if (!music_player_pcm_ring_finish_and_wait(&pcm_ring, &runtime)) {
                    break;
                }
                current_elapsed_ms = music_player_finished_elapsed_ms(current_elapsed_ms, current_duration_ms);
                music_player_track_finished(
                    play_cmd->source,
                    play_cmd->path,
                    file_size,
                    position,
                    current_elapsed_ms,
                    current_duration_ms,
                    current_sample_rate,
                    current_bitrate,
                    current_channels,
                    current_bits_per_sample);
                break;
            }
            if (raw.consumed == 0U && input_len > 0U) {
                music_player_shift_input_buffer(input_buf, &input_len, 1U);
                position = (uint32_t)(read_total - input_len);
            }
            error_budget--;
            if (error_budget <= 0) {
                music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "decode failed");
                final_err = ESP_FAIL;
                break;
            }
            continue;
        }

        if (!made_progress) {
            if (eof && input_len == 0U) {
                if (!music_player_pcm_ring_finish_and_wait(&pcm_ring, &runtime)) {
                    break;
                }
                current_elapsed_ms = music_player_finished_elapsed_ms(current_elapsed_ms, current_duration_ms);
                music_player_track_finished(
                    play_cmd->source,
                    play_cmd->path,
                    file_size,
                    position,
                    current_elapsed_ms,
                    current_duration_ms,
                    current_sample_rate,
                    current_bitrate,
                    current_channels,
                    current_bits_per_sample);
                break;
            }
            if (!eof && input_len < MUSIC_PLAYER_INPUT_BUF_SIZE) {
                continue;
            }
            if (input_len > 0U) {
                music_player_shift_input_buffer(input_buf, &input_len, 1U);
                position = (uint32_t)(read_total - input_len);
            }
            error_budget--;
            if (error_budget <= 0) {
                music_player_track_error(play_cmd->source, play_cmd->path, file_size, position, "decode stalled");
                final_err = ESP_FAIL;
                break;
            }
        }
    }

cleanup:
    if (runtime.switch_track && has_next_track != NULL && next_track_out != NULL) {
        *next_track_out = runtime.next_track;
        *has_next_track = true;
    } else if (runtime.stop_requested) {
        music_player_set_snapshot(
            MUSIC_PLAYER_STATE_IDLE,
            play_cmd->source,
            play_cmd->path,
            file_size,
            position,
            current_elapsed_ms,
            current_duration_ms,
            current_sample_rate,
            current_bitrate,
            current_channels,
            current_bits_per_sample,
            NULL);
    }
    if (pcm_ring_ready) {
        music_player_pcm_ring_destroy(&pcm_ring);
    }
    music_player_cleanup_track(&input, decoder);
    free(input_buf);
    free(decode_buf);
    free(i2s_buf);
    return final_err;
}

static void music_player_task(void *arg)
{
    music_player_cmd_t cmd;

    (void)arg;

    for (;;) {
        bool has_next_track = false;
        music_player_cmd_t next_track = {0};

        if (xQueueReceive(s_music_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (cmd.type) {
            case MUSIC_PLAYER_CMD_SET_VOLUME:
                music_player_snapshot_lock();
                music_player_set_volume_locked(cmd.volume_percent);
                music_player_snapshot_unlock();
                break;
            case MUSIC_PLAYER_CMD_SEEK_RELATIVE:
                break;
            case MUSIC_PLAYER_CMD_TOGGLE_PAUSE:
            case MUSIC_PLAYER_CMD_STOP:
                music_player_set_snapshot(
                    MUSIC_PLAYER_STATE_IDLE,
                    s_snapshot.source,
                    s_snapshot.path,
                    s_snapshot.file_size,
                    s_snapshot.position,
                    s_snapshot.elapsed_ms,
                    s_snapshot.duration_ms,
                    s_snapshot.sample_rate,
                    s_snapshot.bitrate,
                    s_snapshot.channels,
                    s_snapshot.bits_per_sample,
                    NULL);
                music_player_i2s_disable();
                break;
            case MUSIC_PLAYER_CMD_PLAY:
                do {
                    (void)music_player_run_track(&cmd, &next_track, &has_next_track);
                    if (has_next_track) {
                        cmd = next_track;
                    }
                } while (has_next_track);
                break;
            default:
                break;
        }
    }
}

esp_err_t music_player_init(void)
{
    esp_audio_err_t audio_err;

    if (s_music_inited) {
        return ESP_OK;
    }

    if (!s_decoder_registry_ready) {
        audio_err = esp_audio_dec_register_default();
        if (audio_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(MUSIC_PLAYER_TAG, "esp_audio_dec_register_default failed: %d", (int)audio_err);
            return ESP_FAIL;
        }
        audio_err = esp_audio_simple_dec_register_default();
        if (audio_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(MUSIC_PLAYER_TAG, "esp_audio_simple_dec_register_default failed: %d", (int)audio_err);
            return ESP_FAIL;
        }
        s_decoder_registry_ready = true;
    }

    s_music_lock = xSemaphoreCreateMutex();
    s_music_queue = xQueueCreate(MUSIC_PLAYER_QUEUE_LEN, sizeof(music_player_cmd_t));
    if (s_music_lock == NULL || s_music_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(
            music_player_task,
            "music_player",
            MUSIC_PLAYER_TASK_STACK_SIZE,
            NULL,
            MUSIC_PLAYER_TASK_PRIORITY,
            &s_music_task,
            tskNO_AFFINITY) != pdPASS) {
        s_music_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_music_inited = true;
    return ESP_OK;
}

esp_err_t music_player_play(const char *rel_path, uint32_t file_size)
{
    music_player_cmd_t cmd = {
        .type = MUSIC_PLAYER_CMD_PLAY,
        .source = MUSIC_PLAYER_SOURCE_TF,
        .file_size = file_size,
        .start_position = 0U,
        .volume_percent = MUSIC_PLAYER_DEFAULT_VOLUME,
    };

    if (rel_path == NULL || rel_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_player_init() != ESP_OK) {
        return ESP_FAIL;
    }

    music_player_snapshot_lock();
    cmd.volume_percent = music_player_get_volume_locked();
    music_player_snapshot_unlock();
    snprintf(cmd.path, sizeof(cmd.path), "%s", rel_path);

    if (xQueueSend(s_music_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_player_play_from_position(const char *rel_path, uint32_t file_size, uint32_t position)
{
    music_player_cmd_t cmd = {
        .type = MUSIC_PLAYER_CMD_PLAY,
        .source = MUSIC_PLAYER_SOURCE_TF,
        .file_size = file_size,
        .start_position = position,
        .volume_percent = MUSIC_PLAYER_DEFAULT_VOLUME,
    };

    if (rel_path == NULL || rel_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_player_init() != ESP_OK) {
        return ESP_FAIL;
    }

    music_player_snapshot_lock();
    cmd.volume_percent = music_player_get_volume_locked();
    music_player_snapshot_unlock();
    snprintf(cmd.path, sizeof(cmd.path), "%s", rel_path);

    if (xQueueSend(s_music_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_player_play_smb(const char *path, uint32_t file_size)
{
    music_player_cmd_t cmd = {
        .type = MUSIC_PLAYER_CMD_PLAY,
        .source = MUSIC_PLAYER_SOURCE_SMB,
        .file_size = file_size,
        .volume_percent = MUSIC_PLAYER_DEFAULT_VOLUME,
    };

    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!smb_client_normalize_path(path, cmd.path, sizeof(cmd.path), false)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_player_init() != ESP_OK) {
        return ESP_FAIL;
    }

    music_player_snapshot_lock();
    cmd.volume_percent = music_player_get_volume_locked();
    music_player_snapshot_unlock();

    if (xQueueSend(s_music_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_player_stop(void)
{
    music_player_cmd_t cmd = {.type = MUSIC_PLAYER_CMD_STOP};

    if (!s_music_inited) {
        return ESP_OK;
    }
    if (xQueueSend(s_music_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_player_toggle_pause(void)
{
    music_player_cmd_t cmd = {.type = MUSIC_PLAYER_CMD_TOGGLE_PAUSE};

    if (!s_music_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_music_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_player_seek_relative(int32_t delta_bytes)
{
    music_player_cmd_t cmd = {.type = MUSIC_PLAYER_CMD_SEEK_RELATIVE, .seek_delta_bytes = delta_bytes};
    music_player_snapshot_t snapshot = {0};

    if (delta_bytes == 0) {
        return ESP_OK;
    }
    if (!s_music_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    music_player_get_snapshot(&snapshot);
    if (snapshot.state == MUSIC_PLAYER_STATE_IDLE ||
        snapshot.state == MUSIC_PLAYER_STATE_FINISHED ||
        snapshot.state == MUSIC_PLAYER_STATE_ERROR ||
        snapshot.path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_music_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_player_set_volume(uint8_t volume_percent)
{
    music_player_cmd_t cmd = {.type = MUSIC_PLAYER_CMD_SET_VOLUME};

    if (volume_percent > 100U) {
        volume_percent = 100U;
    }

    music_player_snapshot_lock();
    music_player_set_volume_locked(volume_percent);
    music_player_snapshot_unlock();

    if (!s_music_inited) {
        return ESP_OK;
    }

    cmd.volume_percent = volume_percent;
    if (xQueueSend(s_music_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void music_player_get_snapshot(music_player_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    if (!s_music_inited) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = MUSIC_PLAYER_STATE_IDLE;
        snapshot->volume_percent = s_snapshot.volume_percent;
        return;
    }

    music_player_snapshot_lock();
    *snapshot = s_snapshot;
    music_player_snapshot_unlock();
}
