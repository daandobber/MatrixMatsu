#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_player_play_file(const char *path, const char *mimetype, bool force_opus);
esp_err_t audio_player_play_buffer(
    const uint8_t *data, size_t data_len, const char *mimetype, const char *label, bool force_opus
);
const char *audio_player_last_error(void);
void audio_player_set_volume_percent(int percent);
int audio_player_get_volume_percent(void);

#ifdef __cplusplus
}
#endif
