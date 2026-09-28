set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(srcs "hello_world_main.cpp" "st7789.c" "st7789.h" "../src/generated/static_game_data.h")
set(bt_requires)
if(CONFIG_BLUETOOTH_GAMEPAD)
        list(APPEND srcs "bluetooth_gamepad.cpp")
        set(bt_requires bt esp_hid)
endif()
set(SPLORE_RAW "${CMAKE_CURRENT_BINARY_DIR}/splore565.bin")
execute_process(
        COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../scripts/png_to_rgb565.py"
                "${CMAKE_CURRENT_LIST_DIR}/../star_splore.p8.png" "${SPLORE_RAW}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        RESULT_VARIABLE SPLORE_CONVERT_RESULT
        OUTPUT_QUIET)
if(NOT SPLORE_CONVERT_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not convert star_splore.p8.png")
endif()
idf_component_register(SRCS ${srcs}
        INCLUDE_DIRS "lua" ".." "../src"
        REQUIRES esp_timer vfs driver esp_hw_support log spi_flash esp_wifi esp_http_server nvs_flash ${bt_requires}
        EMBED_FILES "${SPLORE_RAW}")

# Audio is deliberately deprecated for the ESP32 bring-up target.  Keep the
# API stubs so carts continue to load, but do not reserve audio state, buffers,
# or an I2S task until M1 is stable.
target_compile_options(${COMPONENT_LIB} PRIVATE
        -DPICO8_DISABLE_AUDIO=1
        # The legacy Pico-8 sources use printf formats that are accepted by
        # the PC toolchain but diagnosed more aggressively by Xtensa GCC.
        -Wno-error=format)

add_subdirectory(lua)
target_link_libraries(${COMPONENT_LIB} PUBLIC my_lua_lib)
