#include "audio_player.h"

#include <stdio.h>

static char s_last_error[64] = "";

static esp_err_t audio_disabled(void) {
    snprintf(s_last_error, sizeof(s_last_error), "audio disabled");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_player_play_file(const char *path, const char *mimetype, bool force_opus) {
    (void)path;
    (void)mimetype;
    (void)force_opus;
    return audio_disabled();
}

esp_err_t audio_player_play_buffer(
    const uint8_t *data, size_t data_len, const char *mimetype, const char *label, bool force_opus
) {
    (void)data;
    (void)data_len;
    (void)mimetype;
    (void)label;
    (void)force_opus;
    return audio_disabled();
}

const char *audio_player_last_error(void) {
    return s_last_error;
}

void audio_player_set_volume_percent(int percent) {
    (void)percent;
}

int audio_player_get_volume_percent(void) {
    return 0;
}
