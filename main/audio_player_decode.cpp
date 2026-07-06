#include "audio_player.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "bsp/audio.h"
esp_err_t bsp_audio_initialize(void);
}
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "micro_opus/ogg_opus_decoder.h"
#include "micro_vorbis/ogg_vorbis_decoder.h"

static const char *TAG = "audio_decode";
static char s_last_error[96] = "";
static bool s_audio_initialized = false;
static int s_volume_percent = 55;
static audio_player_progress_cb_t s_progress_cb = nullptr;
static void *s_progress_ctx = nullptr;
static constexpr int kPcmGainNumerator = 1;
static constexpr int kPcmGainDenominator = 2;
static constexpr size_t kI2sWriteChunkBytes = 2048;
static constexpr int kI2sTimeoutRetries = 40;
static constexpr int kSilenceDrainMs = 120;

static void set_last_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

const char *audio_player_last_error(void) {
    return s_last_error;
}

void audio_player_set_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume_percent = percent;
    if (s_audio_initialized) {
        esp_err_t res = bsp_audio_set_volume((float)s_volume_percent);
        if (res != ESP_OK) ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(res));
    }
}

int audio_player_get_volume_percent(void) {
    return s_volume_percent;
}

void audio_player_set_progress_callback(audio_player_progress_cb_t cb, void *ctx) {
    s_progress_cb = cb;
    s_progress_ctx = ctx;
}

/* Reports elapsed playback time to s_progress_cb at most once per second of
 * decoded audio, so the UI can show "0:07 / 0:23" without redrawing on every frame. */
static void report_progress(uint32_t *last_reported_sec, size_t total_samples, uint32_t sample_rate) {
    if (s_progress_cb == nullptr || sample_rate == 0) return;
    uint32_t elapsed_ms = (uint32_t)(((uint64_t)total_samples * 1000) / sample_rate);
    uint32_t sec = elapsed_ms / 1000;
    if (sec == *last_reported_sec) return;
    *last_reported_sec = sec;
    s_progress_cb(elapsed_ms, s_progress_ctx);
}

static const char *opus_result_name(micro_opus::OggOpusResult result) {
    switch (result) {
        case micro_opus::OGG_OPUS_OK: return "ok";
        case micro_opus::OGG_OPUS_INPUT_INVALID: return "input";
        case micro_opus::OGG_OPUS_NOT_INITIALIZED: return "not init";
        case micro_opus::OGG_OPUS_ALLOCATION_FAILED: return "no mem";
        case micro_opus::OGG_OPUS_OUTPUT_BUFFER_TOO_SMALL: return "buf small";
        case micro_opus::OGG_OPUS_DECODE_ERROR: return "decode";
        default: return "opus";
    }
}

static const char *vorbis_result_name(micro_vorbis::OggVorbisResult result) {
    switch (result) {
        case micro_vorbis::OGG_VORBIS_DECODER_SUCCESS: return "ok";
        case micro_vorbis::OGG_VORBIS_DECODER_STREAM_INFO_READY: return "info";
        case micro_vorbis::OGG_VORBIS_DECODER_NEED_MORE_DATA: return "need data";
        case micro_vorbis::OGG_VORBIS_DECODER_END_OF_STREAM: return "eos";
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL: return "buf small";
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_INPUT_INVALID: return "input";
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_ALLOCATION_FAILED: return "no mem";
        case micro_vorbis::OGG_VORBIS_DECODER_ERROR_DECODE_FAILED: return "decode";
        default: return "vorbis";
    }
}

class BspI2sWriter {
public:
    esp_err_t begin(uint32_t sample_rate) {
        esp_err_t res = bsp_audio_get_i2s_handle(&this->i2s_);
        if ((res != ESP_OK || this->i2s_ == nullptr) && !s_audio_initialized) {
            res = bsp_audio_initialize();
            if (res != ESP_OK) {
                set_last_error("audio init %s", esp_err_to_name(res));
                return res;
            }
            s_audio_initialized = true;
            res = bsp_audio_get_i2s_handle(&this->i2s_);
        } else if (res == ESP_OK && this->i2s_ != nullptr) {
            s_audio_initialized = true;
        }

        if (res != ESP_OK || this->i2s_ == nullptr) {
            set_last_error("i2s handle %s", esp_err_to_name(res));
            return res == ESP_OK ? ESP_FAIL : res;
        }

        esp_err_t disable_res = i2s_channel_disable(this->i2s_);
        if (disable_res != ESP_OK && disable_res != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "I2S disable before rate change failed: %s", esp_err_to_name(disable_res));
        }

        res = bsp_audio_set_rate(sample_rate);
        if (res != ESP_OK) {
            set_last_error("rate %s", esp_err_to_name(res));
            return res;
        }

        res = i2s_channel_enable(this->i2s_);
        if (res != ESP_OK && res != ESP_ERR_INVALID_STATE) {
            set_last_error("i2s enable %s", esp_err_to_name(res));
            return res;
        }

        esp_err_t vol_res = bsp_audio_set_volume((float)s_volume_percent);
        if (vol_res != ESP_OK) ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(vol_res));
        esp_err_t amp_res = bsp_audio_set_amplifier(true);
        if (amp_res != ESP_OK) ESP_LOGW(TAG, "Amplifier enable failed: %s", esp_err_to_name(amp_res));

        this->ready_ = true;
        return ESP_OK;
    }

    esp_err_t write(const int16_t *pcm, size_t samples_per_channel, uint8_t channels) {
        if (!this->ready_ || this->i2s_ == nullptr || pcm == nullptr || samples_per_channel == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (channels != 2) {
            set_last_error("pcm ch %u", (unsigned int)channels);
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
                    set_last_error("i2s no progress");
                } else {
                    set_last_error("i2s write %s", esp_err_to_name(res));
                }
                return res == ESP_OK ? ESP_FAIL : res;
            }
            total_written += written;
            this->bytes_written_ += written;
        }
        return ESP_OK;
    }

    size_t bytes_written() const {
        return this->bytes_written_;
    }

    void finish() {
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

private:
    i2s_chan_handle_t i2s_ = nullptr;
    bool ready_ = false;
    size_t bytes_written_ = 0;
};

static void apply_pcm_gain(int16_t *pcm, size_t samples_per_channel, uint8_t channels) {
    if (pcm == nullptr || samples_per_channel == 0 || channels == 0) return;
    size_t count = samples_per_channel * channels;
    for (size_t i = 0; i < count; i++) {
        int32_t scaled = ((int32_t)pcm[i] * kPcmGainNumerator) / kPcmGainDenominator;
        pcm[i] = (int16_t)scaled;
    }
}

/* Voice messages are usually Ogg Opus, but plain "audio/ogg" uploads are sometimes
 * Ogg Vorbis instead; sniff the codec up front so each buffer is routed to the
 * decoder that actually understands it. */
static bool ogg_bytes_contain(const uint8_t *data, size_t len, const char *needle, size_t needle_len) {
    if (len < needle_len) return false;
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static esp_err_t play_ogg_opus_buffer(const uint8_t *data, size_t data_len) {
    if (data == nullptr || data_len == 0) return ESP_ERR_INVALID_ARG;

    constexpr uint32_t sample_rate = 48000;
    constexpr uint8_t channels = 2;
    constexpr size_t max_samples_per_channel = 5760;
    const size_t pcm_count = max_samples_per_channel * channels;
    auto *pcm = static_cast<int16_t *>(heap_caps_malloc(
        pcm_count * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
    ));
    if (pcm == nullptr) {
        pcm = static_cast<int16_t *>(heap_caps_malloc(pcm_count * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (pcm == nullptr) {
        pcm = static_cast<int16_t *>(heap_caps_malloc(pcm_count * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (pcm == nullptr) {
        set_last_error("pcm no mem");
        return ESP_ERR_NO_MEM;
    }

    micro_opus::OggOpusDecoder decoder(false, sample_rate, channels);
    BspI2sWriter writer;
    const uint8_t *input = data;
    size_t remaining = data_len;
    size_t total_samples = 0;
    uint32_t frames = 0;
    bool writer_started = false;
    uint32_t active_rate = sample_rate;
    uint32_t last_reported_sec = UINT32_MAX;
    esp_err_t result = ESP_OK;

    while (remaining > 0) {
        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;
        micro_opus::OggOpusResult dec_res = decoder.decode(
            input, remaining, reinterpret_cast<uint8_t *>(pcm), pcm_count * sizeof(int16_t),
            bytes_consumed, samples_decoded
        );
        if (dec_res != micro_opus::OGG_OPUS_OK) {
            ESP_LOGW(TAG, "Opus decode failed: %d %s", (int)dec_res, opus_result_name(dec_res));
            set_last_error("opus %s", opus_result_name(dec_res));
            result = ESP_FAIL;
            break;
        }
        if (samples_decoded > 0) {
            if (!writer_started) {
                active_rate = decoder.get_sample_rate() != 0 ? decoder.get_sample_rate() : sample_rate;
                result = writer.begin(active_rate);
                if (result != ESP_OK) break;
                writer_started = true;
            }
            uint8_t out_channels = decoder.get_channels() != 0 ? decoder.get_channels() : channels;
            apply_pcm_gain(pcm, samples_decoded, out_channels);
            result = writer.write(pcm, samples_decoded, out_channels);
            if (result != ESP_OK) break;
            total_samples += samples_decoded;
            report_progress(&last_reported_sec, total_samples, active_rate);
            frames++;
        }
        if (bytes_consumed == 0) {
            if (samples_decoded == 0) {
                set_last_error("opus no progress");
                result = ESP_FAIL;
                break;
            }
        } else {
            input += bytes_consumed;
            remaining -= bytes_consumed;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    writer.finish();
    heap_caps_free(pcm);

    if (result == ESP_OK && total_samples == 0) {
        set_last_error("no pcm");
        return ESP_FAIL;
    }
    if (result == ESP_OK) {
        set_last_error("played %u", (unsigned int)frames);
        return writer.bytes_written() > 0 ? ESP_OK : ESP_FAIL;
    }
    return result;
}

static esp_err_t resize_vorbis_pcm_buffer(uint8_t **pcm, size_t *pcm_bytes, size_t needed) {
    if (needed <= *pcm_bytes) return ESP_OK;
    uint8_t *new_pcm =
        static_cast<uint8_t *>(heap_caps_malloc(needed, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (new_pcm == nullptr) new_pcm = static_cast<uint8_t *>(heap_caps_malloc(needed, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (new_pcm == nullptr) new_pcm = static_cast<uint8_t *>(heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_pcm == nullptr) {
        set_last_error("pcm no mem");
        return ESP_ERR_NO_MEM;
    }
    heap_caps_free(*pcm);
    *pcm = new_pcm;
    *pcm_bytes = needed;
    return ESP_OK;
}

static esp_err_t play_ogg_vorbis_buffer(const uint8_t *data, size_t data_len) {
    if (data == nullptr || data_len == 0) return ESP_ERR_INVALID_ARG;

    constexpr uint8_t channels = 2;
    micro_vorbis::OggVorbisDecoder decoder(channels);
    BspI2sWriter writer;
    const uint8_t *input = data;
    size_t remaining = data_len;
    size_t total_samples = 0;
    uint32_t frames = 0;
    bool writer_started = false;
    uint32_t active_rate = 48000;
    uint32_t last_reported_sec = UINT32_MAX;
    esp_err_t result = ESP_OK;
    uint8_t *pcm = nullptr;
    size_t pcm_bytes = 0;

    while (remaining > 0) {
        size_t bytes_consumed = 0;
        size_t bytes_written = 0;
        micro_vorbis::OggVorbisResult dec_res =
            decoder.decode(input, remaining, pcm, pcm_bytes, bytes_consumed, bytes_written);

        if (dec_res == micro_vorbis::OGG_VORBIS_DECODER_ERROR_OUTPUT_BUFFER_TOO_SMALL) {
            result = resize_vorbis_pcm_buffer(&pcm, &pcm_bytes, decoder.get_required_output_bytes());
            if (result != ESP_OK) break;
            continue;
        }
        if (dec_res < 0) {
            ESP_LOGW(TAG, "Vorbis decode failed: %d %s", (int)dec_res, vorbis_result_name(dec_res));
            set_last_error("vorbis %s", vorbis_result_name(dec_res));
            result = ESP_FAIL;
            break;
        }
        if (dec_res == micro_vorbis::OGG_VORBIS_DECODER_STREAM_INFO_READY) {
            result = resize_vorbis_pcm_buffer(&pcm, &pcm_bytes, decoder.get_pcm_format().max_output_bytes());
            if (result != ESP_OK) break;
        }
        if (dec_res == micro_vorbis::OGG_VORBIS_DECODER_END_OF_STREAM) break;

        if (bytes_written > 0) {
            uint32_t out_channels = decoder.get_pcm_format().num_channels();
            if (out_channels == 0) out_channels = channels;
            size_t samples_per_channel = bytes_written / (out_channels * sizeof(int16_t));
            if (!writer_started) {
                uint32_t rate = decoder.get_pcm_format().sample_rate();
                active_rate = rate != 0 ? rate : 48000;
                result = writer.begin(active_rate);
                if (result != ESP_OK) break;
                writer_started = true;
            }
            int16_t *pcm16 = reinterpret_cast<int16_t *>(pcm);
            apply_pcm_gain(pcm16, samples_per_channel, (uint8_t)out_channels);
            result = writer.write(pcm16, samples_per_channel, (uint8_t)out_channels);
            if (result != ESP_OK) break;
            total_samples += samples_per_channel;
            report_progress(&last_reported_sec, total_samples, active_rate);
            frames++;
        }

        if (bytes_consumed == 0 && bytes_written == 0) {
            set_last_error("vorbis no progress");
            result = ESP_FAIL;
            break;
        }
        input += bytes_consumed;
        remaining -= bytes_consumed;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    writer.finish();
    heap_caps_free(pcm);

    if (result == ESP_OK && total_samples == 0) {
        set_last_error("no pcm");
        return ESP_FAIL;
    }
    if (result == ESP_OK) {
        set_last_error("played %u", (unsigned int)frames);
        return writer.bytes_written() > 0 ? ESP_OK : ESP_FAIL;
    }
    return result;
}

esp_err_t audio_player_play_buffer(
    const uint8_t *data, size_t data_len, const char *mimetype, const char *label, bool force_opus
) {
    (void)mimetype;
    (void)label;
    (void)force_opus;
    set_last_error("");
    if (data == nullptr || data_len == 0) return ESP_ERR_INVALID_ARG;

    constexpr size_t kCodecSniffWindow = 128;
    size_t sniff_len = data_len < kCodecSniffWindow ? data_len : kCodecSniffWindow;
    if (ogg_bytes_contain(data, sniff_len, "OpusHead", 8)) {
        return play_ogg_opus_buffer(data, data_len);
    }
    if (ogg_bytes_contain(data, sniff_len, "vorbis", 6)) {
        return play_ogg_vorbis_buffer(data, data_len);
    }
    set_last_error("ogg codec unsupported");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_player_play_file(const char *path, const char *mimetype, bool force_opus) {
    (void)path;
    (void)mimetype;
    (void)force_opus;
    set_last_error("file decode disabled");
    return ESP_ERR_NOT_SUPPORTED;
}
