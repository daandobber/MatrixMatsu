#include "openh264_probe.h"

#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "openh264_dec_wrapper.h"

extern "C" const uint8_t openh264_probe_clip_264_start[] asm("_binary_openh264_probe_clip_264_start");
extern "C" const uint8_t openh264_probe_clip_264_end[] asm("_binary_openh264_probe_clip_264_end");

static const char *TAG = "openh264_probe";

namespace {

/* Splits `data` into consecutive Annex-B NAL units (each returned slice
 * includes its own leading start code, up to but excluding the next NAL's
 * start code), matching how OpenH264's own reference decoder feeds
 * DecodeFrameNoDelay one NAL at a time (see codec/console/dec/src/h264dec.cpp
 * upstream) -- unlike esp_h264/tinyh264, which accepts a whole multi-NAL
 * buffer per call and reports back how much it consumed. */
bool next_nal(const uint8_t *data, size_t len, size_t *pos, size_t *nal_start, size_t *nal_len) {
    size_t i = *pos;
    while (i + 3 < len && !(data[i] == 0 && data[i + 1] == 0 && (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1)))) {
        i++;
    }
    if (i + 3 >= len) return false;
    *nal_start   = i;
    size_t scan  = i + (data[i + 2] == 1 ? 3 : 4);
    size_t j     = scan;
    while (j + 3 < len && !(data[j] == 0 && data[j + 1] == 0 && (data[j + 2] == 1 || (data[j + 2] == 0 && data[j + 3] == 1)))) {
        j++;
    }
    if (j + 3 >= len) j = len;
    *nal_len = j - *nal_start;
    *pos     = j;
    return true;
}

}  // namespace

void openh264_probe_run(void) {
    const uint8_t *data = openh264_probe_clip_264_start;
    size_t len          = (size_t)(openh264_probe_clip_264_end - openh264_probe_clip_264_start);
    ESP_LOGW(TAG, "starting: clip=%u bytes (Main profile, CABAC, B-frames -- unsupported by tinyh264)", (unsigned)len);

    openh264_dec_t *dec = openh264_dec_create();
    if (dec == nullptr) {
        ESP_LOGE(TAG, "openh264_dec_create failed");
        return;
    }

    size_t pos = 0, nal_start = 0, nal_len = 0;
    int frame_count       = 0;
    int64_t total_decode_us = 0;
    int64_t worst_decode_us = 0;

    while (next_nal(data, len, &pos, &nal_start, &nal_len)) {
        uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
        int width = 0, height = 0, stride_y = 0, stride_uv = 0;

        int64_t t0 = esp_timer_get_time();
        int rv     = openh264_dec_decode(
            dec, data + nal_start, (int)nal_len, &y, &u, &v, &width, &height, &stride_y, &stride_uv
        );
        int64_t elapsed_us = esp_timer_get_time() - t0;

        if (rv < 0) {
            ESP_LOGW(TAG, "decode error at NAL offset %u (len %u)", (unsigned)nal_start, (unsigned)nal_len);
            continue;
        }
        if (rv == 1) {
            frame_count++;
            total_decode_us += elapsed_us;
            if (elapsed_us > worst_decode_us) worst_decode_us = elapsed_us;
            ESP_LOGI(
                TAG, "frame %d: %dx%d decoded in %lld us (%.1f fps equiv)", frame_count, width, height,
                (long long)elapsed_us, elapsed_us > 0 ? 1000000.0 / (double)elapsed_us : 0.0
            );
        }
    }

    openh264_dec_destroy(dec);

    if (frame_count == 0) {
        ESP_LOGE(TAG, "finished: 0 frames decoded -- something is wrong with the port, not just slow");
        return;
    }
    double avg_us = (double)total_decode_us / frame_count;
    ESP_LOGW(
        TAG, "finished: %d frames, avg %.0f us/frame (%.1f fps equiv), worst %lld us/frame (%.1f fps equiv)",
        frame_count, avg_us, avg_us > 0 ? 1000000.0 / avg_us : 0.0, (long long)worst_decode_us,
        worst_decode_us > 0 ? 1000000.0 / (double)worst_decode_us : 0.0
    );
}
