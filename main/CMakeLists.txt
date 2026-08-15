set(BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")
set(WEB_ASSETS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/web_assets.h")
set(APP_SCRIPT_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../scripts/tools")

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_custom_target(build_info
        COMMAND "${PYTHON}" "${APP_SCRIPT_TOOLS_DIR}/generate_build_info.py"
                "${BUILD_INFO_HEADER}" "${CMAKE_CURRENT_LIST_DIR}/.."
        BYPRODUCTS "${BUILD_INFO_HEADER}"
        DEPENDS "${APP_SCRIPT_TOOLS_DIR}/generate_build_info.py"
        VERBATIM
    )

    set(WEB_ASSET_FILES
        "${CMAKE_CURRENT_LIST_DIR}/../web/favicon.svg"
        "${CMAKE_CURRENT_LIST_DIR}/../web/modals.html"
        "${CMAKE_CURRENT_LIST_DIR}/../web/payout-panel.html"
        "${CMAKE_CURRENT_LIST_DIR}/../web/root.html"
        "${CMAKE_CURRENT_LIST_DIR}/../web/root.js"
        "${CMAKE_CURRENT_LIST_DIR}/../web/setup.html"
        "${CMAKE_CURRENT_LIST_DIR}/../web/stats-boxes.html"
        "${CMAKE_CURRENT_LIST_DIR}/../web/stats-summary.html"
        "${CMAKE_CURRENT_LIST_DIR}/../web/styles.css"
        "${CMAKE_CURRENT_LIST_DIR}/../web/toolbar.html"
    )
    add_custom_command(
        OUTPUT "${WEB_ASSETS_HEADER}"
        COMMAND "${PYTHON}" "${APP_SCRIPT_TOOLS_DIR}/embed_web_assets.py"
                "${WEB_ASSETS_HEADER}" "${CMAKE_CURRENT_LIST_DIR}/../web" ${WEB_ASSET_FILES}
        DEPENDS "${APP_SCRIPT_TOOLS_DIR}/embed_web_assets.py" ${WEB_ASSET_FILES}
        VERBATIM
    )
    add_custom_target(web_assets DEPENDS "${WEB_ASSETS_HEADER}")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "asic_frequency_transition.c"
        "asic_crc.c"
        "asic_pll.c"
        "asic_serial.c"
        "bitaxe_fan.c"
        "bitaxe_hw.c"
        "bitmain_asic.c"
        "bm1370.c"
        "m45_config.c"
        "m45_fault_log.c"
        "m45_log_buffer.c"
        "mining.c"
        "m45_oled.c"
        "i2c_bitaxe.c"
        "stratum_minimal.c"
        "stratum_payout.c"
        "tps546.c"
        "utils.c"
        "wifi_http.c"
        "wifi_http_status.c"
        "wifi_swarm.c"
        "third_party/qrcode/qrcode.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_BINARY_DIR}"
        "third_party/qrcode"
    PRIV_REQUIRES
        app_update
        driver
        esp_driver_i2c
        esp_event
        esp_http_server
        esp_hw_support
        esp_lcd
        esp_netif
        esp_timer
        esp_wifi
        freertos
        json
        lwip
        mbedtls
        nvs_flash
        pthread
        tcp_transport
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_dependencies(${COMPONENT_LIB} build_info web_assets)
endif()

option(M45_ASIC_LOSS_METRICS "Build ASIC feed/RX loss instrumentation into status JSON" OFF)
if(M45_ASIC_LOSS_METRICS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M45_ASIC_LOSS_METRICS=1)
endif()

option(M45_STRATUM_FAST_PATHS "Use default-on fast Stratum JSON and hex hot paths" ON)
if(M45_STRATUM_FAST_PATHS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M45_STRATUM_FAST_PATHS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M45_STRATUM_FAST_PATHS=0)
endif()
