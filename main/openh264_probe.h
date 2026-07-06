#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Temporary feasibility probe (see project memory "video playback" notes):
 * decodes an embedded Main-profile H.264 test clip with the OpenH264
 * decoder port in components/openh264_dec and logs per-frame decode timing.
 * Answers "is a fuller-profile SW decoder fast enough on this chip", nothing
 * more -- remove once that question is answered and acted on. */
void openh264_probe_run(void);

#ifdef __cplusplus
}
#endif
