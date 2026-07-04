#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pax_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t image_viewer_decode(
    const uint8_t *data, size_t data_len, const char *mimetype, pax_buf_t *out_image, char *err_out, size_t err_out_len
);
void image_viewer_destroy(pax_buf_t *image);

#ifdef __cplusplus
}
#endif
