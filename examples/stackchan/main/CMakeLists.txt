idf_component_register(SRCS "main.cpp" "../../m5stack/main/Audio.cpp" "../../m5stack/main/Camera.cpp" "Motion.cpp" "Cinema.cpp" "CinemaStorage.cpp" "../../common/StandaloneWifi.cpp" "../../m5stack/main/View.cpp"
    "../../common/fonts/live_font_16.c" "../../common/fonts/live_font_18.c"
    "../../common/fonts/live_font_22.c"
    INCLUDE_DIRS "." "../../m5stack/main" "../../common" "../../common/fonts"
    PRIV_INCLUDE_DIRS "$ENV{LIVE_VOICE_PRIVATE_DIR}"
    REQUIRES live_esp32 m5stack_core_s3 esp_video esp_new_jpeg esp_wifi esp_event esp_netif
        nvs_flash esp_driver_uart esp_driver_usb_serial_jtag esp_app_format esp_http_client)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

if(DEFINED ENV{STACKCHAN_CINEMA_PACK} AND NOT "$ENV{STACKCHAN_CINEMA_PACK}" STREQUAL "")
    set(cinema_pack "$ENV{STACKCHAN_CINEMA_PACK}")
    if(NOT EXISTS "${cinema_pack}/CinemaAssets.hpp" OR NOT EXISTS "${cinema_pack}/fallback.bin")
        message(FATAL_ERROR "STACKCHAN_CINEMA_PACK must name a prepared pack directory")
    endif()
    target_include_directories(${COMPONENT_LIB} PRIVATE "${cinema_pack}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STACKCHAN_CINEMA=1)
    target_add_binary_data(${COMPONENT_LIB} "${cinema_pack}/fallback.bin" BINARY)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_lcd_new_panel_io_spi")
endif()
