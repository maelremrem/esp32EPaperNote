idf_component_register(
    SRCS
        "main.cpp"
        "app/button.cpp"
        "audio/audio_recorder.cpp"
        "audio/wav_writer.cpp"
        "display/epaper_display.cpp"
        "display/ui.cpp"
        "display/wifi_qr.cpp"
        "display/qr/qrcodegen.c"
        "network/api_client.cpp"
        "network/live_preview.cpp"
        "network/wifi_manager.cpp"
        "network/captive_portal.cpp"
        "network/web_server.cpp"
        "storage/note_store.cpp"
    INCLUDE_DIRS
        "."
        "../include"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_http_server
        esp-tls
        nvs_flash
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        json
        esp_codec_dev
)

# PlatformIO/SCons does not run IDF's EMBED_FILES custom commands. Generate with
# IDF's own embed helper at configure time so both native IDF and PIO see real
# assembly sources; asset edits trigger reconfiguration.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    foreach(asset index.html style.css tokens.css app.js)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/web/${asset}")
        set(assembly "${CMAKE_CURRENT_BINARY_DIR}/carnet_${asset}.S")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
        execute_process(COMMAND "${CMAKE_COMMAND}"
            "-DDATA_FILE=${source}" "-DSOURCE_FILE=${assembly}"
            "-DVARIABLE_BASENAME=web_${asset}" "-DFILE_TYPE=BINARY"
            -P "$ENV{IDF_PATH}/tools/cmake/scripts/data_file_embed_asm.cmake"
            RESULT_VARIABLE embed_result)
        if(NOT embed_result EQUAL 0)
            message(FATAL_ERROR "Cannot embed Carnet asset ${asset}")
        endif()
        target_sources(${COMPONENT_LIB} PRIVATE "${assembly}")
    endforeach()
endif()
