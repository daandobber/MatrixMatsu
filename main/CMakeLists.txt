idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"audio_output.cpp"
		"audio_player_decode.cpp"
		"dobber_splash.c"
		"image_viewer.c"
		"main.c"
		"matrix_client.c"
		"video_player.cpp"
		"openh264_probe.cpp"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
		micro-opus
		esp_http_client
		esp_driver_i2s
		esp_driver_jpeg
		esp_jpeg
		esp_rom
		esp-tls
		mbedtls
		json
		esp_timer
		openh264_dec
		${extra_requires}
	INCLUDE_DIRS
		"."
	EMBED_FILES
		"test_assets/openh264_probe_clip.264"
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
