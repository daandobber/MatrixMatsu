#include "audio_output.h"

#include <cstdarg>
#include <cstdio>

extern "C" {
#include "bsp/audio.h"
esp_err_t bsp_audio_initialize(void);
}
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_output";
static bool s_audio_initialized = false;
static int s_volume_percent = 55;
static constexpr size_t kI2sWriteChunkBytes = 2048;
static constexpr int kI2sTimeoutRetries = 40;
static constexpr int kSilenceDrainMs = 120;

int audio_output_get_volume_percent(void) {
    return s_volume_percent;
}

void audio_output_set_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume_percent = percent;
    if (s_audio_initialized) {
        esp_err_t res = bsp_audio_set_volume((float)s_volume_percent);
        if (res != ESP_OK) ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(res));
    }
}

void BspI2sWriter::set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(this->err_, sizeof(this->err_), fmt, args);
    va_end(args);
}

esp_err_t BspI2sWriter::begin(uint32_t sample_rate) {
    esp_err_t res = bsp_audio_get_i2s_handle(&this->i2s_);
    if ((res != ESP_OK || this->i2s_ == nullptr) && !s_audio_initialized) {
        res = bsp_audio_initialize();
        if (res != ESP_OK) {
            this->set_error("audio init %s", esp_err_to_name(res));
            return res;
        }
        s_audio_initialized = true;
        res = bsp_audio_get_i2s_handle(&this->i2s_);
    } else if (res == ESP_OK && this->i2s_ != nullptr) {
        s_audio_initialized = true;
    }

    if (res != ESP_OK || this->i2s_ == nullptr) {
        this->set_error("i2s handle %s", esp_err_to_name(res));
        return res == ESP_OK ? ESP_FAIL : res;
    }

    esp_err_t disable_res = i2s_channel_disable(this->i2s_);
    if (disable_res != ESP_OK && disable_res != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2S disable before rate change failed: %s", esp_err_to_name(disable_res));
    }

    res = bsp_audio_set_rate(sample_rate);
    if (res != ESP_OK) {
        this->set_error("rate %s", esp_err_to_name(res));
        return res;
    }

    res = i2s_channel_enable(this->i2s_);
    if (res != ESP_OK && res != ESP_ERR_INVALID_STATE) {
        this->set_error("i2s enable %s", esp_err_to_name(res));
        return res;
    }

    esp_err_t vol_res = bsp_audio_set_volume((float)s_volume_percent);
    if (vol_res != ESP_OK) ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(vol_res));
    esp_err_t amp_res = bsp_audio_set_amplifier(true);
    if (amp_res != ESP_OK) ESP_LOGW(TAG, "Amplifier enable failed: %s", esp_err_to_name(amp_res));

    this->ready_ = true;
    return ESP_OK;
}

esp_err_t BspI2sWriter::write(const int16_t *pcm, size_t samples_per_channel, uint8_t channels) {
    if (!this->ready_ || this->i2s_ == nullptr || pcm == nullptr || samples_per_channel == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channels != 2) {
        this->set_error("pcm ch %u", (unsigned int)channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint8_t *data = reinterpret_cast<const uint8_t *>(pcm);
    size_t length = samples_per_channel * channels * sizeof(int16_t);
    size_t total_written = 0;
    while (total_written < length) {
        size_t written = 0;
        size_t to_write = length - total_written;
        if (to_write > kI2sWriteChunkBytes) to_write = kI2sWriteChunkBytes;

        esp_err_t res = ESP_OK;
        for (int attempt = 0; attempt <= kI2sTimeoutRetries; attempt++) {
            written = 0;
            res = i2s_channel_write(this->i2s_, data + total_written, to_write, &written, pdMS_TO_TICKS(250));
            if (res != ESP_ERR_TIMEOUT && !(res == ESP_OK && written == 0)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (res != ESP_OK || written == 0) {
            if (res == ESP_OK) {
                this->set_error("i2s no progress");
            } else {
                this->set_error("i2s write %s", esp_err_to_name(res));
            }
            return res == ESP_OK ? ESP_FAIL : res;
        }
        total_written += written;
        this->bytes_written_ += written;
    }
    return ESP_OK;
}

void BspI2sWriter::finish() {
    if (!this->ready_ || this->i2s_ == nullptr) {
        bsp_audio_set_amplifier(false);
        return;
    }

    int16_t silence[480 * 2] = {0};
    const int blocks = kSilenceDrainMs / 10;
    for (int i = 0; i < blocks; i++) {
        if (this->write(silence, 480, 2) != ESP_OK) break;
    }
    vTaskDelay(pdMS_TO_TICKS(40));

    esp_err_t amp_res = bsp_audio_set_amplifier(false);
    if (amp_res != ESP_OK) ESP_LOGW(TAG, "Amplifier disable failed: %s", esp_err_to_name(amp_res));

    esp_err_t disable_res = i2s_channel_disable(this->i2s_);
    if (disable_res != ESP_OK && disable_res != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2S disable after playback failed: %s", esp_err_to_name(disable_res));
    }
    this->ready_ = false;
}
