#include "freertos/queue.h"
//#include "st7789.c"
#include "ili9340.c"
#include "esp_attr.h"
#ifndef PICO8_DISABLE_AUDIO
#include "driver/i2s.h"
#endif
#include "driver/adc.h"
#include "data.h"
#include "engine.c"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/uart.h"
#include <math.h>
#include <time.h>

#if CONFIG_BLUETOOTH_GAMEPAD
extern "C" uint8_t bluetooth_gamepad_buttons(void);
#endif

#ifndef PICO8_DISABLE_AUDIO
static const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE>>1,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, /* the DAC module will only take the 8bits from MSB */
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,//(i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = 0, // default interrupt priority
    .dma_buf_count = SAMPLES_PER_BUFFER, // always want >1; having only one means interruptions in audio
    // changed on the newer sdk version
    //.dma_desc_num = SAMPLES_PER_BUFFER, // always want >1; having only one means interruptions in audio
    //.dma_frame_num = SAMPLES_PER_DURATION, // length at most = 1024
    .dma_buf_len = SAMPLES_PER_DURATION, // length at most = 1024
    .use_apll = false, // > 16MHz
    .tx_desc_auto_clear = false,
};
static const i2s_pin_config_t i2s_pin_config = {
    .bck_io_num = CONFIG_GPIO_AUDIO_BCLK, // BCLK "Bit clock line"
    .ws_io_num = CONFIG_GPIO_AUDIO_WS, // LRC also "LRCLK" or WS
    .data_out_num = CONFIG_GPIO_AUDIO_DATA_OUT, // DIN !! not SD (which is SHUTDOWN)
    .data_in_num = I2S_PIN_NO_CHANGE
};
#endif

static uint8_t backbuffer[CONFIG_WIDTH*CONFIG_HEIGHT*2];
static uint8_t scaled_line[240 * 2];
extern const uint8_t _binary_splore565_bin_start[];
extern const uint8_t _binary_splore565_bin_end[];
static QueueHandle_t q;
static QueueHandle_t i2s_event_queue;
uint8_t FLAG = 1;
uint32_t bytesTX = 0;
// static uint16_t backbuffer[SCREEN_WIDTH*SCREEN_HEIGHT];

uint8_t buttons_prev[6] =  {0, 0, 0, 0, 0, 0};
static uint32_t keyboard_buttons;
static uint32_t keyboard_buttons_until[6];
static uint8_t keyboard_escape;
uint32_t now();
uint8_t current_hour();
uint8_t current_minute();
static bool wifi_connected;
static bool web_server_running;
static bool wifi_setup_ap;
static httpd_handle_t web_server;
static char wifi_ssid[33];
static char wifi_password[65];
static char wifi_ip[16] = "offline";
static void start_web_server(void);

static void copy_cstr(char *dst, size_t dst_len, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_len)
        len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool is_display_gpio(int pin) {
    return pin == CONFIG_MOSI_GPIO || pin == CONFIG_TFT_MISO ||
           pin == CONFIG_SCLK_GPIO || pin == CONFIG_CS_GPIO ||
           pin == CONFIG_DC_GPIO || pin == CONFIG_RESET_GPIO ||
           pin == CONFIG_BL_GPIO;
}

static void add_button_gpio(uint64_t *mask, int pin) {
    if (pin >= 0 && pin < 64 && !is_display_gpio(pin))
        *mask |= (1ULL << pin);
}

static int read_button_gpio(int pin) {
    return (pin >= 0 && !is_display_gpio(pin))
        ? gpio_get_level((gpio_num_t)pin)
        : 0;
}

//static inline void put_pixel(uint8_t x, uint8_t y, const uint8_t* p);
void put_buffer();

void video_close(){
}

void gfx_flip() {
    // memcpy(backbuffer, frontbuffer, sizeof(frontbuffer));
    // Flip endianness
    // put_buffer();

    for(uint8_t y=0; y<SCREEN_HEIGHT; y++)
        for(uint8_t x=0; x<SCREEN_WIDTH; x++) {
            palidx_t p = get_pixel(x, y);
            color_t c = palette[p];
            backbuffer[y*SCREEN_WIDTH*2+x*2  ] = (c >> 8);
            backbuffer[y*SCREEN_WIDTH*2+x*2+1] = c & 0xFF;
        }
    xQueueSendToBack(q, (void*)&FLAG, (TickType_t) 0);
}

void delay(uint16_t ms) {
    TickType_t ticks = ms / portTICK_PERIOD_MS;
    vTaskDelay(ticks ? ticks : 1);
}
#ifndef PICO8_DISABLE_AUDIO
void i2sTask(void*) {
    uint16_t samples = SAMPLES_PER_DURATION * SAMPLES_PER_BUFFER;
    uint32_t bytesOut;
    while (true) {
        i2s_event_t event;
        // Wait indefinitely for a new message in the queue
        if (xQueueReceive(i2s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (event.type == I2S_EVENT_TX_DONE) {
                memset(audiobuf, 0, sizeof(audiobuf));

                for(uint8_t i=0; i<4; i++)
                    fill_buffer(audiobuf, &channels[i], samples);

                i2s_write(I2S_NUM_0, audiobuf, sizeof(audiobuf), &bytesOut, 100);
            }
        }
    }
}
#endif


bool init_platform() {
    /*
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    */
    return true;
}

static void init_wifi(void) {
    copy_cstr(wifi_ssid, sizeof(wifi_ssid), CONFIG_WIFI_SSID);
    copy_cstr(wifi_password, sizeof(wifi_password), CONFIG_WIFI_PASSWORD);

    nvs_flash_init();
    nvs_handle_t nvs;
    if (nvs_open("picopico", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(wifi_ssid);
        nvs_get_str(nvs, "ssid", wifi_ssid, &len);
        len = sizeof(wifi_password);
        nvs_get_str(nvs, "password", wifi_password, &len);
        nvs_close(nvs);
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("esp_netif_init failed: %s\n", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("event loop init failed: %s\n", esp_err_to_name(err));
        return;
    }
    if (wifi_ssid[0] == '\0') {
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        wifi_config_t ap = {};
        copy_cstr((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "PicoPico-Setup");
        copy_cstr((char *)ap.ap.password, sizeof(ap.ap.password), "picopico8");
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.max_connection = 2;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_setup_ap = true;
        copy_cstr(wifi_ip, sizeof(wifi_ip), "192.168.4.1");
        printf("WiFi setup AP active: PicoPico-Setup / picopico8\n");
        start_web_server();
        return;
    }

    esp_netif_create_default_wifi_sta();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        [](void *, esp_event_base_t base, int32_t id, void *) {
            if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
                wifi_connected = false;
                esp_wifi_connect();
            }
            if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
                wifi_connected = true;
        }, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        [](void *, esp_event_base_t, int32_t, void *event_data) {
            wifi_connected = true;
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(wifi_ip, sizeof(wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        }, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t station = {};
    copy_cstr((char *)station.sta.ssid, sizeof(station.sta.ssid), wifi_ssid);
    copy_cstr((char *)station.sta.password, sizeof(station.sta.password), wifi_password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    start_web_server();
    printf("WiFi station starting for SSID '%s'\n", wifi_ssid);
}

static void status_glyph(char c, uint8_t rows[7]) {
    memset(rows, 0, 7);
    switch (c) {
    case 'A': { uint8_t r[] = {14,17,17,31,17,17,17}; memcpy(rows,r,7); break; }
    case 'D': { uint8_t r[] = {30,17,17,17,17,17,30}; memcpy(rows,r,7); break; }
    case 'E': { uint8_t r[] = {31,16,16,30,16,16,31}; memcpy(rows,r,7); break; }
    case 'F': { uint8_t r[] = {31,16,16,30,16,16,16}; memcpy(rows,r,7); break; }
    case 'I': { uint8_t r[] = {31,4,4,4,4,4,31}; memcpy(rows,r,7); break; }
    case 'N': { uint8_t r[] = {17,25,21,19,17,17,17}; memcpy(rows,r,7); break; }
    case 'O': { uint8_t r[] = {14,17,17,17,17,17,14}; memcpy(rows,r,7); break; }
    case 'P': { uint8_t r[] = {30,17,17,30,16,16,16}; memcpy(rows,r,7); break; }
    case 'S': { uint8_t r[] = {15,16,16,14,1,1,30}; memcpy(rows,r,7); break; }
    case 'T': { uint8_t r[] = {31,4,4,4,4,4,4}; memcpy(rows,r,7); break; }
    case 'W': { uint8_t r[] = {17,17,17,21,21,10,10}; memcpy(rows,r,7); break; }
    case '0': { uint8_t r[] = {14,17,19,21,25,17,14}; memcpy(rows,r,7); break; }
    case '1': { uint8_t r[] = {4,12,4,4,4,4,14}; memcpy(rows,r,7); break; }
    case '2': { uint8_t r[] = {14,17,1,2,4,8,31}; memcpy(rows,r,7); break; }
    case '3': { uint8_t r[] = {30,1,1,14,1,1,30}; memcpy(rows,r,7); break; }
    case '4': { uint8_t r[] = {2,6,10,18,31,2,2}; memcpy(rows,r,7); break; }
    case '5': { uint8_t r[] = {31,16,16,30,1,1,30}; memcpy(rows,r,7); break; }
    case '6': { uint8_t r[] = {14,16,16,30,17,17,14}; memcpy(rows,r,7); break; }
    case '7': { uint8_t r[] = {31,1,2,4,8,8,8}; memcpy(rows,r,7); break; }
    case '8': { uint8_t r[] = {14,17,17,14,17,17,14}; memcpy(rows,r,7); break; }
    case '9': { uint8_t r[] = {14,17,17,15,1,1,14}; memcpy(rows,r,7); break; }
    case ':': rows[2] = rows[4] = 4; break;
    case '.': rows[6] = 4; break;
    case '-': rows[3] = 14; break;
    }
}

static void status_text(uint16_t x, uint16_t y, const char *text, uint16_t color) {
    while (*text && x < 232) {
        uint8_t rows[7];
        char c = *text++;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        status_glyph(c, rows);
        for (uint8_t row = 0; row < 7; row++)
            for (uint8_t col = 0; col < 5; col++)
                if (rows[row] & (1u << (4 - col)))
                    lcdDrawFillRect(&dev, x + col, y + row,
                                    x + col, y + row, color);
        x += 7;
    }
}

static void keyboard_press(uint8_t button) {
    if (button >= 6)
        return;
    keyboard_buttons |= 1u << button;
    keyboard_buttons_until[button] = now() + 180;
}

static esp_err_t web_index(httpd_req_t *req) {
    const char *page =
        "<!doctype html><meta name='viewport' content='width=device-width'>"
        "<title>PicoPico CYD</title><h1>PicoPico CYD</h1>"
        "<p>WiFi: <b id='s'>loading</b></p>"
        "<form method='post' action='/config'>"
        "<label>SSID <input name='ssid' maxlength='32'></label><br>"
        "<label>Password <input name='password' type='password' maxlength='64'></label><br>"
        "<button>Save and reconnect</button></form>"
        "<script>fetch('/status').then(r=>r.json()).then(x=>s.textContent=x.wifi?'connected':'offline')</script>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_status(httpd_req_t *req) {
    char status[96];
    snprintf(status, sizeof(status), "{\"wifi\":%s,\"server\":true,\"setup\":%s}",
             wifi_connected ? "true" : "false",
             wifi_setup_ap ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, status, HTTPD_RESP_USE_STRLEN);
}

static void form_value(const char *body, const char *key, char *out, size_t out_len) {
    char needle[24];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *start = strstr(body, needle);
    if (!start) { out[0] = '\0'; return; }
    start += strlen(needle);
    size_t i = 0;
    while (start[i] && start[i] != '&' && i + 1 < out_len) {
        out[i] = start[i] == '+' ? ' ' : start[i];
        i++;
    }
    out[i] = '\0';
}

static esp_err_t web_config(httpd_req_t *req) {
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0)
        return ESP_FAIL;
    body[received] = '\0';

    char new_ssid[33], new_password[65];
    form_value(body, "ssid", new_ssid, sizeof(new_ssid));
    form_value(body, "password", new_password, sizeof(new_password));
    if (new_ssid[0]) {
        copy_cstr(wifi_ssid, sizeof(wifi_ssid), new_ssid);
        copy_cstr(wifi_password, sizeof(wifi_password), new_password);
        nvs_handle_t nvs;
        if (nvs_open("picopico", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "ssid", wifi_ssid);
            nvs_set_str(nvs, "password", wifi_password);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        printf("WiFi settings saved; reboot to apply\n");
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, "Saved. Reboot the device to reconnect with the new settings.");
}

static void start_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&web_server, &config) != ESP_OK)
        return;
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = web_index, .user_ctx = NULL};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = web_status, .user_ctx = NULL};
    httpd_uri_t config_uri = {.uri = "/config", .method = HTTP_POST, .handler = web_config, .user_ctx = NULL};
    httpd_register_uri_handler(web_server, &root);
    httpd_register_uri_handler(web_server, &status);
    httpd_register_uri_handler(web_server, &config_uri);
    web_server_running = true;
    printf("WiFi configuration server listening on port 80\n");
}

static void sidebar_digit(uint16_t x, uint16_t y, uint8_t digit, uint16_t color) {
    static const uint8_t segments[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66,
        0x6d, 0x7d, 0x07, 0x7f, 0x6f
    };
    const uint8_t mask = segments[digit % 10];
    if (mask & 0x01) lcdDrawFillRect(&dev, x + 2, y, x + 9, y + 2, color);
    if (mask & 0x02) lcdDrawFillRect(&dev, x + 9, y + 2, x + 11, y + 10, color);
    if (mask & 0x04) lcdDrawFillRect(&dev, x + 9, y + 12, x + 11, y + 20, color);
    if (mask & 0x08) lcdDrawFillRect(&dev, x + 2, y + 20, x + 9, y + 22, color);
    if (mask & 0x10) lcdDrawFillRect(&dev, x, y + 12, x + 2, y + 20, color);
    if (mask & 0x20) lcdDrawFillRect(&dev, x, y + 2, x + 2, y + 10, color);
    if (mask & 0x40) lcdDrawFillRect(&dev, x + 2, y + 10, x + 9, y + 12, color);
}

static void draw_sidebar(void) {
    static uint32_t last_update;
    uint32_t timestamp = now();
    if (timestamp - last_update < 250)
        return;
    last_update = timestamp;

    lcdDrawFillRect(&dev, 0, 256, 239, 319, 0x18e3);
    lcdDrawFillRect(&dev, 8, 264, 23, 279, wifi_connected ? 0x07e0 : 0xf800);
    lcdDrawFillRect(&dev, 32, 264, 47, 279, web_server_running ? 0x07e0 : 0xf800);
    status_text(8, 286, "WIFI", 0xffff);
    status_text(42, 286, "WEB", 0xffff);
    status_text(8, 296, "SSID", 0xffff);
    status_text(42, 296, wifi_ssid[0] ? wifi_ssid : "NONE", 0xffff);
    status_text(8, 306, "IP", 0xffff);
    status_text(42, 306, wifi_ip, 0xffff);

    uint8_t hour = current_hour();
    uint8_t minute = current_minute();
    sidebar_digit(64, 264, hour / 10, 0xffff);
    sidebar_digit(78, 264, hour % 10, 0xffff);
    lcdDrawFillRect(&dev, 93, 271, 95, 274, 0xffff);
    lcdDrawFillRect(&dev, 93, 281, 95, 284, 0xffff);
    sidebar_digit(102, 264, minute / 10, 0xffff);
    sidebar_digit(116, 264, minute % 10, 0xffff);

    lcdDrawFillRect(&dev, 136, 264, 235, 319, 0x0000);
    for (uint16_t i = 0; i < 32; i++) {
        float phase = (float)timestamp * 0.006f + i * 0.45f;
        int16_t y = 292 + (int16_t)(sinf(phase) * 18.0f);
        lcdDrawFillRect(&dev, 138 + i * 3, y, 140 + i * 3, y + 2, 0x07ff);
    }
}

static void poll_serial_keyboard(void) {
    uint8_t data[32];
    size_t count = 0;
    if (uart_get_buffered_data_len(UART_NUM_0, &count) != ESP_OK || count == 0)
        return;
    count = count > sizeof(data) ? sizeof(data) : count;
    int len = uart_read_bytes(UART_NUM_0, data, count, 0);
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (keyboard_escape) {
            if (keyboard_escape == 1 && c == '[') {
                keyboard_escape = 2;
                continue;
            }
            if (keyboard_escape == 2) {
                if (c == 'A') keyboard_press(BTN_IDX_UP);
                if (c == 'B') keyboard_press(BTN_IDX_DOWN);
                if (c == 'C') keyboard_press(BTN_IDX_RIGHT);
                if (c == 'D') keyboard_press(BTN_IDX_LEFT);
                keyboard_escape = 0;
                continue;
            }
            keyboard_escape = 0;
        }
        if (c == 0x1b) keyboard_escape = 1;
        else if (c == 'w' || c == 'W') keyboard_press(BTN_IDX_UP);
        else if (c == 's' || c == 'S') keyboard_press(BTN_IDX_DOWN);
        else if (c == 'a' || c == 'A') keyboard_press(BTN_IDX_LEFT);
        else if (c == 'd' || c == 'D') keyboard_press(BTN_IDX_RIGHT);
        else if (c == 'z' || c == 'Z' || c == '\n' || c == '\r') keyboard_press(BTN_IDX_A);
        else if (c == 'x' || c == 'X' || c == ' ') keyboard_press(BTN_IDX_B);
    }
}
bool init_audio() {
#ifdef PICO8_DISABLE_AUDIO
    return true;
#else

    i2s_driver_install(I2S_NUM_0, &i2s_config, 4, &i2s_event_queue);   //install and start i2s driver
    i2s_zero_dma_buffer(I2S_NUM_0);

    i2s_set_pin(I2S_NUM_0, &i2s_pin_config); //for internal DAC, this will enable both of the internal channels

    i2s_zero_dma_buffer(I2S_NUM_0);
    xTaskCreatePinnedToCore(i2sTask, "I2Sout", 4096, NULL, 1 /* prio */, NULL, 1 /* core id */);

    return true;
#endif
}

void draw_hud() {
    // The full logical framebuffer is scaled by put_buffer().
}

void show_splore_stand_in(void) {
    // The source image is 160x205. Scale it to 240x308, leaving the final
    // 12 rows black; this preserves the mockup's aspect ratio on the CYD.
    const uint8_t *pixels = _binary_splore565_bin_start;
    const uint16_t out_w = 240;
    const uint16_t out_h = 308;
    for (uint16_t y = 0; y < out_h; y++) {
        uint16_t sy = (uint32_t)y * 205 / out_h;
        for (uint16_t x = 0; x < out_w; x++) {
            uint16_t sx = (uint32_t)x * 160 / out_w;
            const uint8_t *src = pixels + ((uint32_t)sy * 160 + sx) * 2;
            scaled_line[x * 2] = src[0];
            scaled_line[x * 2 + 1] = src[1];
        }
        lcdSetWindowRect(&dev, 0, y + 6, 239, y + 6);
        send_buffer(&dev, scaled_line, sizeof(scaled_line));
    }
    lcdDrawFillRect(&dev, 0, 0, 239, 5, 0x0000);
    lcdDrawFillRect(&dev, 0, 314, 239, 319, 0x0000);
}

bool init_video() {
    spi_master_init(&dev, (gpio_num_t)CONFIG_MOSI_GPIO, (gpio_num_t)CONFIG_SCLK_GPIO, (gpio_num_t)CONFIG_CS_GPIO, (gpio_num_t)CONFIG_DC_GPIO, (gpio_num_t)CONFIG_RESET_GPIO, (gpio_num_t)CONFIG_BL_GPIO);
    lcdInit(&dev, CONFIG_TFT_CONTROLLER_ILI9341 ? 0x9341 : 0x7735,
            CONFIG_TFT_WIDTH, CONFIG_TFT_HEIGHT, 0, 0);

    // Keep a deterministic diagnostic strip visible while the CYD bring-up
    // screen is being developed. This also verifies address windows and RGB565
    // byte ordering independently of the Pico-8 renderer.
    lcdDrawFillRect(&dev, 0, 0, 239, 319, 0x0000);

    uint64_t button_mask = 0;
    add_button_gpio(&button_mask, CONFIG_GPIO_LEFT);
    add_button_gpio(&button_mask, CONFIG_GPIO_RIGHT);
    add_button_gpio(&button_mask, CONFIG_GPIO_A);
    add_button_gpio(&button_mask, CONFIG_GPIO_B);
    add_button_gpio(&button_mask, CONFIG_GPIO_UP);
    gpio_config_t c = {
        .pin_bit_mask = button_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (button_mask)
        gpio_config(&c);

    return true;
}

bool handle_input() {
    poll_serial_keyboard();
    int up = read_button_gpio(CONFIG_GPIO_UP);
    int left = read_button_gpio(CONFIG_GPIO_LEFT);
    int right = read_button_gpio(CONFIG_GPIO_RIGHT);
    int a = read_button_gpio(CONFIG_GPIO_A);
    int b = read_button_gpio(CONFIG_GPIO_B);
    // printf("left %d, right %d, a %d\n", left, right, a);

    buttons_prev[0] = buttons[0];
    buttons_prev[1] = buttons[1];
    buttons_prev[2] = buttons[2];
    buttons_prev[3] = buttons[3];
    buttons_prev[4] = buttons[4];
    buttons_prev[5] = buttons[5];

    buttons[BTN_IDX_LEFT] = left == 1;
    buttons[BTN_IDX_RIGHT] = right == 1;
    buttons[BTN_IDX_UP] = up == 1;
    buttons[BTN_IDX_DOWN] = 0; // FIXME no down connected
    buttons[BTN_IDX_A] = a == 1;
    buttons[BTN_IDX_B] = b == 1;

#if CONFIG_BLUETOOTH_GAMEPAD
    uint8_t bt = bluetooth_gamepad_buttons();
    if (bt) {
        buttons[BTN_IDX_LEFT] |= (bt & (1u << 0)) != 0;
        buttons[BTN_IDX_RIGHT] |= (bt & (1u << 1)) != 0;
        buttons[BTN_IDX_UP] |= (bt & (1u << 2)) != 0;
        buttons[BTN_IDX_DOWN] |= (bt & (1u << 3)) != 0;
        buttons[BTN_IDX_A] |= (bt & (1u << 4)) != 0;
        buttons[BTN_IDX_B] |= (bt & (1u << 5)) != 0;
    }
#endif
    for (uint8_t i = 0; i < 6; i++) {
        if (now() < keyboard_buttons_until[i])
            buttons[i] = 1;
        else
            keyboard_buttons &= ~(1u << i);
    }

    buttons_frame[0] = (buttons[0] == 1) && (buttons_prev[0] == 0);
    buttons_frame[1] = (buttons[1] == 1) && (buttons_prev[1] == 0);
    buttons_frame[2] = (buttons[2] == 1) && (buttons_prev[2] == 0);
    buttons_frame[3] = (0)               && (buttons_prev[3] == 0); // FIXME no down connected
    buttons_frame[4] = (buttons[4] == 1) && (buttons_prev[4] == 0);
    buttons_frame[5] = (buttons[5] == 1) && (buttons_prev[5] == 0);

    // TODO
    return false;
}

uint32_t now(){
    return (esp_timer_get_time()/1000) & 0xFFFFFFFF;
    //return (esp_timer_get_time()) & 0xFFFFFFFF;
}

void put_buffer(void *pvParameters)
{
    uint8_t buf[1];
    while (true) {
        xQueueReceive(q, &buf, portMAX_DELAY);

        // uint64_t frame_start_time = now();

        for (uint16_t y = 0; y < 256; y++) {
            uint16_t sy = y / 2;
            for (uint16_t x = 0; x < 240; x++) {
                uint16_t sx = 4 + x / 2;
                color_t c = palette[get_pixel(sx, sy)];
                scaled_line[x * 2] = c >> 8;
                scaled_line[x * 2 + 1] = c & 0xff;
            }
            lcdSetWindowRect(&dev, 0, y, 239, y);
            send_buffer(&dev, scaled_line, sizeof(scaled_line));
        }
        draw_sidebar();

        // uint64_t frame_end_time = now();
        // int delta = (frame_end_time - frame_start_time);

        //printf("Copying to SPI took: %d\n", delta);
    }
}

uint8_t current_hour() {
    // 0-24h
    time_t rawtime;
    struct tm* timeinfo;

    time(&rawtime);
    timeinfo = localtime ( &rawtime );
    return timeinfo->tm_hour;
}
uint8_t current_minute() {
    // 0-60m
    time_t rawtime;
    struct tm* timeinfo;

    time(&rawtime);
    timeinfo = localtime ( &rawtime );
    return timeinfo->tm_min;
}
uint8_t wifi_strength() {
    // arbitrary 0-3 scale (limited sprites)
    // 0 = off, 1=low, 2=med, 3 = high
    wifi_ap_record_t ap;
    esp_err_t result = esp_wifi_sta_get_ap_info(&ap);
    if (result == ESP_ERR_WIFI_NOT_CONNECT || result == ESP_ERR_WIFI_CONN) {
        printf("ded wifi\n");
        return 0;
    }
    printf("rssi %d\n", ap.rssi);
    if (ap.rssi == 0) return 0; // probably bug, too good signal
    // ap.rssi is -127..0
    if (ap.rssi > -10) return 3;
    if (ap.rssi > -30) return 2;
    return 1;
}
uint8_t battery_left() {
    // arbitrary 0-3 scale
    // 0 = almost empty, 3 = full
    return 3;
}
