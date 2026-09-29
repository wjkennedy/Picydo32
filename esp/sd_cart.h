#pragma once

#include "sdkconfig.h"
#include "fix32.h"
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH CONFIG_WIDTH
#define SCREEN_HEIGHT CONFIG_HEIGHT
#endif
#include "cart.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount the CYD SD card and load CONFIG_SD_BOOT_CART into heap-backed storage.
// The returned cart remains valid until the next call.
bool sd_cart_load_boot(void);
bool sd_cart_scan(void);
uint16_t sd_cart_count(void);
const char *sd_cart_name(uint16_t index);
bool sd_cart_load_index(uint16_t index);
extern const GameCart *sd_selected_cart;

#ifdef __cplusplus
}
#endif
