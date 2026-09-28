// Small Bluetooth Classic HID host for generic gamepads.
//
// This deliberately uses Classic HID first.  It is the lowest-RAM path on the
// original ESP32 and covers the inexpensive "generic Bluetooth" pads commonly
// sold for phones and PCs.  Reports are printed once so unusual layouts can be
// mapped without guessing silently.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hidh.h"
#include "esp_hidh_bluedroid.h"
#include "esp_hid_common.h"

static volatile bool bt_gamepad_connected;
static volatile uint8_t bt_gamepad_buttons;
static bool bt_report_logged;
static esp_bd_addr_t bt_candidate;
static volatile bool bt_candidate_valid;

static void bt_hid_event(void *, esp_event_base_t, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;
    if (!p) return;

    if (event == ESP_HIDH_OPEN_EVENT) {
        bt_gamepad_connected = p->open.status == ESP_OK;
        printf("BT HID %s\n", bt_gamepad_connected ? "connected" : "open failed");
        if (bt_gamepad_connected)
            esp_hidh_dev_dump(p->open.dev, stdout);
        return;
    }
    if (event == ESP_HIDH_CLOSE_EVENT) {
        bt_gamepad_connected = false;
        bt_gamepad_buttons = 0;
        printf("BT HID disconnected\n");
        return;
    }
    if (event != ESP_HIDH_INPUT_EVENT || !p->input.data || !p->input.length)
        return;

    const uint8_t *d = p->input.data;
    const uint16_t n = p->input.length;
    if (!bt_report_logged) {
        printf("BT HID first input report (id=%u len=%u):", p->input.report_id, n);
        for (uint16_t i = 0; i < n && i < 32; ++i) printf(" %02x", d[i]);
        printf("\n");
        bt_report_logged = true;
    }

    // Most low-cost pads use: buttons, hat, X, Y.  Also accept the common
    // report-id-prefixed variant by trying the following byte positions.
    uint8_t buttons = d[0];
    uint8_t hat = n > 1 ? d[1] : 8;
    uint8_t x = n > 2 ? d[2] : 128;
    uint8_t y = n > 3 ? d[3] : 128;
    if (n >= 5 && d[0] == p->input.report_id) {
        buttons = d[1]; hat = d[2]; x = d[3]; y = d[4];
    }
    uint8_t mapped = 0;
    if (buttons & 0x01) mapped |= 1u << 4; // A
    if (buttons & 0x02) mapped |= 1u << 5; // B
    if (hat <= 7) {
        if (hat == 0 || hat == 1 || hat == 7) mapped |= 1u << 2; // up
        if (hat == 1 || hat == 2 || hat == 3) mapped |= 1u << 1; // right
        if (hat == 3 || hat == 4 || hat == 5) mapped |= 1u << 3; // down
        if (hat == 5 || hat == 6 || hat == 7) mapped |= 1u << 0; // left
    }
    if (x < 64) mapped |= 1u << 0;
    if (x > 192) mapped |= 1u << 1;
    if (y < 64) mapped |= 1u << 2;
    if (y > 192) mapped |= 1u << 3;
    bt_gamepad_buttons = mapped;
}

static void bt_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p)
{
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        // Inquiry results expose properties rather than a decoded COD in this
        // IDF version.  Retain the first result; the HID host will reject a
        // non-HID device during the open/SDP step.
        if (!bt_candidate_valid) {
            memcpy(bt_candidate, p->disc_res.bda, sizeof(bt_candidate));
            bt_candidate_valid = true;
        }
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT &&
               p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
               bt_candidate_valid && !bt_gamepad_connected) {
        printf("BT HID candidate found; connecting\n");
        esp_hidh_dev_open(bt_candidate, ESP_HID_TRANSPORT_BT, 0);
    } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
        esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
        esp_bt_gap_pin_reply(p->pin_req.bda, true, 4, pin);
    } else if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
    }
}

static void bt_scan_task(void *)
{
    while (true) {
        if (!bt_gamepad_connected) {
            bt_candidate_valid = false;
            bt_report_logged = false;
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(12000));
    }
}

extern "C" bool bluetooth_gamepad_init(void)
{
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    cfg.mode = ESP_BT_MODE_CLASSIC_BT;
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { printf("BT controller init: %s\n", esp_err_to_name(err)); return false; }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { printf("BT controller enable: %s\n", esp_err_to_name(err)); return false; }
    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { printf("Bluedroid init: %s\n", esp_err_to_name(err)); return false; }
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { printf("Bluedroid enable: %s\n", esp_err_to_name(err)); return false; }
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_gap_event));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
    esp_hidh_config_t hidh = { .callback = bt_hid_event, .event_stack_size = 4096, .callback_arg = NULL };
    ESP_ERROR_CHECK(esp_hidh_init(&hidh));
    xTaskCreate(bt_scan_task, "bt_scan", 4096, NULL, 2, NULL);
    printf("Bluetooth HID host ready; put the controller in pairing mode\n");
    return true;
}

extern "C" uint8_t bluetooth_gamepad_buttons(void)
{
    return bt_gamepad_buttons;
}
