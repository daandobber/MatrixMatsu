#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2s_common.h"
#include "esp_err.h"

/* Shared I2S output plumbing used by both the audio message player
 * (audio_player_decode.cpp) and the video player's audio track (video_player.cpp),
 * so both share one volume setting and one "is the audio subsystem initialized"
 * state instead of drifting apart. */

int audio_output_get_volume_percent(void);
void audio_output_set_volume_percent(int percent);

class BspI2sWriter {
public:
    esp_err_t begin(uint32_t sample_rate);
    esp_err_t write(const int16_t *pcm, size_t samples_per_channel, uint8_t channels);
    size_t bytes_written() const {
        return this->bytes_written_;
    }
    const char *last_error() const {
        return this->err_;
    }
    void finish();

private:
    void set_error(const char *fmt, ...);

    i2s_chan_handle_t i2s_ = nullptr;
    bool ready_ = false;
    size_t bytes_written_ = 0;
    char err_[64] = "";
};
