#include "video_player.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "audio_output.h"
extern "C" {
#include "bsp/display.h"
}
#include "esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openh264_dec_wrapper.h"

static const char *TAG = "video_player";
static char s_last_error[96] = "";
static volatile bool s_stop_requested = false;

static constexpr uint16_t kScreenW = 480;
static constexpr uint16_t kScreenH = 800;
static constexpr uint16_t kMaxVideoH = 600; /* leave room for a status line */

static void set_last_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

const char *video_player_last_error(void) {
    return s_last_error;
}

void video_player_request_stop(void) {
    s_stop_requested = true;
}

/* ------------------------------------------------------------------------ */
/* In-memory buffer as an esp_extractor input source                        */
/* ------------------------------------------------------------------------ */

namespace {

struct BufferCtx {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

int buffer_read_cb(void *buffer, uint32_t size, void *ctx_ptr) {
    BufferCtx *ctx = static_cast<BufferCtx *>(ctx_ptr);
    if (ctx->pos >= ctx->len) return 0;
    size_t remaining = ctx->len - ctx->pos;
    size_t to_copy = size < remaining ? size : remaining;
    memcpy(buffer, ctx->data + ctx->pos, to_copy);
    ctx->pos += to_copy;
    return (int)to_copy;
}

int buffer_seek_cb(uint32_t position, void *ctx_ptr) {
    BufferCtx *ctx = static_cast<BufferCtx *>(ctx_ptr);
    if (position > ctx->len) return -1;
    ctx->pos = position;
    return 0;
}

uint32_t buffer_size_cb(void *ctx_ptr) {
    return (uint32_t)static_cast<BufferCtx *>(ctx_ptr)->len;
}

/* ------------------------------------------------------------------------ */
/* I420 -> RGB565 with nearest-neighbor scale-to-fit                        */
/* ------------------------------------------------------------------------ */

inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void compute_fit_size(uint16_t src_w, uint16_t src_h, uint16_t *dst_w, uint16_t *dst_h) {
    if (src_w == 0 || src_h == 0) {
        *dst_w = 0;
        *dst_h = 0;
        return;
    }
    float scale_w = (float)kScreenW / (float)src_w;
    float scale_h = (float)kMaxVideoH / (float)src_h;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale > 1.0f) scale = 1.0f;
    *dst_w = (uint16_t)(src_w * scale);
    *dst_h = (uint16_t)(src_h * scale);
    if (*dst_w == 0) *dst_w = 1;
    if (*dst_h == 0) *dst_h = 1;
}

/* True if `b` could plausibly be an H.264 NAL header byte (forbidden_zero_bit
 * clear, nal_unit_type in the defined 1-23 range). Used to disambiguate a real
 * Annex-B start code from a same-looking byte sequence inside an AVCC 4-byte
 * length prefix (e.g. a length of 256-511 encodes as 00 00 01 xx, which is
 * indistinguishable from a 3-byte start code by prefix bytes alone). */
inline bool looks_like_nal_header(uint8_t b) {
    return (b & 0x80) == 0 && (b & 0x1f) != 0 && (b & 0x1f) <= 23;
}

/* True if `data` already starts with an Annex-B start code (3- or 4-byte)
 * immediately followed by a plausible NAL header byte. */
bool looks_like_annexb(const uint8_t *data, uint32_t len) {
    if (len >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1 && looks_like_nal_header(data[4]))
        return true;
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 && looks_like_nal_header(data[3])) return true;
    return false;
}

/* Re-frames one MP4 "sample" (an access unit made of one or more NALs, each
 * prefixed by a 4-byte big-endian length, per the avcC lengthSizeMinusOne
 * convention almost universally used by MP4 muxers) into Annex-B so the SW
 * H.264 decoder can consume it. Returns a heap_caps_malloc'd buffer, or
 * nullptr if the input doesn't parse as length-prefixed NALs. */
uint8_t *remux_avcc_frame_to_annexb(const uint8_t *frame, uint32_t frame_len, size_t *out_len) {
    *out_len = 0;
    size_t total = 0;
    size_t pos = 0;
    while (pos + 4 <= frame_len) {
        uint32_t nal_len =
            ((uint32_t)frame[pos] << 24) | ((uint32_t)frame[pos + 1] << 16) | ((uint32_t)frame[pos + 2] << 8) |
            frame[pos + 3];
        pos += 4;
        if (nal_len == 0 || pos + nal_len > frame_len) return nullptr;
        total += 4 + nal_len;
        pos += nal_len;
    }
    if (total == 0 || pos != frame_len) return nullptr;

    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) buf = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_8BIT));
    if (buf == nullptr) return nullptr;

    uint8_t *w = buf;
    pos = 0;
    while (pos + 4 <= frame_len) {
        uint32_t nal_len =
            ((uint32_t)frame[pos] << 24) | ((uint32_t)frame[pos + 1] << 16) | ((uint32_t)frame[pos + 2] << 8) |
            frame[pos + 3];
        pos += 4;
        w[0] = 0;
        w[1] = 0;
        w[2] = 0;
        w[3] = 1;
        memcpy(w + 4, frame + pos, nal_len);
        w += 4 + nal_len;
        pos += nal_len;
    }
    *out_len = total;
    return buf;
}

/* Splits `data` into consecutive Annex-B NAL units (each returned slice
 * includes its own leading start code, up to but excluding the next NAL's
 * start code). OpenH264's DecodeFrameNoDelay expects to be fed exactly one
 * NAL per call (confirmed against upstream's own reference decoder,
 * codec/console/dec/src/h264dec.cpp) -- unlike tinyh264/esp_h264, which
 * accepted one big multi-NAL buffer per call and reported back bytes
 * consumed. */
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

/* tinyh264 (esp_h264's SW decoder) only accepts Baseline profile
 * (profile_idc 66) -- confirmed on real hardware, where a High-profile
 * (profile_idc 100) SPS is rejected outright ("profile_idc is error"). But
 * it's dramatically faster than OpenH264 for content it CAN decode (measured
 * elsewhere at 25-31fps @ 640x480 with esp_h264's own dual-task mode vs.
 * OpenH264's ~6fps @ 384x384), since Baseline's CAVLC entropy coding and lack
 * of B-frames are intrinsically cheaper than Main/High's CABAC. So: sniff the
 * profile out of spec_info's SPS and prefer tinyh264 when it applies,
 * falling back to OpenH264 (Main/High-capable but slower) otherwise. */
bool spec_info_is_baseline(const uint8_t *spec_info, uint32_t len) {
    if (spec_info == nullptr) return false;
    size_t pos = 0, nal_start = 0, nal_len = 0;
    while (next_nal(spec_info, len, &pos, &nal_start, &nal_len)) {
        if (nal_len < 2) continue;
        size_t hdr_off = (spec_info[nal_start + 2] == 1) ? 3 : 4;
        if (nal_len <= hdr_off + 1) continue;
        uint8_t nal_type = spec_info[nal_start + hdr_off] & 0x1f;
        if (nal_type == 7) {  // SPS
            uint8_t profile_idc = spec_info[nal_start + hdr_off + 1];
            return profile_idc == 66;
        }
    }
    return false;
}

/* Output is 24-bit RGB888 (3 bytes/pixel, B-G-R byte order -- confirmed
 * empirically on-device; "888RGB" naming in the BSP doesn't guarantee R is
 * byte 0), matching this app's actual configured display format (see main.c's
 * bsp_configuration.display.requested_color_format =
 * BSP_DISPLAY_COLOR_FORMAT_24_888RGB). An earlier version of this function
 * packed RGB565 (2 bytes/pixel) into the buffer handed to bsp_display_blit,
 * which forwards straight through to the panel in its *actual* 24bpp format
 * -- feeding it a 2-bytes/pixel buffer produced a progressively shifting/
 * streaky image (each "pixel" the panel read consumed 3 bytes of a buffer
 * only meaningful every 2 bytes).
 *
 * Also bakes in a 90-degree clockwise rotation: this device's panel has a
 * 270-degree default mounting rotation (bsp_display_get_default_rotation()),
 * which the rest of the UI corrects for via pax's orientation handling --
 * but video frames are blitted directly, bypassing pax entirely, so that
 * correction has to be applied here instead. Because of the rotation, the
 * source column (sx) is now constant per *destination row* and the source
 * row (sy) varies per *destination column* -- the reverse of the unrotated
 * case -- so the Y/U/V row pointers can no longer be hoisted out of the
 * inner loop the way an unrotated nearest-neighbor scaler normally would. */
void render_i420_scaled(
    const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane, uint16_t stride_y, uint16_t stride_uv,
    uint16_t src_w, uint16_t src_h, uint8_t *rgb_out, uint16_t dst_w, uint16_t dst_h
) {
    for (uint16_t dy = 0; dy < dst_h; dy++) {
        uint16_t sx = (uint32_t)dy * src_w / dst_h;
        uint8_t *out_row = rgb_out + (size_t)dy * dst_w * 3;
        for (uint16_t dx = 0; dx < dst_w; dx++) {
            uint16_t sy = (uint32_t)(dst_w - 1 - dx) * src_h / dst_w;
            const uint8_t *y_row = y_plane + (size_t)sy * stride_y;
            const uint8_t *u_row = u_plane + (size_t)(sy / 2) * stride_uv;
            const uint8_t *v_row = v_plane + (size_t)(sy / 2) * stride_uv;
            int y = y_row[sx];
            int u = u_row[sx / 2];
            int v = v_row[sx / 2];
            int c = y - 16;
            int d = u - 128;
            int e = v - 128;
            out_row[dx * 3 + 0] = clamp_u8((298 * c + 516 * d + 128) >> 8);         // B
            out_row[dx * 3 + 1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8); // G
            out_row[dx * 3 + 2] = clamp_u8((298 * c + 409 * e + 128) >> 8);         // R
        }
    }
}

}  // namespace

/* ------------------------------------------------------------------------ */
/* Playback                                                                  */
/* ------------------------------------------------------------------------ */

esp_err_t video_player_play_buffer(const uint8_t *data, size_t data_len, const char *label) {
    (void)label;
    set_last_error("");
    s_stop_requested = false;
    if (data == nullptr || data_len == 0) return ESP_ERR_INVALID_ARG;

    static bool extractors_registered = false;
    if (!extractors_registered) {
        esp_extractor_register_default();
        extractors_registered = true;
    }

    BufferCtx buf_ctx{data, data_len, 0};
    esp_extractor_config_t ext_cfg = {};
    ext_cfg.type = ESP_EXTRACTOR_TYPE_MP4;
    ext_cfg.extract_mask = ESP_EXTRACT_MASK_AV;
    ext_cfg.in_read_cb = buffer_read_cb;
    ext_cfg.in_seek_cb = buffer_seek_cb;
    ext_cfg.in_size_cb = buffer_size_cb;
    ext_cfg.in_ctx = &buf_ctx;
    ext_cfg.out_pool_size = 512 * 1024;
    ext_cfg.out_align = 4;

    esp_extractor_handle_t extractor = nullptr;
    if (esp_extractor_open(&ext_cfg, &extractor) != ESP_EXTRACTOR_ERR_OK || extractor == nullptr) {
        set_last_error("mp4 open failed");
        return ESP_FAIL;
    }
    if (esp_extractor_parse_stream(extractor) != ESP_EXTRACTOR_ERR_OK) {
        set_last_error("mp4 parse failed");
        esp_extractor_close(extractor);
        return ESP_FAIL;
    }

    esp_extractor_stream_info_t video_info = {};
    esp_extractor_stream_info_t audio_info = {};
    bool has_video = esp_extractor_get_stream_info(extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0, &video_info) ==
                      ESP_EXTRACTOR_ERR_OK;
    bool has_audio = esp_extractor_get_stream_info(extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0, &audio_info) ==
                      ESP_EXTRACTOR_ERR_OK;

    if (!has_video || video_info.video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_H264) {
        set_last_error("no h264 video track");
        esp_extractor_close(extractor);
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool use_tinyh264 = spec_info_is_baseline(video_info.spec_info, video_info.spec_info_len);
    openh264_dec_t *h264_dec_open = nullptr;
    esp_h264_dec_handle_t h264_dec_tiny = nullptr;
    if (use_tinyh264) {
        esp_h264_dec_cfg_sw_t dec_cfg = {.pic_type = ESP_H264_RAW_FMT_I420};
        if (esp_h264_dec_sw_new(&dec_cfg, &h264_dec_tiny) != ESP_H264_ERR_OK || h264_dec_tiny == nullptr) {
            set_last_error("h264 decoder init failed");
            esp_extractor_close(extractor);
            return ESP_FAIL;
        }
        esp_h264_dec_open(h264_dec_tiny);
        ESP_LOGI(TAG, "using tinyh264 (Baseline profile, fast path)");
    } else {
        h264_dec_open = openh264_dec_create();
        if (h264_dec_open == nullptr) {
            set_last_error("h264 decoder init failed");
            esp_extractor_close(extractor);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "using OpenH264 (Main/High profile, slow path)");
    }

    void *aac_dec = nullptr;
    if (has_audio && audio_info.audio_info.format == ESP_EXTRACTOR_AUDIO_FORMAT_AAC) {
        esp_aac_dec_cfg_t aac_cfg = {};
        aac_cfg.sample_rate = (int32_t)audio_info.audio_info.sample_rate;
        aac_cfg.channel = audio_info.audio_info.channel;
        aac_cfg.bits_per_sample = 16;
        aac_cfg.no_adts_header = false; /* esp_extractor's MP4 samples already carry an ADTS header */
        aac_cfg.aac_plus_enable = false;
        if (esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &aac_dec) != ESP_AUDIO_ERR_OK) {
            aac_dec = nullptr;
        }
    }
    esp_extractor_enable_stream(extractor, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0, aac_dec != nullptr);

    BspI2sWriter writer;
    bool writer_started = false;
    uint8_t *aac_pcm = nullptr;
    size_t aac_pcm_cap = 0;
    int16_t *stereo_pcm = nullptr; /* mono AAC upmix scratch buffer, since the I2S writer requires stereo */
    size_t stereo_pcm_cap = 0;

    uint16_t dec_w = 0;
    uint16_t dec_h = 0;
    uint16_t dst_w = 0;
    uint16_t dst_h = 0;
    uint16_t screen_x = 0;
    uint16_t screen_y = 0;
    uint8_t *rgb_buf = nullptr;

    int64_t start_us = -1;
    bool got_any_frame = false;
    bool primed = false;
    bool dumped_first_slice = false;
    esp_err_t result = ESP_OK;

    /* TEMPORARY: per-stage timing to find where playback time actually goes
     * before optimizing anything, see project memory "video playback" notes.
     * Remove once the perf question is answered. */
    int64_t total_read_us = 0, total_decode_us = 0, total_render_us = 0, total_blit_us = 0;
    int rendered_frame_count = 0;

    /* Shared by both decode paths (tinyh264 and OpenH264 expose the decoded
     * picture very differently -- see below -- but once reduced to a plane
     * pointer + stride triple, everything from here on is identical). */
    auto handle_decoded_picture = [&](
                                       const uint8_t *y, const uint8_t *u, const uint8_t *v, int stride_y_,
                                       int stride_uv_, int w, int h, uint32_t pts
                                   ) {
        got_any_frame = true;
        if (dst_w == 0) {
            dec_w = (uint16_t)w;
            dec_h = (uint16_t)h;
            /* Swapped on purpose: dst_w/dst_h represent the on-screen
             * footprint *after* the 90-degree rotation baked into
             * render_i420_scaled, where width and height trade places. */
            compute_fit_size(dec_h, dec_w, &dst_w, &dst_h);
            rgb_buf = static_cast<uint8_t *>(
                heap_caps_malloc((size_t)dst_w * dst_h * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
            );
            screen_x = (kScreenW > dst_w) ? (kScreenW - dst_w) / 2 : 0;
            screen_y = (kScreenH > dst_h) ? (kScreenH - dst_h) / 2 : 0;
            ESP_LOGW(
                TAG, "video geometry: coded=%dx%d stride_y=%d stride_uv=%d dst=%ux%u screen=(%u,%u)", w, h, stride_y_,
                stride_uv_, (unsigned)dst_w, (unsigned)dst_h, (unsigned)screen_x, (unsigned)screen_y
            );
        }

        if (start_us < 0) start_us = esp_timer_get_time();
        int64_t target_us = start_us + (int64_t)pts * 1000;
        int64_t now_us = esp_timer_get_time();
        if (target_us > now_us) {
            vTaskDelay(pdMS_TO_TICKS((target_us - now_us) / 1000));
        }

        if (rgb_buf != nullptr && dec_w > 0 && dec_h > 0) {
            int64_t t_render0 = esp_timer_get_time();
            render_i420_scaled(
                y, u, v, (uint16_t)stride_y_, (uint16_t)stride_uv_, dec_w, dec_h, rgb_buf, dst_w, dst_h
            );
            int64_t t_blit0 = esp_timer_get_time();
            total_render_us += t_blit0 - t_render0;
            /* bsp_display_blit forwards straight to esp_lcd_panel_draw_bitmap,
             * whose real contract is (x_start, y_start, x_end, y_end) -- exclusive
             * end coordinates, not a width/height pair, despite bsp/display.h's
             * parameter names suggesting otherwise. */
            bsp_display_blit(screen_x, screen_y, screen_x + dst_w, screen_y + dst_h, rgb_buf);
            total_blit_us += esp_timer_get_time() - t_blit0;
            rendered_frame_count++;
        }
    };

    while (!s_stop_requested) {
        esp_extractor_frame_info_t frame = {};
        int64_t t_read0             = esp_timer_get_time();
        esp_extractor_err_t rres = esp_extractor_read_frame(extractor, &frame);
        total_read_us += esp_timer_get_time() - t_read0;
        if (rres == ESP_EXTRACTOR_ERR_EOS) break;
        if (rres == ESP_EXTRACTOR_ERR_WAITING_OUTPUT) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (rres != ESP_EXTRACTOR_ERR_OK) {
            ESP_LOGW(TAG, "mp4 read_frame failed: %d", (int)rres);
            set_last_error("mp4 read %d", (int)rres);
            result = ESP_FAIL;
            break;
        }

        if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_VIDEO) {
            /* esp_extractor normalizes the avcC SPS/PPS to Annex-B, but per-frame
             * samples may still use AVCC 4-byte length prefixes (that's simply how
             * they're stored in the file); detect which one this is and only
             * convert when needed. The SPS/PPS are prepended to the first video
             * access unit rather than fed as a separate priming call, so the
             * decoder sees one continuous stream instead of two disjoint sessions.
             *
             * Try the strict AVCC length-prefix parse FIRST: it either tiles the
             * whole sample exactly or it doesn't, so it has near-zero false-positive
             * risk. A naive "does it start with 00 00 01" Annex-B sniff, tried first,
             * has a real false-positive: an AVCC length of 256-511 literally encodes
             * as bytes 00 00 01 xx, which looks like a 3-byte start code. Only trust
             * the buffer as already-Annex-B if the strict AVCC parse fails. */
            uint8_t *remuxed = nullptr;
            const uint8_t *frame_data = nullptr;
            uint32_t frame_data_len = 0;
            size_t remuxed_len = 0;
            remuxed = remux_avcc_frame_to_annexb(frame.frame_buffer, frame.frame_size, &remuxed_len);
            if (remuxed != nullptr) {
                frame_data = remuxed;
                frame_data_len = (uint32_t)remuxed_len;
            } else if (looks_like_annexb(frame.frame_buffer, frame.frame_size)) {
                frame_data = frame.frame_buffer;
                frame_data_len = frame.frame_size;
            }

            if (!dumped_first_slice) {
                dumped_first_slice = true;
                char hex[3 * 24 + 1] = "";
                uint32_t dump_n = frame.frame_size < 24 ? frame.frame_size : 24;
                for (uint32_t i = 0; i < dump_n; i++) snprintf(hex + i * 3, 4, "%02x ", frame.frame_buffer[i]);
                ESP_LOGW(
                    TAG, "first video sample: raw_size=%u spec_info_len=%u framing=%s raw_bytes=%s",
                    (unsigned)frame.frame_size, (unsigned)video_info.spec_info_len,
                    frame_data == nullptr ? "undetected" : (frame_data == remuxed ? "avcc->annexb" : "already-annexb"),
                    hex
                );
            }

            if (frame_data == nullptr) {
                ESP_LOGW(TAG, "video sample: unrecognized NAL framing, skipping frame");
                esp_extractor_release_frame(extractor, &frame);
                continue;
            }

            uint8_t *combined = nullptr;
            const uint8_t *cursor = frame_data;
            uint32_t remain = frame_data_len;
            if (!primed) {
                primed = true;
                if (video_info.spec_info != nullptr && video_info.spec_info_len > 0) {
                    uint32_t combined_len = video_info.spec_info_len + frame_data_len;
                    combined = static_cast<uint8_t *>(heap_caps_malloc(combined_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (combined == nullptr) combined = static_cast<uint8_t *>(heap_caps_malloc(combined_len, MALLOC_CAP_8BIT));
                    if (combined != nullptr) {
                        memcpy(combined, video_info.spec_info, video_info.spec_info_len);
                        memcpy(combined + video_info.spec_info_len, frame_data, frame_data_len);
                        cursor = combined;
                        remain = combined_len;
                    }
                }
            }
            if (use_tinyh264) {
                /* tinyh264's contract: hand it the whole multi-NAL buffer at
                 * once, it reports back how many bytes it consumed per call
                 * (usually one NAL's worth) via in_frame.consume -- the
                 * opposite of OpenH264's one-NAL-per-call contract below. */
                while (remain > 0) {
                    esp_h264_dec_in_frame_t in_frame = {};
                    in_frame.raw_data.buffer = const_cast<uint8_t *>(cursor);
                    in_frame.raw_data.len = remain;
                    esp_h264_dec_out_frame_t out_frame = {};
                    int64_t t_decode0 = esp_timer_get_time();
                    esp_h264_err_t dr = esp_h264_dec_process(h264_dec_tiny, &in_frame, &out_frame);
                    total_decode_us += esp_timer_get_time() - t_decode0;
                    if (dr != ESP_H264_ERR_OK) {
                        ESP_LOGW(TAG, "h264 decode failed: %d", (int)dr);
                        break;
                    }
                    if (in_frame.consume == 0) break;
                    cursor += in_frame.consume;
                    remain -= in_frame.consume;

                    if (out_frame.out_size > 0 && out_frame.outbuf != nullptr) {
                        int frame_w = video_info.video_info.width;
                        int frame_h = video_info.video_info.height;
                        if (dst_w == 0) {
                            esp_h264_resolution_t res = {video_info.video_info.width, video_info.video_info.height};
                            esp_h264_dec_param_sw_handle_t param_hd = nullptr;
                            if (esp_h264_dec_sw_get_param_hd(h264_dec_tiny, &param_hd) == ESP_H264_ERR_OK) {
                                esp_h264_dec_get_resolution(param_hd, &res);
                            }
                            frame_w = res.width;
                            frame_h = res.height;
                        }
                        /* tinyh264 hands back one tightly-packed I420 buffer
                         * (no per-plane stride/padding), unlike OpenH264's
                         * separate plane pointers + strides -- carve it up to
                         * match handle_decoded_picture's shared interface. */
                        const uint8_t *y = out_frame.outbuf;
                        const uint8_t *u = y + (size_t)frame_w * frame_h;
                        const uint8_t *v = u + (size_t)(frame_w / 2) * (frame_h / 2);
                        handle_decoded_picture(y, u, v, frame_w, frame_w / 2, frame_w, frame_h, frame.pts);
                    }
                }
            } else {
                size_t nal_pos = 0, nal_start = 0, nal_len = 0;
                while (next_nal(cursor, remain, &nal_pos, &nal_start, &nal_len)) {
                    uint8_t *out_y = nullptr, *out_u = nullptr, *out_v = nullptr;
                    int out_w = 0, out_h = 0, stride_y = 0, stride_uv = 0;
                    int64_t t_decode0 = esp_timer_get_time();
                    int dr = openh264_dec_decode(
                        h264_dec_open, cursor + nal_start, (int)nal_len, &out_y, &out_u, &out_v, &out_w, &out_h,
                        &stride_y, &stride_uv
                    );
                    total_decode_us += esp_timer_get_time() - t_decode0;
                    if (dr < 0) {
                        ESP_LOGW(TAG, "h264 decode failed at NAL offset %u", (unsigned)nal_start);
                        continue;
                    }
                    if (dr == 0) continue; /* SPS/PPS or not enough data yet for a picture */
                    handle_decoded_picture(out_y, out_u, out_v, stride_y, stride_uv, out_w, out_h, frame.pts);
                }
            }
            heap_caps_free(combined);
            heap_caps_free(remuxed);
        } else if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO && aac_dec != nullptr) {
            if (aac_pcm == nullptr) {
                aac_pcm_cap = 8192;
                aac_pcm = static_cast<uint8_t *>(
                    heap_caps_malloc(aac_pcm_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
                );
            }
            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = frame.frame_buffer;
            raw.len = frame.frame_size;
            while (aac_pcm != nullptr && raw.len > 0 && !s_stop_requested) {
                esp_audio_dec_out_frame_t out = {};
                out.buffer = aac_pcm;
                out.len = (uint32_t)aac_pcm_cap;
                esp_audio_dec_info_t info = {};
                esp_audio_err_t ar = esp_aac_dec_decode(aac_dec, &raw, &out, &info);
                if (ar == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    uint8_t *bigger = static_cast<uint8_t *>(
                        heap_caps_realloc(aac_pcm, out.needed_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
                    );
                    if (bigger == nullptr) break;
                    aac_pcm = bigger;
                    aac_pcm_cap = out.needed_size;
                    continue;
                }
                if (ar != ESP_AUDIO_ERR_OK) break;
                if (out.decoded_size > 0 && (info.channel == 1 || info.channel == 2)) {
                    got_any_frame = true;
                    if (!writer_started) {
                        esp_err_t wres = writer.begin(info.sample_rate != 0 ? info.sample_rate : audio_info.audio_info.sample_rate);
                        if (wres == ESP_OK) writer_started = true;
                    }
                    size_t samples_per_channel = out.decoded_size / (info.channel * sizeof(int16_t));
                    if (writer_started && info.channel == 2) {
                        writer.write(reinterpret_cast<int16_t *>(out.buffer), samples_per_channel, 2);
                    } else if (writer_started && info.channel == 1) {
                        size_t needed_bytes = samples_per_channel * 2 * sizeof(int16_t);
                        if (needed_bytes > stereo_pcm_cap) {
                            int16_t *bigger = static_cast<int16_t *>(heap_caps_realloc(
                                stereo_pcm, needed_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
                            ));
                            if (bigger != nullptr) {
                                stereo_pcm = bigger;
                                stereo_pcm_cap = needed_bytes;
                            }
                        }
                        if (stereo_pcm != nullptr && needed_bytes <= stereo_pcm_cap) {
                            const int16_t *mono = reinterpret_cast<int16_t *>(out.buffer);
                            for (size_t i = 0; i < samples_per_channel; i++) {
                                stereo_pcm[i * 2] = mono[i];
                                stereo_pcm[i * 2 + 1] = mono[i];
                            }
                            writer.write(stereo_pcm, samples_per_channel, 2);
                        }
                    }
                } else if (out.decoded_size > 0) {
                    ESP_LOGW(TAG, "unexpected AAC channel count: %u", (unsigned)info.channel);
                }
                if (raw.consumed == 0) break;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
        }
        esp_extractor_release_frame(extractor, &frame);
    }

    ESP_LOGI(TAG, "task stack high water mark: %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (rendered_frame_count > 0) {
        ESP_LOGW(
            TAG,
            "perf: %d frames -- avg read=%lldus decode=%lldus render=%lldus blit=%lldus (total/frame=%lldus, %.1f fps equiv)",
            rendered_frame_count, (long long)(total_read_us / rendered_frame_count),
            (long long)(total_decode_us / rendered_frame_count), (long long)(total_render_us / rendered_frame_count),
            (long long)(total_blit_us / rendered_frame_count),
            (long long)((total_read_us + total_decode_us + total_render_us + total_blit_us) / rendered_frame_count),
            (total_read_us + total_decode_us + total_render_us + total_blit_us) > 0
                ? 1000000.0 * rendered_frame_count / (double)(total_read_us + total_decode_us + total_render_us + total_blit_us)
                : 0.0
        );
    }

    writer.finish();
    heap_caps_free(aac_pcm);
    heap_caps_free(stereo_pcm);
    heap_caps_free(rgb_buf);
    if (aac_dec != nullptr) esp_aac_dec_close(aac_dec);
    if (use_tinyh264) {
        esp_h264_dec_close(h264_dec_tiny);
        esp_h264_dec_del(h264_dec_tiny);
    } else {
        openh264_dec_destroy(h264_dec_open);
    }
    esp_extractor_close(extractor);

    if (result != ESP_OK) return result;
    if (!got_any_frame) {
        set_last_error("no frames decoded");
        return ESP_FAIL;
    }
    set_last_error(s_stop_requested ? "stopped" : "played");
    return ESP_OK;
}
