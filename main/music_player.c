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
#include "file_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pin_map.h"

#define MUSIC_PLAYER_TAG "music_player"
#define MUSIC_PLAYER_TASK_STACK_SIZE 8192
#define MUSIC_PLAYER_TASK_PRIORITY 4
#define MUSIC_PLAYER_QUEUE_LEN 8
#define MUSIC_PLAYER_CMD_WAIT_MS 50
#define MUSIC_PLAYER_DEFAULT_VOLUME 60
#define MUSIC_PLAYER_INPUT_BUF_SIZE 8192
#define MUSIC_PLAYER_INPUT_LOW_WATER 2048
#define MUSIC_PLAYER_DECODE_BUF_SIZE 8192
#define MUSIC_PLAYER_I2S_BUF_SIZE 8192
#define MUSIC_PLAYER_WRITE_TIMEOUT_MS 1000
#define MUSIC_PLAYER_ERROR_BUDGET 64

typedef enum {
    MUSIC_PLAYER_CMD_PLAY = 0,
    MUSIC_PLAYER_CMD_STOP,
    MUSIC_PLAYER_CMD_TOGGLE_PAUSE,
    MUSIC_PLAYER_CMD_SET_VOLUME,
} music_player_cmd_type_t;

typedef struct {
    music_player_cmd_type_t type;
    char path[MUSIC_PLAYER_PATH_MAX];
    uint32_t file_size;
    uint8_t volume_percent;
} music_player_cmd_t;

typedef struct {
    bool stop_requested;
    bool switch_track;
    bool paused;
    bool volume_changed;
    uint8_t volume_percent;
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
    const char *path,
    uint32_t file_size,
    uint32_t position,
    uint32_t sample_rate,
    uint8_t channels,
    uint8_t bits_per_sample,
    const char *message)
{
    music_player_snapshot_lock();
    s_snapshot.state = state;
    if (path != NULL) {
        snprintf(s_snapshot.path, sizeof(s_snapshot.path), "%s", path);
        music_player_copy_name_from_path(s_snapshot.name, sizeof(s_snapshot.name), path);
    }
    s_snapshot.file_size = file_size;
    s_snapshot.position = position;
    s_snapshot.sample_rate = sample_rate;
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

    if (sample_rate == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    err = music_player_i2s_ensure_ready();
    if (err != ESP_OK) {
        return err;
    }

    if (s_i2s_enabled) {
        music_player_i2s_disable();
    }
    if (s_i2s_sample_rate != sample_rate) {
        clk_cfg = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        err = i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(
                MUSIC_PLAYER_TAG,
                "i2s reconfig sample_rate=%" PRIu32 " failed: %s",
                sample_rate,
                esp_err_to_name(err));
            return err;
        }
        s_i2s_sample_rate = sample_rate;
    }
    return music_player_i2s_enable();
}

static void music_player_update_progress(
    const char *path,
    uint32_t file_size,
    uint32_t position,
    uint32_t sample_rate,
    uint8_t channels,
    uint8_t bits_per_sample)
{
    music_player_snapshot_lock();
    s_snapshot.state = MUSIC_PLAYER_STATE_PLAYING;
    snprintf(s_snapshot.path, sizeof(s_snapshot.path), "%s", path);
    music_player_copy_name_from_path(s_snapshot.name, sizeof(s_snapshot.name), path);
    s_snapshot.file_size = file_size;
    s_snapshot.position = position;
    s_snapshot.sample_rate = sample_rate;
    s_snapshot.channels = channels;
    s_snapshot.bits_per_sample = bits_per_sample;
    s_snapshot.message[0] = '\0';
    music_player_snapshot_unlock();
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

    if (full_path == NULL || full_path_len == 0U) {
        return false;
    }
    if (rel_path == NULL || rel_path[0] == '\0') {
        n = snprintf(full_path, full_path_len, "%s", mount_point);
    } else {
        n = snprintf(full_path, full_path_len, "%s/%s", mount_point, rel_path);
    }
    return n > 0 && n < (int)full_path_len;
}

static void music_player_update_paused_state(bool paused, const char *path)
{
    music_player_snapshot_lock();
    s_snapshot.state = paused ? MUSIC_PLAYER_STATE_PAUSED : MUSIC_PLAYER_STATE_PLAYING;
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

static esp_err_t music_player_wait_while_paused(const char *path, music_player_runtime_cmd_t *runtime)
{
    music_player_cmd_t cmd;

    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    music_player_update_paused_state(true, path);
    music_player_i2s_disable();

    while (runtime->paused && !runtime->stop_requested && !runtime->switch_track) {
        if (xQueueReceive(s_music_queue, &cmd, pdMS_TO_TICKS(MUSIC_PLAYER_CMD_WAIT_MS)) == pdTRUE) {
            music_player_handle_runtime_cmd(&cmd, runtime);
        }
    }

    if (!runtime->paused && !runtime->stop_requested && !runtime->switch_track) {
        music_player_update_paused_state(false, path);
        return music_player_i2s_enable();
    }
    return ESP_OK;
}

static void music_player_cleanup_track(FILE *fp, esp_audio_simple_dec_handle_t decoder)
{
    if (fp != NULL) {
        fclose(fp);
    }
    if (decoder != NULL) {
        esp_audio_simple_dec_close(decoder);
    }
    music_player_i2s_disable();
}

static void music_player_track_error(const char *path, uint32_t file_size, uint32_t position, const char *message)
{
    music_player_set_snapshot(MUSIC_PLAYER_STATE_ERROR, path, file_size, position, 0U, 0U, 0U, message);
}

static void music_player_track_finished(const char *path, uint32_t file_size, uint32_t position)
{
    music_player_set_snapshot(MUSIC_PLAYER_STATE_FINISHED, path, file_size, position, 0U, 0U, 0U, NULL);
}

static esp_err_t music_player_write_pcm_to_i2s(
    const uint8_t *src,
    size_t src_size,
    uint8_t bits_per_sample,
    uint8_t channels,
    uint8_t volume_percent,
    uint8_t *i2s_buf,
    size_t i2s_buf_size)
{
    size_t bytes_per_sample = (size_t)((bits_per_sample + 7U) / 8U);
    size_t frame_bytes = bytes_per_sample * channels;
    size_t src_offset = 0U;

    if (src == NULL || i2s_buf == NULL || src_size == 0U || frame_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    while (src_offset + frame_bytes <= src_size) {
        size_t converted_size;
        size_t consumed_frames;
        size_t consumed_bytes;
        size_t wrote_bytes = 0U;

        converted_size = music_player_convert_frame_to_stereo16(
            src + src_offset,
            src_size - src_offset,
            bits_per_sample,
            channels,
            volume_percent,
            i2s_buf,
            i2s_buf_size);
        if (converted_size == 0U) {
            return ESP_FAIL;
        }

        if (i2s_channel_write(s_i2s_tx, i2s_buf, converted_size, &wrote_bytes, MUSIC_PLAYER_WRITE_TIMEOUT_MS) !=
                ESP_OK ||
            wrote_bytes != converted_size) {
            return ESP_FAIL;
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
    FILE *fp = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_type_t decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    uint8_t *input_buf = NULL;
    uint8_t *decode_buf = NULL;
    uint8_t *i2s_buf = NULL;
    size_t decode_buf_size = MUSIC_PLAYER_DECODE_BUF_SIZE;
    size_t input_len = 0U;
    size_t read_total = 0U;
    bool eof = false;
    bool output_ready = false;
    uint32_t file_size = 0U;
    uint32_t position = 0U;
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
        music_player_track_error(play_cmd->path, 0U, 0U, "unsupported file");
        return ESP_ERR_NOT_SUPPORTED;
    }

    runtime.volume_percent = play_cmd->volume_percent;
    music_player_set_snapshot(
        MUSIC_PLAYER_STATE_LOADING,
        play_cmd->path,
        play_cmd->file_size,
        0U,
        0U,
        0U,
        0U,
        NULL);

    if (!music_player_build_full_path(play_cmd->path, full_path, sizeof(full_path))) {
        music_player_track_error(play_cmd->path, 0U, 0U, "path too long");
        return ESP_FAIL;
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        music_player_track_error(play_cmd->path, 0U, 0U, "open file failed");
        return ESP_FAIL;
    }

    input_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_INPUT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input_buf == NULL) {
        input_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_INPUT_BUF_SIZE, MALLOC_CAP_8BIT);
    }
    decode_buf = (uint8_t *)heap_caps_malloc(decode_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decode_buf == NULL) {
        decode_buf = (uint8_t *)heap_caps_malloc(decode_buf_size, MALLOC_CAP_8BIT);
    }
    i2s_buf = (uint8_t *)heap_caps_malloc(MUSIC_PLAYER_I2S_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (input_buf == NULL || decode_buf == NULL || i2s_buf == NULL) {
        music_player_track_error(play_cmd->path, 0U, 0U, "no memory");
        final_err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    if (play_cmd->file_size != 0U) {
        file_size = play_cmd->file_size;
    } else {
        struct stat st;

        if (stat(full_path, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= UINT32_MAX) {
            file_size = (uint32_t)st.st_size;
        }
    }

    if (music_player_i2s_ensure_ready() != ESP_OK) {
        music_player_track_error(play_cmd->path, file_size, 0U, "i2s init failed");
        final_err = ESP_FAIL;
        goto cleanup;
    }

    if (esp_audio_simple_check_audio_type(decoder_type) != ESP_AUDIO_ERR_OK) {
        music_player_track_error(play_cmd->path, file_size, 0U, "decoder unavailable");
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
            music_player_track_error(play_cmd->path, file_size, 0U, "decoder open failed");
            final_err = ESP_FAIL;
            goto cleanup;
        }
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
            runtime.volume_changed = false;
        }
        if (runtime.paused) {
            if (music_player_wait_while_paused(play_cmd->path, &runtime) != ESP_OK) {
                music_player_track_error(play_cmd->path, file_size, position, "resume failed");
                final_err = ESP_FAIL;
                break;
            }
            continue;
        }

        if (!eof && input_len < MUSIC_PLAYER_INPUT_LOW_WATER) {
            size_t free_space = MUSIC_PLAYER_INPUT_BUF_SIZE - input_len;
            if (free_space > 0U) {
                size_t read_now = fread(input_buf + input_len, 1U, free_space, fp);
                if (read_now > 0U) {
                    input_len += read_now;
                    read_total += read_now;
                }
                if (read_now == 0U && feof(fp)) {
                    eof = true;
                }
            } else if (feof(fp)) {
                eof = true;
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
                music_player_track_error(play_cmd->path, file_size, position, "decode buffer grow failed");
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
            sample_rate = (dec_info.sample_rate != 0U) ? dec_info.sample_rate : 44100U;
            bits_per_sample = (dec_info.bits_per_sample != 0U) ? dec_info.bits_per_sample : 16U;
            channels = (dec_info.channel != 0U) ? dec_info.channel : 2U;

            if (!output_ready || s_i2s_sample_rate != sample_rate) {
                if (music_player_i2s_apply_rate(sample_rate) != ESP_OK) {
                    music_player_track_error(play_cmd->path, file_size, position, "i2s start failed");
                    final_err = ESP_FAIL;
                    break;
                }
                output_ready = true;
            }

            if (music_player_write_pcm_to_i2s(
                    frame.buffer,
                    frame.decoded_size,
                    bits_per_sample,
                    channels,
                    runtime.volume_percent,
                    i2s_buf,
                    MUSIC_PLAYER_I2S_BUF_SIZE) != ESP_OK) {
                music_player_track_error(play_cmd->path, file_size, position, "audio output failed");
                final_err = ESP_FAIL;
                break;
            }

            music_player_update_progress(
                play_cmd->path,
                file_size,
                position,
                sample_rate,
                channels,
                bits_per_sample);
            error_budget = MUSIC_PLAYER_ERROR_BUDGET;
            made_progress = true;
        }

        if (dec_ret == ESP_AUDIO_ERR_DATA_LACK) {
            if (eof && input_len == 0U && frame.decoded_size == 0U) {
                music_player_track_finished(play_cmd->path, file_size, position);
                break;
            }
            continue;
        }

        if (dec_ret != ESP_AUDIO_ERR_OK) {
            if (eof && input_len == 0U && frame.decoded_size == 0U) {
                music_player_track_finished(play_cmd->path, file_size, position);
                break;
            }
            if (raw.consumed == 0U && input_len > 0U) {
                music_player_shift_input_buffer(input_buf, &input_len, 1U);
                position = (uint32_t)(read_total - input_len);
            }
            error_budget--;
            if (error_budget <= 0) {
                music_player_track_error(play_cmd->path, file_size, position, "decode failed");
                final_err = ESP_FAIL;
                break;
            }
            continue;
        }

        if (!made_progress) {
            if (eof && input_len == 0U) {
                music_player_track_finished(play_cmd->path, file_size, position);
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
                music_player_track_error(play_cmd->path, file_size, position, "decode stalled");
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
            play_cmd->path,
            file_size,
            position,
            0U,
            0U,
            0U,
            NULL);
    }
    music_player_cleanup_track(fp, decoder);
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
            case MUSIC_PLAYER_CMD_TOGGLE_PAUSE:
            case MUSIC_PLAYER_CMD_STOP:
                music_player_set_snapshot(
                    MUSIC_PLAYER_STATE_IDLE,
                    s_snapshot.path,
                    s_snapshot.file_size,
                    s_snapshot.position,
                    0U,
                    0U,
                    0U,
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
        .file_size = file_size,
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

esp_err_t music_player_set_volume(uint8_t volume_percent)
{
    music_player_cmd_t cmd = {.type = MUSIC_PLAYER_CMD_SET_VOLUME};

    if (volume_percent > 100U) {
        volume_percent = 100U;
    }
    if (!s_music_inited && music_player_init() != ESP_OK) {
        return ESP_FAIL;
    }

    music_player_snapshot_lock();
    music_player_set_volume_locked(volume_percent);
    music_player_snapshot_unlock();

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
