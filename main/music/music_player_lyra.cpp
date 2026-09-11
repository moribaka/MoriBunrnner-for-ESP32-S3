// Existing MoriBurnner C/UI API backed by coderkei/lyra-firmware audio.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lyra/lyra_audio.h"
#include "music_player.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
extern "C" {
#include "smb_client.h"
#include "usb_msc_tf.h"
}
namespace {
StaticSemaphore_t mutex_storage;
SemaphoreHandle_t mutex;
bool ready;
uint8_t volume = 1;
music_player_source_t source = MUSIC_PLAYER_SOURCE_TF;
uint32_t file_size;
uint32_t burn_owners;
bool saved_pause;
lyra::audio::Status paused_track;
void lock() {
    if (!mutex)
        mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    xSemaphoreTake(mutex, portMAX_DELAY);
}
void unlock() { xSemaphoreGive(mutex); }
esp_err_t begin(const char *path, uint32_t size, uint32_t byte_position,
                music_player_source_t kind) {
    if (!path || !*path)
        return ESP_ERR_INVALID_ARG;
    if (std::strlen(path) >= MUSIC_PLAYER_PATH_MAX)
        return ESP_ERR_INVALID_SIZE;
    char full[lyra::audio::kMaxPath];
    const char *prefix = kind == MUSIC_PLAYER_SOURCE_SMB          ? "/smb/"
                         : std::strncmp(path, "/sdcard/", 8) == 0 ? ""
                                                                  : "/sdcard/";
    const char *tail = path;
    if (*prefix)
        while (*tail == '/')
            ++tail;
    if (std::snprintf(full, sizeof(full), "%s%s", prefix, tail) >= (int)sizeof(full))
        return ESP_ERR_INVALID_SIZE;
    if (kind == MUSIC_PLAYER_SOURCE_TF && usb_msc_tf_in_use_by_host())
        return ESP_ERR_INVALID_STATE;
    if (!size && kind == MUSIC_PLAYER_SOURCE_TF) {
        struct stat st;
        if (stat(full, &st) == 0)
            size = st.st_size;
    }
    esp_err_t err = music_player_init();
    if (err != ESP_OK)
        return err;
    lock();
    if (burn_owners) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    err = lyra::audio::play_from(full, 0, byte_position);
    if (err == ESP_OK) {
        saved_pause = false;
        source = kind;
        file_size = size;
    }
    unlock();
    return err;
}
} // namespace
extern "C" esp_err_t music_player_init() {
    lock();
    if (burn_owners) { unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = ESP_OK;
    if (!ready) {
        err = lyra::audio::init();
        if (err == ESP_OK) {
            ready = true;
            lyra::audio::set_maximum_volume_percent(100);
            lyra::audio::set_volume(volume);
        }
    }
    unlock();
    return err;
}
extern "C" esp_err_t music_player_play(const char *p, uint32_t n) {
    return begin(p, n, 0, MUSIC_PLAYER_SOURCE_TF);
}
extern "C" esp_err_t music_player_play_from_position(const char *p, uint32_t n, uint32_t pos) {
    return begin(p, n, pos, MUSIC_PLAYER_SOURCE_TF);
}
extern "C" esp_err_t music_player_play_smb(const char *p, uint32_t n) {
    return begin(p, n, 0, MUSIC_PLAYER_SOURCE_SMB);
}
extern "C" esp_err_t music_player_stop() {
    lock();
    saved_pause = false;
    auto e = ready ? lyra::audio::stop() : ESP_OK;
    unlock();
    return e;
}
extern "C" esp_err_t music_player_toggle_pause() {
    lock();
    if (burn_owners) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (saved_pause) {
        auto e = lyra::audio::play_from(paused_track.path, paused_track.position_ms);
        if (e == ESP_OK) saved_pause = false;
        unlock();
        return e;
    }
    auto e = ready ? lyra::audio::toggle_pause() : ESP_ERR_INVALID_STATE;
    unlock();
    return e;
}
extern "C" esp_err_t music_player_set_volume(uint8_t v) {
    lock();
    volume = std::min<uint8_t>(v, 100);
    auto e = ready ? lyra::audio::set_volume(volume) : ESP_OK;
    unlock();
    return e;
}
extern "C" esp_err_t music_player_seek_relative(int32_t delta) {
    lock();
    if (!ready || burn_owners || saved_pause) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    auto s = lyra::audio::status();
    if (!file_size || !s.duration_ms) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    int64_t target = static_cast<int64_t>(s.position_ms) +
                     static_cast<int64_t>(delta) * s.duration_ms / file_size;
    auto e = lyra::audio::seek(std::max<int64_t>(target, 0));
    unlock();
    return e;
}
extern "C" void music_player_get_snapshot(music_player_snapshot_t *out) {
    if (!out)
        return;
    lock();
    *out = {};
    out->volume_percent = volume;
    if (ready) {
        auto s = saved_pause ? paused_track : lyra::audio::status();
        out->source = source;
        out->file_size = file_size;
        out->state = s.last_error != ESP_OK ? MUSIC_PLAYER_STATE_ERROR
                     : s.eof                ? MUSIC_PLAYER_STATE_FINISHED
                     : s.paused             ? MUSIC_PLAYER_STATE_PAUSED
                     : s.playing
                         ? (s.sample_rate ? MUSIC_PLAYER_STATE_PLAYING : MUSIC_PLAYER_STATE_LOADING)
                         : MUSIC_PLAYER_STATE_IDLE;
        const char *p = std::strncmp(s.path, "/smb/", 5) == 0 ? s.path + 5 :
                        std::strncmp(s.path, "/sdcard/", 8) == 0 ? s.path + 8 : s.path;
        std::snprintf(out->path, sizeof(out->path), "%.*s", (int)sizeof(out->path) - 1, p);
        const char *base = std::strrchr(p, '/');
        std::snprintf(out->name, sizeof(out->name), "%.*s", (int)sizeof(out->name) - 1,
                      base ? base + 1 : p);
        out->elapsed_ms = s.position_ms;
        out->duration_ms = s.duration_ms;
        out->sample_rate = s.sample_rate;
        out->channels = s.channels;
        out->bits_per_sample = s.bits_per_sample;
        out->position = s.duration_ms
                            ? std::min<uint64_t>(file_size, static_cast<uint64_t>(file_size) *
                                                                s.position_ms / s.duration_ms)
                            : 0;
        out->bitrate = s.duration_ms ? static_cast<uint64_t>(file_size) * 8000 / s.duration_ms : 0;
        if (s.last_error != ESP_OK)
            std::snprintf(out->message, sizeof(out->message), "Lyra: %s",
                          esp_err_to_name(s.last_error));
    }
    unlock();
}

extern "C" esp_err_t music_player_acquire_burn_priority() {
    lock();
    if (!burn_owners && ready) {
        auto s = lyra::audio::status();
        if (!saved_pause && s.playing && s.last_error == ESP_OK && s.path[0]) {
            paused_track = s;
            paused_track.paused = true;
            saved_pause = true;
        }
        // Holding this adapter lock excludes new play/seek requests until
        // the Lyra worker acknowledges input, decoder and I2S cleanup.
        auto err = lyra::audio::stop_and_wait();
        if (err != ESP_OK) { unlock(); return err; }
    }
    ++burn_owners;
    unlock();
    return ESP_OK;
}

extern "C" void music_player_release_burn_priority() {
    lock();
    if (burn_owners) --burn_owners;
    unlock();
}
