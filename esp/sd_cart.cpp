#include "sd_cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <dirent.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define TAG "SD_CART"

const GameCart *sd_selected_cart = nullptr;

namespace {
struct Buffer {
    uint8_t *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;

    bool append(const void *src, size_t len) {
        if (size + len > capacity) {
            size_t next = capacity ? capacity * 2 : 1024;
            while (next < size + len) next *= 2;
            void *grown = realloc(data, next);
            if (!grown) return false;
            data = static_cast<uint8_t *>(grown);
            capacity = next;
        }
        memcpy(data + size, src, len);
        size += len;
        return true;
    }
};

struct LoadedCart {
    Buffer code, gfx, gff, map, sfx, label;
    char name[64] = {};
    GameCart cart;

    LoadedCart() : cart{0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                        0, nullptr, 0, nullptr, 0, nullptr} {}

    void release() {
        free(code.data); free(gfx.data); free(gff.data);
        free(map.data); free(sfx.data); free(label.data);
        code = {}; gfx = {}; gff = {}; map = {}; sfx = {}; label = {};
    }

    bool finish() {
        if (code.size > UINT16_MAX || gfx.size > UINT16_MAX ||
            gff.size > UINT16_MAX || map.size > UINT16_MAX ||
            sfx.size > UINT16_MAX || label.size > UINT16_MAX) {
            ESP_LOGE(TAG, "cart section exceeds runtime limit");
            return false;
        }
        cart.~GameCart();
        new (&cart) GameCart{
            static_cast<uint8_t>(strlen(name)), name,
            static_cast<uint16_t>(code.size), code.data,
            static_cast<uint16_t>(gff.size), gff.data,
            static_cast<uint16_t>(gfx.size), gfx.data,
            static_cast<uint16_t>(sfx.size), sfx.data,
            static_cast<uint16_t>(map.size), map.data,
            static_cast<uint16_t>(label.size), label.data};
        return true;
    }
};

LoadedCart loaded;
sdmmc_card_t *card = nullptr;
bool mounted = false;
static constexpr uint16_t MAX_CARTS = 24;
char cart_names[MAX_CARTS][64] = {};
char cart_paths[MAX_CARTS][128] = {};
uint16_t cart_count = 0;

enum Section { NONE, LUA, GFX, GFF, MAP, SFX, LABEL };

Buffer *section_buffer(Section section) {
    switch (section) {
    case LUA: return &loaded.code;
    case GFX: return &loaded.gfx;
    case GFF: return &loaded.gff;
    case MAP: return &loaded.map;
    case SFX: return &loaded.sfx;
    case LABEL: return &loaded.label;
    default: return nullptr;
    }
}

int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool append_encoded(Buffer *out, const char *line, size_t len, Section section) {
    if (section == LUA || section == SFX) {
        if (!out->append(line, len)) return false;
        if (section == LUA && !out->append("\n", 1)) return false;
        return true;
    }
    if (section == GFX || section == LABEL) {
        for (size_t i = 0; i < len; ++i) {
            int digit = hex_digit(static_cast<uint8_t>(line[i]));
            if (digit < 0 || !out->append(&digit, 1)) return false;
        }
        return true;
    }
    if (section == GFF || section == MAP) {
        for (size_t i = 0; i + 1 < len; i += 2) {
            int hi = hex_digit(static_cast<uint8_t>(line[i]));
            int lo = hex_digit(static_cast<uint8_t>(line[i + 1]));
            if (hi < 0 || lo < 0) return false;
            uint8_t value = static_cast<uint8_t>((hi << 4) | lo);
            if (!out->append(&value, 1)) return false;
        }
    }
    return true;
}

bool mount_card() {
    if (mounted) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // The ILI9341 driver uses HSPI_HOST on the original ESP32.  The default
    // SDSPI host is also HSPI there, so explicitly move the SD card to VSPI.
    host.slot = VSPI_HOST;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = CONFIG_SD_MOSI;
    bus.miso_io_num = CONFIG_SD_MISO;
    bus.sclk_io_num = CONFIG_SD_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    spi_host_device_t host_id = static_cast<spi_host_device_t>(host.slot);
    esp_err_t err = spi_bus_initialize(host_id, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SD SPI init failed: %s", esp_err_to_name(err));
        return false;
    }
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = static_cast<gpio_num_t>(CONFIG_SD_CS);
    dev.host_id = host_id;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;
    err = esp_vfs_fat_sdspi_mount("/sd", &host, &dev, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }
    mounted = true;
    ESP_LOGI(TAG, "SD mounted");
    return true;
}

bool load_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    loaded.release();
    const char *slash = strrchr(path, '/');
    const char *filename = slash ? slash + 1 : path;
    snprintf(loaded.name, sizeof(loaded.name), "%s", filename);
    Section section = NONE;
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
        if (len == 0) continue;
        if (line[0] != '_') {
            if (section == NONE) continue;
            Buffer *out = section_buffer(section);
            if (!out || !append_encoded(out, line, len, section)) {
                fclose(file); return false;
            }
            continue;
        }
        if (!strcmp(line, "__lua__")) section = LUA;
        else if (!strcmp(line, "__gfx__")) section = GFX;
        else if (!strcmp(line, "__gff__")) section = GFF;
        else if (!strcmp(line, "__map__")) section = MAP;
        else if (!strcmp(line, "__sfx__")) section = SFX;
        else if (!strcmp(line, "__label__")) section = LABEL;
        else section = NONE;
    }
    fclose(file);
    if (loaded.code.size == 0 || !loaded.finish()) {
        loaded.release();
        return false;
    }
    return true;
}

bool has_p8_suffix(const char *name) {
    size_t len = strlen(name);
    return len > 3 && name[len - 3] == '.' &&
           (name[len - 2] == 'p' || name[len - 2] == 'P') && name[len - 1] == '8';
}

void scan_directory(const char *directory, uint8_t depth) {
    if (depth > 4 || cart_count >= MAX_CARTS) return;
    DIR *dir = opendir(directory);
    if (!dir) return;
    while (struct dirent *entry = readdir(dir)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[128];
        size_t directory_len = strlen(directory);
        size_t entry_len = strlen(entry->d_name);
        if (directory_len + 1 + entry_len >= sizeof(path)) continue;
        memcpy(path, directory, directory_len);
        path[directory_len] = '/';
        memcpy(path + directory_len + 1, entry->d_name, entry_len + 1);
        struct stat info = {};
        if (entry->d_type == DT_UNKNOWN && stat(path, &info) != 0) continue;
        bool is_dir = entry->d_type == DT_DIR ||
                      (entry->d_type == DT_UNKNOWN && S_ISDIR(info.st_mode));
        if (is_dir) {
            scan_directory(path, depth + 1);
        } else if (has_p8_suffix(entry->d_name) && cart_count < MAX_CARTS) {
            const char *relative = path + 4; // strip "/sd/"
            strncpy(cart_paths[cart_count], path, sizeof(cart_paths[cart_count]) - 1);
            cart_paths[cart_count][sizeof(cart_paths[cart_count]) - 1] = '\0';
            strncpy(cart_names[cart_count], relative, sizeof(cart_names[cart_count]) - 1);
            cart_names[cart_count][sizeof(cart_names[cart_count]) - 1] = '\0';
            ++cart_count;
        }
    }
    closedir(dir);
    for (uint16_t i = 0; i < cart_count; ++i) {
        for (uint16_t j = i + 1; j < cart_count; ++j) {
            if (strcasecmp(cart_names[j], cart_names[i]) < 0) {
                char tmp[64];
                char path_tmp[128];
                memcpy(tmp, cart_names[i], sizeof(tmp));
                memcpy(path_tmp, cart_paths[i], sizeof(path_tmp));
                memcpy(cart_names[i], cart_names[j], sizeof(tmp));
                memcpy(cart_paths[i], cart_paths[j], sizeof(path_tmp));
                memcpy(cart_names[j], tmp, sizeof(tmp));
                memcpy(cart_paths[j], path_tmp, sizeof(path_tmp));
            }
        }
    }
}
}

bool sd_cart_scan(void) {
    if (!mount_card()) return false;
    cart_count = 0;
    scan_directory("/sd", 0);
    ESP_LOGI(TAG, "found %u .p8 cartridges", cart_count);
    return true;
}
uint16_t sd_cart_count(void) { return cart_count; }
const char *sd_cart_name(uint16_t index) {
    return index < cart_count ? cart_names[index] : nullptr;
}
bool sd_cart_load_index(uint16_t index) {
    if (index >= cart_count) return false;
    char path[96];
    snprintf(path, sizeof(path), "%s", cart_paths[index]);
    if (!load_file(path)) return false;
    sd_selected_cart = &loaded.cart;
    ESP_LOGI(TAG, "loaded %s (%u bytes Lua, %u bytes gfx)", loaded.name,
             loaded.cart.code_len, loaded.cart.gfx_len);
    return true;
}

bool sd_cart_load_boot(void) {
    if (!sd_cart_scan()) return false;
    for (uint16_t i = 0; i < cart_count; ++i)
        if (!strcasecmp(cart_names[i], CONFIG_SD_BOOT_CART)) return sd_cart_load_index(i);
    return false;
}
