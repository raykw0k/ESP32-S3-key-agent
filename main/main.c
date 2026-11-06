// Based on Espressif's CC0/Unlicense tusb_hid example.
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "sequence.h"
#include "config_json.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

static const char *TAG = "keyboard123";
static const uint8_t report_descriptor[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const char *strings[] = {
    (char[]){0x09, 0x04}, "ESP32-S3", "ESP32 Wi-Fi Keyboard",
    "1020BA71A158", "Keyboard"
};
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(report_descriptor), 0x81, 16, 10),
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return report_descriptor;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id,
    hid_report_type_t type, uint8_t *buffer, uint16_t len)
{
    (void)instance; (void)id; (void)type; (void)buffer; (void)len;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id,
    hid_report_type_t type, const uint8_t *buffer, uint16_t len)
{
    (void)instance; (void)id; (void)type; (void)buffer; (void)len;
}

#define AP_SSID "key-agent"
#define AP_PASSWORD "71581234"
static scheduler_t scheduler;
static SemaphoreHandle_t state_lock;
static bool updating;
static nvs_handle_t storage;
extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

static bool send_key(void *context, uint8_t mods, uint8_t key)
{
    (void)context;
    uint8_t keys[6] = {key};
    return tud_hid_keyboard_report(0, mods, keys);
}
static esp_err_t json_response(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t api_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    // Messages originate in the firmware, never unescaped request contents.
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    return json_response(req, body);
}
static bool valid_request(httpd_req_t *req)
{
    char header[64];
    if (httpd_req_get_hdr_value_str(req, "X-Keyboard-Request", header, sizeof(header)) != ESP_OK ||
        strcmp(header, "1")) {
        api_error(req, "403 Forbidden", "Use the configuration page on this device"); return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", header, sizeof(header)) != ESP_OK ||
        strncmp(header, "application/json", 16)) {
        api_error(req, "415 Unsupported Media Type", "A JSON request is required"); return false;
    }
    return true;
}
static char *read_body(httpd_req_t *req)
{
    if (req->content_len < 1 || req->content_len > MAX_CONFIG_JSON) {
        api_error(req, "400 Bad Request", "Invalid request length"); return NULL;
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) { api_error(req, "503 Service Unavailable", "Out of memory; try again later"); return NULL; }
    size_t done = 0;
    while (done < req->content_len) {
        int n = httpd_req_recv(req, body + done, req->content_len - done);
        if (n <= 0) {
            free(body); api_error(req, "408 Request Timeout", "Request read timed out"); return NULL;
        }
        done += n;
    }
    if (memchr(body, '\0', done)) {
        free(body); api_error(req, "400 Bad Request", "JSON contains invalid characters"); return NULL;
    }
    return body;
}
static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}
static int logic_id(httpd_req_t *req)
{
    char query[32], value[8];
    if (httpd_req_get_url_query_len(req) == 0) return 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", value, sizeof(value)) != ESP_OK ||
        strlen(value) != 1 || value[0] < '0' || value[0] > '3') return -1;
    return value[0] - '0';
}
static esp_err_t status_handler(httpd_req_t *req)
{
    char body[1024];
    xSemaphoreTake(state_lock, portMAX_DELAY);
    int n = snprintf(body, sizeof(body), "{\"usb\":%s,\"updating\":%s,\"logics\":[",
        scheduler.engines[0].connected ? "true" : "false", updating ? "true" : "false");
    for (unsigned i = 0; i < LOGIC_COUNT; i++) {
        engine_t *e = &scheduler.engines[i];
        n += snprintf(body+n, sizeof(body)-n,
            "%s{\"enabled\":%s,\"running\":%s,\"step\":%u,\"completed\":%lu,\"loops\":%lu}",
            i ? "," : "", e->config.enabled ? "true" : "false", e->running ? "true" : "false", e->index+1,
            (unsigned long)e->completed, (unsigned long)e->config.loops);
    }
    snprintf(body+n, sizeof(body)-n, "]}");
    xSemaphoreGive(state_lock);
    return json_response(req, body);
}
static esp_err_t config_handler(httpd_req_t *req)
{
    int id = logic_id(req);
    if (id < 0) return api_error(req, "400 Bad Request", "Invalid sequence ID");
    engine_t *selected = &scheduler.engines[id];
    char storage_key[24];
    snprintf(storage_key, sizeof(storage_key), "logic_%d", id);
    if (req->method == HTTP_GET) {
        sequence_t snapshot;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        snapshot = selected->config;
        xSemaphoreGive(state_lock);
        char *text = config_encode(&snapshot);
        if (!text) return api_error(req, "503 Service Unavailable", "Out of memory");
        esp_err_t result = json_response(req, text);
        free(text);
        return result;
    }
    if (!valid_request(req)) return ESP_OK;
    char *body = read_body(req);
    if (!body) return ESP_OK;
    sequence_t next;
    char error[160];
    bool valid = config_parse(body, &next, error, sizeof(error));
    free(body);
    if (!valid) return api_error(req, "400 Bad Request", error);
    char *canonical = config_encode(&next);
    if (!canonical) return api_error(req, "503 Service Unavailable", "Out of memory");
    xSemaphoreTake(state_lock, portMAX_DELAY);
    updating = true;
    engine_stop(selected);
    xSemaphoreGive(state_lock);

    // The keyboard task owns USB transmission. Wait for release before saving.
    bool released = false;
    for (unsigned i = 0; i < 200; i++) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        released = !selected->connected || (!selected->held && !selected->release_pending);
        xSemaphoreGive(state_lock);
        if (released) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    esp_err_t err = released ? nvs_set_str(storage, storage_key, canonical) : ESP_ERR_TIMEOUT;
    if (err == ESP_OK) err = nvs_commit(storage);
    free(canonical);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        selected->config = next;
        selected->index = 0;
        selected->completed = 0;
    }
    updating = false;
    xSemaphoreGive(state_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        return api_error(req, "503 Service Unavailable", "Save failed; sequence paused. Try again");
    }
    ESP_LOGI(TAG, "Saved %u actions; paused", next.count);
    return json_response(req, "{\"ok\":true}");
}
static esp_err_t control_handler(httpd_req_t *req)
{
    if (!valid_request(req)) return ESP_OK;
    char *body = read_body(req);
    if (!body) return ESP_OK;
    free(body);
    int id = logic_id(req);
    if (id < 0) return api_error(req, "400 Bad Request", "Invalid sequence ID");
    bool start = !strncmp(req->uri, "/api/start", 10);
    bool start_all = !strncmp(req->uri, "/api/start-all", 14);
    bool all = !strncmp(req->uri, "/api/stop-all", 13);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool busy = updating;
    bool ok = !busy;
    if (!busy) {
        if (all) scheduler_stop_all(&scheduler);
        else if (start_all) ok = scheduler_start_all(&scheduler);
        else if (start) ok = engine_start(&scheduler.engines[id]);
        else engine_stop(&scheduler.engines[id]);
    }
    xSemaphoreGive(state_lock);
    if (busy) return api_error(req, "409 Conflict", "Saving; try again shortly");
    if (!ok) return api_error(req, "409 Conflict", "Enable and save the sequence, connect USB/OTG, and wake the computer");
    ESP_LOGI(TAG, "%s via web", start ? "RUNNING" : "PAUSED");
    return status_handler(req);
}
static void start_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    assert(ap);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi = {.ap = {
        .ssid = AP_SSID, .ssid_len = sizeof(AP_SSID) - 1,
        .password = AP_PASSWORD, .channel = 6, .authmode = WIFI_AUTH_WPA2_PSK,
        .max_connection = 4, .pmf_cfg = {.required = false},
    }};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_handler},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler},
        {.uri = "/api/start-all", .method = HTTP_POST, .handler = control_handler},
        {.uri = "/api/start", .method = HTTP_POST, .handler = control_handler},
        {.uri = "/api/stop-all", .method = HTTP_POST, .handler = control_handler},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = control_handler},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    ESP_LOGI(TAG, "Hotspot %s; http://192.168.4.1", AP_SSID);
}
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("keypad", NVS_READWRITE, &storage));
    sequence_t configs[LOGIC_COUNT];
    for (unsigned i = 0; i < LOGIC_COUNT; i++) {
        sequence_defaults(&configs[i]);
        char key[16];
        snprintf(key, sizeof(key), "logic_%u", i);
        size_t length = 0;
        esp_err_t found = nvs_get_str(storage, key, NULL, &length);
        // Preserve the previous firmware's sequence as logic 1.
        if (i == 0 && found == ESP_ERR_NVS_NOT_FOUND) {
            strcpy(key, "sequence_v1");
            found = nvs_get_str(storage, key, NULL, &length);
        }
        if (found == ESP_OK && length <= MAX_CONFIG_JSON + 1) {
            char *text = malloc(length), error[160];
            if (text) {
                if (nvs_get_str(storage, key, text, &length) == ESP_OK)
                    config_parse(text, &configs[i], error, sizeof(error));
                free(text);
            }
        }
    }
    scheduler_init(&scheduler, configs);
    state_lock = xSemaphoreCreateMutex();
    assert(state_lock);
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
    usb.descriptor.full_speed_config = configuration;
    usb.descriptor.string = strings;
    usb.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb));
    start_network();
    bool armed = false;
    int previous = gpio_get_level(GPIO_NUM_0), stable = previous;
    uint32_t changed = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "READY: paused; BOOT or web toggles sequence");
    for (;;) {
        uint32_t now = esp_timer_get_time() / 1000;
        bool connected = tud_mounted() && !tud_suspended();
        int raw = gpio_get_level(GPIO_NUM_0);
        if (raw != previous) { previous = raw; changed = now; }
        xSemaphoreTake(state_lock, portMAX_DELAY);
        scheduler_tick(&scheduler, now, connected, tud_hid_ready(), send_key, NULL);
        if ((uint32_t)(now - changed) >= 30) {
            if (raw == 1) armed = true;
            if (stable != raw) {
                stable = raw;
                if (stable == 0 && armed) {
                    armed = false;
                    if (!updating) {
                        if (scheduler_running(&scheduler)) scheduler_stop_all(&scheduler);
                        else scheduler_start_all(&scheduler);
                        ESP_LOGI(TAG, "%s via BOOT", scheduler_running(&scheduler) ? "RUNNING" : "PAUSED");
                    }
                }
            }
        }
        xSemaphoreGive(state_lock);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
