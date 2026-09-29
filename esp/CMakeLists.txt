set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(srcs "hello_world_main.cpp" "sd_cart.cpp" "st7789.c" "st7789.h" "../src/generated/static_game_data.h")
set(bt_requires bt esp_hid)
set(network_requires)
if(CONFIG_BLUETOOTH_GAMEPAD)
        list(APPEND srcs "bluetooth_gamepad.cpp")
else()
        set(network_requires esp_wifi)
endif()
idf_component_register(SRCS ${srcs}
        INCLUDE_DIRS "lua" ".." "../src"
        REQUIRES esp_timer vfs fatfs sdmmc driver esp_hw_support log spi_flash nvs_flash ${network_requires} ${bt_requires})

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
