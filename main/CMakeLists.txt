idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.c"
		"matrix_client.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		bt
		badge-bsp
		tanmatsu-wifi
		esp_http_client
		esp-tls
		mbedtls
		json
		esp_timer
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
