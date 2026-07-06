#include "image_viewer.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "miniz.h"

#define IMAGE_PREVIEW_MAX_DIM 720
#define IMAGE_MAX_IDAT_BYTES  (4 * 1024 * 1024)
#define IMAGE_MAX_RAW_BYTES   (12 * 1024 * 1024)
/* tjpgd's built-in default pool (3.1kB) is too small for some real-world photos
 * (extra Huffman/quant tables, 4:4:4 subsampling); give it a roomier scratch pool. */
#define JPEG_DECODE_WORKBUF_SIZE (32 * 1024)

static void set_err(char *err_out, size_t err_out_len, const char *text) {
    if (err_out != NULL && err_out_len > 0) snprintf(err_out, err_out_len, "%s", text);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool mime_is_jpeg(const char *mime) {
    return mime != NULL && (strstr(mime, "jpeg") != NULL || strstr(mime, "jpg") != NULL);
}

static bool mime_is_png(const char *mime) {
    return mime != NULL && strstr(mime, "png") != NULL;
}

static bool magic_is_jpeg(const uint8_t *data, size_t len) {
    return len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
}

static bool magic_is_png(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return len >= sizeof(sig) && memcmp(data, sig, sizeof(sig)) == 0;
}

static void preview_size(uint32_t width, uint32_t height, uint32_t *out_w, uint32_t *out_h) {
    uint32_t max_dim = width > height ? width : height;
    if (max_dim <= IMAGE_PREVIEW_MAX_DIM) {
        *out_w = width;
        *out_h = height;
        return;
    }
    *out_w = (width * IMAGE_PREVIEW_MAX_DIM + max_dim - 1) / max_dim;
    *out_h = (height * IMAGE_PREVIEW_MAX_DIM + max_dim - 1) / max_dim;
    if (*out_w == 0) *out_w = 1;
    if (*out_h == 0) *out_h = 1;
}

static bool parse_jpeg_size(const uint8_t *data, size_t data_len, uint32_t *out_w, uint32_t *out_h) {
    if (!magic_is_jpeg(data, data_len)) return false;
    size_t pos = 2;
    while (pos + 4 <= data_len) {
        while (pos < data_len && data[pos] == 0xff) pos++;
        if (pos >= data_len) return false;
        uint8_t marker = data[pos++];
        if (marker == 0xd9 || marker == 0xda) return false;
        if (marker >= 0xd0 && marker <= 0xd7) continue;
        if (pos + 2 > data_len) return false;
        uint16_t len = ((uint16_t)data[pos] << 8) | data[pos + 1];
        if (len < 2 || pos + len > data_len) return false;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            if (len < 7) return false;
            *out_h = ((uint16_t)data[pos + 3] << 8) | data[pos + 4];
            *out_w = ((uint16_t)data[pos + 5] << 8) | data[pos + 6];
            return *out_w > 0 && *out_h > 0;
        }
        pos += len;
    }
    return false;
}

static esp_err_t decode_jpeg(const uint8_t *data, size_t data_len, pax_buf_t *out, char *err, size_t err_len) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (!parse_jpeg_size(data, data_len, &width, &height)) {
        set_err(err, err_len, "jpg header");
        return ESP_FAIL;
    }

    uint8_t scale_div = 1;
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    uint32_t max_dim = width > height ? width : height;
    if (max_dim > IMAGE_PREVIEW_MAX_DIM * 4) {
        scale = JPEG_IMAGE_SCALE_1_8;
        scale_div = 8;
    } else if (max_dim > IMAGE_PREVIEW_MAX_DIM * 2) {
        scale = JPEG_IMAGE_SCALE_1_4;
        scale_div = 4;
    } else if (max_dim > IMAGE_PREVIEW_MAX_DIM) {
        scale = JPEG_IMAGE_SCALE_1_2;
        scale_div = 2;
    }

    uint32_t out_w_est = (width + scale_div - 1) / scale_div;
    uint32_t out_h_est = (height + scale_div - 1) / scale_div;
    uint16_t *pixels = heap_caps_malloc((size_t)out_w_est * (size_t)out_h_est * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) pixels = heap_caps_malloc((size_t)out_w_est * (size_t)out_h_est * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        set_err(err, err_len, "jpg no mem");
        return ESP_ERR_NO_MEM;
    }

    uint8_t *jpeg_workbuf = heap_caps_malloc(JPEG_DECODE_WORKBUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_workbuf == NULL) jpeg_workbuf = heap_caps_malloc(JPEG_DECODE_WORKBUF_SIZE, MALLOC_CAP_8BIT);
    if (jpeg_workbuf == NULL) {
        heap_caps_free(pixels);
        set_err(err, err_len, "jpg no mem");
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)data,
        .indata_size = data_len,
        .outbuf = (uint8_t *)pixels,
        .outbuf_size = (size_t)out_w_est * (size_t)out_h_est * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
        .advanced = {
            .working_buffer = jpeg_workbuf,
            .working_buffer_size = JPEG_DECODE_WORKBUF_SIZE,
        },
    };
    esp_jpeg_image_output_t decoded = {0};
    esp_err_t res = esp_jpeg_decode(&jpeg_cfg, &decoded);
    heap_caps_free(jpeg_workbuf);
    if (res != ESP_OK || decoded.width == 0 || decoded.height == 0) {
        heap_caps_free(pixels);
        if (err != NULL && err_len > 0) snprintf(err, err_len, "jpg %s", esp_err_to_name(res));
        return res == ESP_OK ? ESP_FAIL : res;
    }

    if (!pax_buf_init(out, pixels, (int)decoded.width, (int)decoded.height, PAX_BUF_16_565RGB)) {
        heap_caps_free(pixels);
        set_err(err, err_len, "pax image");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a);
    int pb = abs(p - (int)b);
    int pc = abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static esp_err_t append_idat(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *data, size_t add) {
    if (*len + add > IMAGE_MAX_IDAT_BYTES) return ESP_ERR_INVALID_SIZE;
    if (*len + add > *cap) {
        size_t next = *cap == 0 ? 16384 : *cap * 2;
        while (next < *len + add) next *= 2;
        if (next > IMAGE_MAX_IDAT_BYTES) next = IMAGE_MAX_IDAT_BYTES;
        uint8_t *new_buf = heap_caps_realloc(*buf, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_buf == NULL) new_buf = heap_caps_realloc(*buf, next, MALLOC_CAP_8BIT);
        if (new_buf == NULL) return ESP_ERR_NO_MEM;
        *buf = new_buf;
        *cap = next;
    }
    memcpy(*buf + *len, data, add);
    *len += add;
    return ESP_OK;
}

static int png_channels(uint8_t color_type) {
    switch (color_type) {
        case 0: return 1;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        case 6: return 4;
        default: return 0;
    }
}

static esp_err_t decode_png(const uint8_t *data, size_t data_len, pax_buf_t *out, char *err, size_t err_len) {
    if (!magic_is_png(data, data_len)) {
        set_err(err, err_len, "png magic");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
    uint8_t interlace = 0;
    pax_col_t palette[256] = {0};
    size_t palette_len = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0;
    size_t idat_cap = 0;

    size_t pos = 8;
    while (pos + 12 <= data_len) {
        uint32_t len = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        const uint8_t *chunk = data + pos + 8;
        if (pos + 12 + len > data_len) break;

        if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            width = be32(chunk);
            height = be32(chunk + 4);
            bit_depth = chunk[8];
            color_type = chunk[9];
            interlace = chunk[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            palette_len = len / 3;
            if (palette_len > 256) palette_len = 256;
            for (size_t i = 0; i < palette_len; i++) {
                palette[i] = 0xff000000 | ((pax_col_t)chunk[i * 3] << 16) | ((pax_col_t)chunk[i * 3 + 1] << 8) |
                             (pax_col_t)chunk[i * 3 + 2];
            }
        } else if (memcmp(type, "tRNS", 4) == 0 && palette_len > 0) {
            size_t n = len < palette_len ? len : palette_len;
            for (size_t i = 0; i < n; i++) palette[i] = (palette[i] & 0x00ffffff) | ((pax_col_t)chunk[i] << 24);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            esp_err_t res = append_idat(&idat, &idat_len, &idat_cap, chunk, len);
            if (res != ESP_OK) {
                heap_caps_free(idat);
                set_err(err, err_len, res == ESP_ERR_NO_MEM ? "png no mem" : "png too large");
                return res;
            }
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }

    int channels = png_channels(color_type);
    if (width == 0 || height == 0 || bit_depth != 8 || channels == 0 || interlace != 0 || idat_len == 0) {
        heap_caps_free(idat);
        set_err(err, err_len, "png unsupported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (color_type == 3 && palette_len == 0) {
        heap_caps_free(idat);
        set_err(err, err_len, "png palette");
        return ESP_FAIL;
    }

    size_t row_bytes = (size_t)width * (size_t)channels;
    size_t raw_len = (row_bytes + 1) * (size_t)height;
    if (raw_len > IMAGE_MAX_RAW_BYTES) {
        heap_caps_free(idat);
        set_err(err, err_len, "png too large");
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *raw = heap_caps_malloc(raw_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) raw = heap_caps_malloc(raw_len, MALLOC_CAP_8BIT);
    if (raw == NULL) {
        heap_caps_free(idat);
        set_err(err, err_len, "png no mem");
        return ESP_ERR_NO_MEM;
    }

    size_t inflated = tinfl_decompress_mem_to_mem(raw, raw_len, idat, idat_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
    heap_caps_free(idat);
    if (inflated != raw_len) {
        heap_caps_free(raw);
        set_err(err, err_len, "png inflate");
        return ESP_FAIL;
    }

    uint32_t out_w = 0;
    uint32_t out_h = 0;
    preview_size(width, height, &out_w, &out_h);
    pax_col_t *pixels = heap_caps_malloc((size_t)out_w * (size_t)out_h * sizeof(pax_col_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) pixels = heap_caps_malloc((size_t)out_w * (size_t)out_h * sizeof(pax_col_t), MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        heap_caps_free(raw);
        set_err(err, err_len, "png no mem");
        return ESP_ERR_NO_MEM;
    }

    uint8_t *prev = heap_caps_calloc(1, row_bytes, MALLOC_CAP_8BIT);
    uint8_t *cur = heap_caps_malloc(row_bytes, MALLOC_CAP_8BIT);
    if (prev == NULL || cur == NULL) {
        heap_caps_free(prev);
        heap_caps_free(cur);
        heap_caps_free(raw);
        heap_caps_free(pixels);
        set_err(err, err_len, "png no mem");
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t filter = raw[y * (row_bytes + 1)];
        const uint8_t *src = raw + y * (row_bytes + 1) + 1;
        for (size_t x = 0; x < row_bytes; x++) {
            uint8_t left = x >= (size_t)channels ? cur[x - channels] : 0;
            uint8_t up = prev[x];
            uint8_t up_left = x >= (size_t)channels ? prev[x - channels] : 0;
            switch (filter) {
                case 0: cur[x] = src[x]; break;
                case 1: cur[x] = (uint8_t)(src[x] + left); break;
                case 2: cur[x] = (uint8_t)(src[x] + up); break;
                case 3: cur[x] = (uint8_t)(src[x] + ((uint16_t)left + up) / 2); break;
                case 4: cur[x] = (uint8_t)(src[x] + paeth(left, up, up_left)); break;
                default:
                    heap_caps_free(prev);
                    heap_caps_free(cur);
                    heap_caps_free(raw);
                    heap_caps_free(pixels);
                    set_err(err, err_len, "png filter");
                    return ESP_FAIL;
            }
        }

        for (uint32_t oy = 0; oy < out_h; oy++) {
            uint32_t sample_y = (uint64_t)oy * height / out_h;
            if (sample_y != y) continue;
            for (uint32_t ox = 0; ox < out_w; ox++) {
                uint32_t x = (uint64_t)ox * width / out_w;
            const uint8_t *px = cur + (size_t)x * (size_t)channels;
            pax_col_t color = 0xff000000;
            if (color_type == 0) {
                color |= ((pax_col_t)px[0] << 16) | ((pax_col_t)px[0] << 8) | px[0];
            } else if (color_type == 2) {
                color |= ((pax_col_t)px[0] << 16) | ((pax_col_t)px[1] << 8) | px[2];
            } else if (color_type == 3) {
                color = px[0] < palette_len ? palette[px[0]] : 0xffff00ff;
            } else if (color_type == 4) {
                color = ((pax_col_t)px[1] << 24) | ((pax_col_t)px[0] << 16) | ((pax_col_t)px[0] << 8) | px[0];
            } else if (color_type == 6) {
                color = ((pax_col_t)px[3] << 24) | ((pax_col_t)px[0] << 16) | ((pax_col_t)px[1] << 8) | px[2];
            }
                pixels[(size_t)oy * out_w + ox] = color;
            }
        }
        uint8_t *tmp = prev;
        prev = cur;
        cur = tmp;
    }

    heap_caps_free(prev);
    heap_caps_free(cur);
    heap_caps_free(raw);

    if (!pax_buf_init(out, pixels, (int)out_w, (int)out_h, PAX_BUF_32_8888ARGB)) {
        heap_caps_free(pixels);
        set_err(err, err_len, "pax image");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t image_viewer_decode(
    const uint8_t *data, size_t data_len, const char *mimetype, pax_buf_t *out_image, char *err_out, size_t err_out_len
) {
    if (out_image == NULL) return ESP_ERR_INVALID_ARG;
    memset(out_image, 0, sizeof(*out_image));
    if (data == NULL || data_len == 0) {
        set_err(err_out, err_out_len, "empty image");
        return ESP_ERR_INVALID_ARG;
    }
    if (mime_is_jpeg(mimetype) || magic_is_jpeg(data, data_len)) return decode_jpeg(data, data_len, out_image, err_out, err_out_len);
    if (mime_is_png(mimetype) || magic_is_png(data, data_len)) return decode_png(data, data_len, out_image, err_out, err_out_len);
    set_err(err_out, err_out_len, "image type");
    return ESP_ERR_NOT_SUPPORTED;
}

void image_viewer_destroy(pax_buf_t *image) {
    if (image == NULL || image->buf == NULL) return;
    void *pixels = image->buf;
    pax_buf_destroy(image);
    heap_caps_free(pixels);
    memset(image, 0, sizeof(*image));
}
