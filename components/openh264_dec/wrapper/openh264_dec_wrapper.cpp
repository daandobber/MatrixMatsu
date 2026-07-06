#include "openh264_dec_wrapper.h"

#include <cstring>

#include "codec_api.h"

struct openh264_dec_s {
    ISVCDecoder *decoder;
};

openh264_dec_t *openh264_dec_create(void) {
    ISVCDecoder *decoder = nullptr;
    if (WelsCreateDecoder(&decoder) != 0 || decoder == nullptr) return nullptr;

    /* Tried DECODER_OPTION_NUM_OF_THREADS=2 to use both of ESP32-P4's HP
     * cores (decode measured as ~77% of per-frame playback time) -- reverted.
     * OpenH264's multi-context decode expects each decode call to hand it one
     * complete access unit, round-robining calls across the N contexts; our
     * NAL-by-NAL feeding (see video_player.cpp's next_nal loop) means SPS/PPS
     * and the first slice can land in *different* contexts, so a later
     * context's pSps is never populated. Crashed with a load access fault in
     * ParseAccessUnit (m_pLastDecThrCtx->pCtx->pSps null) on the very first
     * frame. Using multi-threaded decode for real would need reworking the
     * feeding strategy to submit whole access units per call, not individual
     * NALs -- a substantial redesign for an uncertain payoff. Stayed single-
     * threaded (m_iThreadCount defaults to 0, no SetOption call needed). */

    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.size = sizeof(SVideoProperty);
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    param.eEcActiveIdc = ERROR_CON_DISABLE;

    if (decoder->Initialize(&param) != cmResultSuccess) {
        WelsDestroyDecoder(decoder);
        return nullptr;
    }

    openh264_dec_t *dec = new openh264_dec_s();
    dec->decoder = decoder;
    return dec;
}

void openh264_dec_destroy(openh264_dec_t *dec) {
    if (dec == nullptr) return;
    if (dec->decoder != nullptr) {
        dec->decoder->Uninitialize();
        WelsDestroyDecoder(dec->decoder);
    }
    delete dec;
}

int openh264_dec_decode(
    openh264_dec_t *dec, const uint8_t *data, int len, uint8_t **out_y, uint8_t **out_u, uint8_t **out_v,
    int *out_width, int *out_height, int *out_stride_y, int *out_stride_uv
) {
    if (dec == nullptr || dec->decoder == nullptr) return -1;

    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    uint8_t *planes[3] = {nullptr, nullptr, nullptr};

    /* DecodeFrame2, not DecodeFrameNoDelay: in single-threaded mode,
     * DecodeFrameNoDelay calls DecodeFrame2 with the real data and then
     * immediately again with (NULL, 0), which flags end-of-stream internally.
     * Doing that after every single NAL (rather than just at the true end of
     * the stream) corrupts the B-frame reordering/reference state -- it
     * decoded without erroring, but produced garbled output. DecodeFrame2
     * alone is the correct call for streaming NAL-by-NAL; picture output
     * will lag by the decoder's own reorder depth, which is normal. */
    DECODING_STATE state = dec->decoder->DecodeFrame2(data, len, planes, &info);
    if (state != dsErrorFree) return -1;
    if (info.iBufferStatus != 1) return 0;

    *out_y         = info.pDst[0];
    *out_u         = info.pDst[1];
    *out_v         = info.pDst[2];
    *out_width     = info.UsrData.sSystemBuffer.iWidth;
    *out_height    = info.UsrData.sSystemBuffer.iHeight;
    *out_stride_y  = info.UsrData.sSystemBuffer.iStride[0];
    *out_stride_uv = info.UsrData.sSystemBuffer.iStride[1];
    return 1;
}
