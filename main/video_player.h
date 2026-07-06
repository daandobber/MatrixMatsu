#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Demuxes an in-memory MP4 buffer (H.264 + AAC), decodes it, and renders video
 * frames directly to the display while playing the audio track through the
 * shared I2S output. Blocking: runs until the stream ends, a fatal decode
 * error occurs, or video_player_request_stop() is called from another task. */
esp_err_t video_player_play_buffer(const uint8_t *data, size_t data_len, const char *label);
const char *video_player_last_error(void);
void video_player_request_stop(void);

#ifdef __cplusplus
}
#endif
