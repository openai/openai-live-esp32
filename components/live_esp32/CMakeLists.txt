idf_component_register(SRCS "live_esp32.cpp" "https_transport.cpp" INCLUDE_DIRS "include"
    REQUIRES live esp_peer esp_audio_codec esp_http_client mbedtls esp_timer tcp_transport)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
