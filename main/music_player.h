#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_PLAYER_PATH_MAX 240
#define MUSIC_PLAYER_NAME_MAX 128
#define MUSIC_PLAYER_MESSAGE_MAX 96

typedef enum {
    MUSIC_PLAYER_STATE_IDLE = 0,
    MUSIC_PLAYER_STATE_LOADING,
    MUSIC_PLAYER_STATE_PLAYING,
    MUSIC_PLAYER_STATE_PAUSED,
    MUSIC_PLAYER_STATE_FINISHED,
    MUSIC_PLAYER_STATE_ERROR,
} music_player_state_t;

typedef struct {
    music_player_state_t state;
    char path[MUSIC_PLAYER_PATH_MAX];
    char name[MUSIC_PLAYER_NAME_MAX];
    char message[MUSIC_PLAYER_MESSAGE_MAX];
    uint32_t file_size;
    uint32_t position;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume_percent;
} music_player_snapshot_t;

esp_err_t music_player_init(void);
esp_err_t music_player_play(const char *rel_path, uint32_t file_size);
esp_err_t music_player_stop(void);
esp_err_t music_player_toggle_pause(void);
esp_err_t music_player_set_volume(uint8_t volume_percent);
void music_player_get_snapshot(music_player_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_H */
