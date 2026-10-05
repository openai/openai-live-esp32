idf_component_register(SRCS "live_esp32.cpp" INCLUDE_DIRS "include"
    REQUIRES live esp_peer esp_audio_codec esp_http_client mbedtls esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
