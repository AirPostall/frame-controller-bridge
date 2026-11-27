#include "bridge.h"
#include "ble_host.h"
#include "boot_button.h"
#include "pairing.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const char page[] asm("_binary_index_html_start");
extern const char styles[] asm("_binary_style_css_start");
extern const char script[] asm("_binary_ui_js_start");

extern const char touch_script[] asm("_binary_touch_js_start");

extern const char backup_script[] asm("_binary_backup_js_start");

static TaskHandle_t setup_task;
static httpd_handle_t server;
static bool initialized;
// Handlers run serially in the HTTP server task. Share a buffer for profile requests.
static pad_profile transfer_profile;
static const char *TAG = "bridge-setup";

static esp_err_t asset(httpd_req_t *req)
{
    const char *data = req->user_ctx;
    const char *type = data == page ? "text/html; charset=utf-8" :
                       data == styles ? "text/css" : "text/javascript";
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, type);
    return httpd_resp_sendstr(req, data);
}

static esp_err_t snapshot(httpd_req_t *req)
{
    bridge_status s;
    bridge_get_status(&s);
    static const char digits[] = "0123456789abcdef";
    char raw[HAND_COUNT][RAW_CHANNELS * 2 + 1] = {0}, body[896];
    const motion_sample *motion = &s.output.motion;
    int64_t age_ms = s.motion_time_us ? (esp_timer_get_time() - s.motion_time_us) / 1000 : -1;
    const char *route = s.bluetooth_mode ? (ble_host_motion_ready() ? "bluetooth" : "none") :
                        (s.usb && pairing.usb_layout == USB_LAYOUT_PRO3 ? "usb" : "none");
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        for (unsigned i = 0; i < RAW_CHANNELS; ++i) {
            uint8_t b = s.hands[h].raw[i];
            raw[h][2*i] = digits[b >> 4];
            raw[h][2*i+1] = digits[b & 15];
        }
    }
    int length = snprintf(body, sizeof(body),
        "{\"raw\":[\"%s\",\"%s\"],\"seen\":[%u,%u],\"epoch\":[%lu,%lu],\"active\":[%u,%u],\"usb\":%u,\"bluetoothMode\":%u,\"bluetooth\":%u,\"usbLayout\":%u,\"standardUsb\":%u,\"up\":[%u,%u],\"masks\":[%u,%u],\"axes\":[[%d,%d],[%d,%d]],"
        "\"travel\":[%u,%u],\"packets\":[%lu,%lu],\"buttons\":%u,\"hat\":%u,"
        "\"triggers\":[%u,%u],\"bad\":%lu,\"overflows\":%lu,\"bluetoothOverflows\":%lu,"
        "\"motion\":{\"hand\":%u,\"samples\":%lu,\"zeroCount\":%lu,\"ageMs\":%lld,\"valid\":%s,\"route\":\"%s\","
        "\"accel\":[%ld,%ld,%ld],\"gyro\":[%ld,%ld,%ld]}}",
        raw[0], raw[1], s.hands[0].seen, s.hands[1].seen,
        (unsigned long)s.epoch[0], (unsigned long)s.epoch[1], s.hands[0].active, s.hands[1].active,
        s.usb, s.bluetooth_mode, (unsigned)ble_host_state(), pairing.usb_layout, pairing.usb_layout == USB_LAYOUT_STANDARD,
        s.connected[0], s.connected[1], s.hands[0].buttons, s.hands[1].buttons,
        s.hands[0].x, s.hands[0].y, s.hands[1].x, s.hands[1].y,
        s.hands[0].pressed_travel, s.hands[1].pressed_travel,
        (unsigned long)s.packets[0], (unsigned long)s.packets[1],
        pad_usb_buttons(&s.output), s.output.bytes[2],
        s.output.trigger_level[0], s.output.trigger_level[1],
        (unsigned long)s.bad, (unsigned long)s.overflows, (unsigned long)s.ble_overflows,
        s.motion_hand, (unsigned long)s.motion_samples, (unsigned long)s.gyro_zero_count, (long long)age_ms,
        s.output.motion_valid ? "true" : "false", route,
        (long)motion->accel[0], (long)motion->accel[1], (long)motion->accel[2],
        (long)motion->gyro[0], (long)motion->gyro[1], (long)motion->gyro[2]);
    if (length < 0 || (size_t)length >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "State exceeds buffer");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, length);
}

static esp_err_t profile_get(httpd_req_t *req)
{
    bridge_get_profile(&transfer_profile);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)&transfer_profile, sizeof(transfer_profile));
}

static esp_err_t profile_defaults(httpd_req_t *req)
{
    pad_defaults(&transfer_profile);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)&transfer_profile, sizeof(transfer_profile));
}

static esp_err_t profile_input(httpd_req_t *req)
{
    pad_profile *p = &transfer_profile;
    if (req->content_len != sizeof(*p))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected profile length");
    for (size_t used = 0; used < sizeof(*p);) {
        int n = httpd_req_recv(req, (char *)p + used, sizeof(*p) - used);
        if (n <= 0) return ESP_FAIL;
        used += n;
    }
    if (!pad_profile_valid(p))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid profile");
    if (req->method == HTTP_POST) return httpd_resp_sendstr(req, "Valid");
    if (bridge_save(p) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
    return httpd_resp_sendstr(req, "Saved");
}

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    esp_err_t rc = httpd_start(&server, &config);
    if (rc != ESP_OK) return rc;
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=asset, .user_ctx=(void *)page},
        {.uri="/style.css", .method=HTTP_GET, .handler=asset, .user_ctx=(void *)styles},
        {.uri="/backup.js", .method=HTTP_GET, .handler=asset, .user_ctx=(void *)backup_script},
        {.uri="/touch.js", .method=HTTP_GET, .handler=asset, .user_ctx=(void *)touch_script},
        {.uri="/ui.js", .method=HTTP_GET, .handler=asset, .user_ctx=(void *)script},
        {.uri="/api/state", .method=HTTP_GET, .handler=snapshot},
        {.uri="/api/defaults", .method=HTTP_GET, .handler=profile_defaults},
        {.uri="/api/profile", .method=HTTP_GET, .handler=profile_get},
        {.uri="/api/profile", .method=HTTP_POST, .handler=profile_input},
        {.uri="/api/profile", .method=HTTP_PUT, .handler=profile_input},
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        rc = httpd_register_uri_handler(server, &routes[i]);
        if (rc != ESP_OK) {
            httpd_stop(server);
            server = NULL;
            return rc;
        }
    }
    return ESP_OK;
}

static esp_err_t start_setup(void)
{
    if (!initialized) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        if (!esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;
        initialized = true;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t rc = esp_wifi_init(&init);
    if (rc != ESP_OK) return rc;
    wifi_config_t config = {.ap = {.password = "frame-setup", .channel = 1,
                                    .max_connection = 1, .authmode = WIFI_AUTH_WPA2_PSK}};
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BASE));
    snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), "FrameBridge-%02X%02X", mac[4], mac[5]);
    rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (rc == ESP_OK) rc = esp_wifi_set_mode(WIFI_MODE_AP);
    if (rc == ESP_OK) rc = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (rc == ESP_OK) rc = esp_wifi_start();
    if (rc == ESP_OK) rc = start_server();
    if (rc != ESP_OK) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    return rc;
}

static void switch_setup(void *arg)
{
    // Ignore BOOT if it was held during reset, until the first release.
    bool down = gpio_get_level(GPIO_NUM_0) == 0;
    boot_button button = {.down=down, .raw_down=down, .hold_sent=down};
    int64_t wifi_deadline = INT64_MAX;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        boot_action action = boot_button_sample(&button, gpio_get_level(GPIO_NUM_0) == 0, now);
        if (action == BOOT_HOLD) ble_host_toggle_mode();
        if (server && (action == BOOT_TAP || now >= wifi_deadline)) {
            httpd_stop(server);
            server = NULL;
            esp_wifi_stop();
            esp_wifi_deinit();
            wifi_deadline = INT64_MAX;
        } else if (action == BOOT_TAP) {
            esp_err_t rc = start_setup();
            if (rc == ESP_OK) wifi_deadline = esp_timer_get_time() / 1000 + SETUP_WIFI_MS;
            else ESP_LOGE(TAG, "Setup network unavailable: %s", esp_err_to_name(rc));
        }
        int64_t deadline = boot_next_wake(&button, wifi_deadline);
        TickType_t wait = portMAX_DELAY;
        if (deadline != INT64_MAX) {
            int64_t remaining = deadline - esp_timer_get_time() / 1000;
            wait = remaining > 0 ? pdMS_TO_TICKS(remaining) + 1 : 0;
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

static void IRAM_ATTR boot_edge(void *arg)
{
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(setup_task, &wake);
    if (wake) portYIELD_FROM_ISR();
}

void setup_init(void)
{
    gpio_config_t button = {.pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_ANYEDGE};
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_ERROR_CHECK(xTaskCreate(switch_setup, "setup-switch", 4096, NULL, 3, &setup_task)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_NUM_0, boot_edge, NULL));
    xTaskNotifyGive(setup_task);
}
