idf_build_get_property(target IDF_TARGET)

file(GLOB DEC_CORE_SRCS "openh264/decoder/core/src/*.cpp")
file(GLOB COMMON_SRCS "openh264/common/src/*.cpp")

idf_component_register(
    SRCS
        ${DEC_CORE_SRCS}
        ${COMMON_SRCS}
        "openh264/decoder/plus/src/welsDecoderExt.cpp"
        "wrapper/openh264_dec_wrapper.cpp"
    INCLUDE_DIRS
        "openh264/api/wels"
        "wrapper"
    PRIV_INCLUDE_DIRS
        "openh264/common/inc"
        "openh264/decoder/core/inc"
        "openh264/decoder/plus/inc"
    REQUIRES
        pthread
)

# OpenH264 is plain portable C++; nothing here needs any architecture-specific
# SIMD path (those live under common/<arch>/ and decoder/core/<arch>/, which are
# deliberately NOT added to SRCS above -- only the generic fallback is built).
#
# -O2 overrides this project's global -Os: decode is CPU-bound (measured
# ~130ms/frame, ~77% of total playback time), so optimizing this component for
# speed rather than size is worth the extra flash it costs.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter -Wno-sign-compare -Wno-format -O2)
