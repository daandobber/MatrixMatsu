#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct openh264_dec_s openh264_dec_t;

/* Creates and initializes an OpenH264 AVC decoder. Returns NULL on failure. */
openh264_dec_t *openh264_dec_create(void);
void openh264_dec_destroy(openh264_dec_t *dec);

/* Feed one Annex-B buffer (SPS/PPS/slice NALs, start-code prefixed -- same
 * convention as esp_h264's tinyh264 SW decoder already uses in this repo).
 * Returns 1 if a picture was produced (out_* filled in, valid until the next
 * call or until the decoder is destroyed), 0 if no picture yet (e.g. this
 * buffer only contained SPS/PPS), negative on error. */
int openh264_dec_decode(
    openh264_dec_t *dec, const uint8_t *data, int len, uint8_t **out_y, uint8_t **out_u, uint8_t **out_v,
    int *out_width, int *out_height, int *out_stride_y, int *out_stride_uv
);

#ifdef __cplusplus
}
#endif
