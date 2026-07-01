/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_http_server.h"

#define WIFI_SSID "357"
#define WIFI_PASS "12345678"

#define UART1_PORT UART_NUM_1
#define UART1_TX_GPIO GPIO_NUM_1
#define UART1_RX_GPIO GPIO_NUM_2
#define UART1_BAUD_RATE 115200

#define UART_RX_BUF_SIZE 512
#define DATA_LINE_MAX_LEN 256
#define DATA_HISTORY_COUNT 10
#define BUTTON_VALUE_MAX_LEN 64

#define HTTP_PORT 80
#define CONFIG_RESPONSE_MAX_LEN 2048
#define DATA_RESPONSE_MAX_LEN (DATA_HISTORY_COUNT * (DATA_LINE_MAX_LEN + 2))


static const char *TAG = "wifi_serial";
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *sta_netif = NULL;
static httpd_handle_t http_server = NULL;

static SemaphoreHandle_t data_mutex;
static char uart1_history[DATA_HISTORY_COUNT][DATA_LINE_MAX_LEN];
static size_t uart1_history_index;
static bool uart1_history_full;

static SemaphoreHandle_t config_mutex;
static char button_values[8][BUTTON_VALUE_MAX_LEN];

static const int CONNECTED_BIT = BIT0;

static void uart0_print(const char *text)
{
    if (text) {
        printf("%s", text);
    }
}

static size_t url_encode(const char *src, char *dst, size_t dst_len)
{
    size_t out = 0;

    if (dst_len == 0) {
        return 0;
    }

    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        bool unreserved = (isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~';

        if (unreserved) {
            if (out + 1 >= dst_len) {
                break;
            }
            dst[out++] = (char)c;
        } else {
            if (out + 3 >= dst_len) {
                break;
            }
            snprintf(dst + out, 4, "%%%02X", c);
            out += 3;
        }
    }

    dst[out] = '\0';
    return out;
}

static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t out = 0;

    if (dst_len == 0) {
        return;
    }

    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_len; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[out++] = (char)strtoul(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[out++] = ' ';
        } else {
            dst[out++] = src[i];
        }
    }

    dst[out] = '\0';
}

static void uart1_send_value(int index, bool with_newline)
{
    char value[BUTTON_VALUE_MAX_LEN];
    size_t value_len = 0;

    if (index < 0 || index >= 8) {
        return;
    }

    xSemaphoreTake(config_mutex, portMAX_DELAY);
    strlcpy(value, button_values[index], sizeof(value));
    xSemaphoreGive(config_mutex);

    value_len = strlen(value);
    if (value_len > 0) {
        uart_write_bytes(UART1_PORT, value, value_len);
        if (with_newline) {
            uart_write_bytes(UART1_PORT, "\r\n", 2);
        }
    }
}

static void store_uart1_history(const char *data)
{
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    strlcpy(uart1_history[uart1_history_index], data, DATA_LINE_MAX_LEN);
    uart1_history_index = (uart1_history_index + 1) % DATA_HISTORY_COUNT;
    if (uart1_history_index == 0) {
        uart1_history_full = true;
    }
    xSemaphoreGive(data_mutex);
}

static void uart1_rx_task(void *pvParameters)
{
    uint8_t buf[UART_RX_BUF_SIZE];

    while (1) {
        int len = uart_read_bytes(UART1_PORT, buf, sizeof(buf) - 1, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            buf[len] = '\0';
            store_uart1_history((const char *)buf);
            printf("\"%s\"\r\n", (const char *)buf);
        }
    }
}

static esp_err_t data_get_handler(httpd_req_t *req)
{
    char response[DATA_RESPONSE_MAX_LEN];
    size_t used = 0;

    response[0] = '\0';
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    size_t count = uart1_history_full ? DATA_HISTORY_COUNT : uart1_history_index;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (uart1_history_index + DATA_HISTORY_COUNT - 1 - i) % DATA_HISTORY_COUNT;
        int written = snprintf(response + used, sizeof(response) - used, "%s%s",
                               uart1_history[idx], (i + 1 < count) ? "\n" : "");
        if (written < 0 || (size_t)written >= sizeof(response) - used) {
            break;
        }
        used += (size_t)written;
    }
    xSemaphoreGive(data_mutex);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char response[CONFIG_RESPONSE_MAX_LEN];
    char encoded[BUTTON_VALUE_MAX_LEN * 3 + 1];
    size_t used = 0;

    response[0] = '\0';
    xSemaphoreTake(config_mutex, portMAX_DELAY);
    for (int i = 0; i < 8; i++) {
        url_encode(button_values[i], encoded, sizeof(encoded));
        int written = snprintf(response + used, sizeof(response) - used,
                               "btn%d=%s%s", i + 1, encoded, (i < 7) ? "&" : "");
        if (written < 0 || (size_t)written >= sizeof(response) - used) {
            break;
        }
        used += (size_t)written;
    }
    xSemaphoreGive(config_mutex);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char *buf = NULL;
    int received = 0;
    char index_str[8];
    char value_raw[BUTTON_VALUE_MAX_LEN * 3 + 1];
    char value_decoded[BUTTON_VALUE_MAX_LEN];

    if (req->content_len <= 0 || req->content_len > 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
    }

    received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
    }

    if (httpd_query_key_value(buf, "index", index_str, sizeof(index_str)) != ESP_OK ||
        httpd_query_key_value(buf, "value", value_raw, sizeof(value_raw)) != ESP_OK) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    }

    free(buf);
    url_decode(value_raw, value_decoded, sizeof(value_decoded));

    int index = atoi(index_str);
    if (index < 1 || index > 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index out of range");
    }

    xSemaphoreTake(config_mutex, portMAX_DELAY);
    strlcpy(button_values[index - 1], value_decoded, BUTTON_VALUE_MAX_LEN);
    xSemaphoreGive(config_mutex);

    nvs_handle_t nvs_handle;
    if (nvs_open("btncfg", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), "btn%d", index);
        nvs_set_str(nvs_handle, key, button_values[index - 1]);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_get_handler(httpd_req_t *req)
{
    char query[64];
    char index_str[8];
    char mode_str[8];
    bool with_newline = false;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
    }

    if (httpd_query_key_value(query, "index", index_str, sizeof(index_str)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing index");
    }

    if (httpd_query_key_value(query, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
        if (strcmp(mode_str, "long") == 0) {
            with_newline = true;
        }
    }

    int index = atoi(index_str);
    if (index < 1 || index > 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index out of range");
    }

    uart1_send_value(index - 1, with_newline);
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32S3 UART</title>"
        "<style>"
        "body{font-family:Arial,Helvetica,sans-serif;background:#f4f6f8;margin:0;padding:16px;}"
        ".card{background:#fff;border-radius:12px;padding:16px;box-shadow:0 6px 16px rgba(0,0,0,0.08);}"
        "h1{font-size:20px;margin:0 0 12px 0;}"
        ".row{display:flex;flex-wrap:wrap;gap:8px;margin:12px 0;}"
        ".btn{flex:1 1 22%;padding:12px;border:none;border-radius:8px;background:#2b6cb0;color:#fff;font-size:16px;}"
        ".btn:active{background:#1a4c80;}"
        "#rx{width:100%;min-height:80px;font-size:16px;padding:8px;}"
        ".cfg{display:grid;grid-template-columns:60px 1fr 70px;gap:8px;align-items:center;margin-top:8px;}"
        ".cfg input{padding:6px;font-size:14px;}"
        ".save{padding:6px;border:none;border-radius:6px;background:#38a169;color:#fff;}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"card\">"
        "<h1>UART1 Monitor</h1>"
        "<textarea id=\"rx\" readonly></textarea>"
        "<div class=\"row\" id=\"buttons\"></div>"
        "<h1>Button Config</h1>"
        "<div id=\"config\"></div>"
        "</div>"
        "<script>"
        "const rxBox=document.getElementById('rx');"
        "const btnRow=document.getElementById('buttons');"
        "const cfgBox=document.getElementById('config');"
        "let cfg={};"
        "function send(index,mode){fetch(`/send?index=${index}&mode=${mode}`);}"
        "function buildButtons(){btnRow.innerHTML='';for(let i=1;i<=8;i++){"
        "const b=document.createElement('button');b.className='btn';b.textContent='BTN '+i;"
        "let timer=null;"
        "b.addEventListener('click',()=>send(i,'short'));"
        "b.addEventListener('mousedown',()=>{timer=setInterval(()=>send(i,'long'),100);});"
        "b.addEventListener('mouseup',()=>{if(timer){clearInterval(timer);timer=null;}});"
        "b.addEventListener('mouseleave',()=>{if(timer){clearInterval(timer);timer=null;}});"
        "b.addEventListener('touchstart',(e)=>{e.preventDefault();timer=setInterval(()=>send(i,'long'),100);},{passive:false});"
        "b.addEventListener('touchend',()=>{if(timer){clearInterval(timer);timer=null;}});"
        "btnRow.appendChild(b);}"
        "}"
        "function buildConfig(){cfgBox.innerHTML='';for(let i=1;i<=8;i++){"
        "const row=document.createElement('div');row.className='cfg';"
        "const label=document.createElement('div');label.textContent='BTN '+i;"
        "const input=document.createElement('input');input.value=cfg['btn'+i]||'';"
        "const save=document.createElement('button');save.className='save';save.textContent='Save';"
        "save.addEventListener('click',()=>{fetch('/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:`index=${i}&value=${encodeURIComponent(input.value)}`}).then(loadConfig);});"
        "row.appendChild(label);row.appendChild(input);row.appendChild(save);cfgBox.appendChild(row);}"
        "}"
        "function loadConfig(){fetch('/config').then(r=>r.text()).then(t=>{"
        "const params=new URLSearchParams(t);cfg={};for(let i=1;i<=8;i++){cfg['btn'+i]=params.get('btn'+i)||'';}"
        "buildConfig();});}"
        "function pollData(){fetch('/data').then(r=>r.text()).then(t=>{rxBox.value=t;});}"
        "buildButtons();loadConfig();setInterval(pollData,300);"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.stack_size = 8192;

    if (httpd_start(&http_server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t data = {
            .uri = "/data",
            .method = HTTP_GET,
            .handler = data_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t config_get = {
            .uri = "/config",
            .method = HTTP_GET,
            .handler = config_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t config_post = {
            .uri = "/config",
            .method = HTTP_POST,
            .handler = config_post_handler,
            .user_ctx = NULL
        };
        httpd_uri_t send = {
            .uri = "/send",
            .method = HTTP_GET,
            .handler = send_get_handler,
            .user_ctx = NULL
        };

        httpd_register_uri_handler(http_server, &root);
        httpd_register_uri_handler(http_server, &data);
        httpd_register_uri_handler(http_server, &config_get);
        httpd_register_uri_handler(http_server, &config_post);
        httpd_register_uri_handler(http_server, &send);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        esp_netif_ip_info_t ip;

        if (esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
            char line[128];
            snprintf(line, sizeof(line), "WiFi connected SSID=%s IP=" IPSTR "\r\n", WIFI_SSID, IP2STR(&ip.ip));
            uart0_print(line);
            snprintf(line, sizeof(line), "http://" IPSTR ":%d\r\n", IP2STR(&ip.ip), HTTP_PORT);
            uart0_print(line);
        }

        if (http_server == NULL) {
            start_http_server();
        }
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_LOGI(TAG, "Connecting to SSID %s", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void uart1_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART1_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(uart_driver_install(UART1_PORT, UART_RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART1_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART1_PORT, UART1_TX_GPIO, UART1_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void load_button_config(void)
{
    nvs_handle_t nvs_handle;
    char key[8];
    char default_value[2];

    xSemaphoreTake(config_mutex, portMAX_DELAY);
    for (int i = 0; i < 8; i++) {
        snprintf(default_value, sizeof(default_value), "%d", i + 1);
        strlcpy(button_values[i], default_value, BUTTON_VALUE_MAX_LEN);
    }
    xSemaphoreGive(config_mutex);

    if (nvs_open("btncfg", NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        size_t len = BUTTON_VALUE_MAX_LEN;
        snprintf(key, sizeof(key), "btn%d", i + 1);
        if (nvs_get_str(nvs_handle, key, NULL, &len) == ESP_OK && len > 0 && len <= BUTTON_VALUE_MAX_LEN) {
            char temp[BUTTON_VALUE_MAX_LEN];
            if (nvs_get_str(nvs_handle, key, temp, &len) == ESP_OK) {
                xSemaphoreTake(config_mutex, portMAX_DELAY);
                strlcpy(button_values[i], temp, BUTTON_VALUE_MAX_LEN);
                xSemaphoreGive(config_mutex);
            }
        }
    }
    nvs_close(nvs_handle);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    data_mutex = xSemaphoreCreateMutex();
    config_mutex = xSemaphoreCreateMutex();
    if (data_mutex == NULL || config_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    for (size_t i = 0; i < DATA_HISTORY_COUNT; i++) {
        uart1_history[i][0] = '\0';
    }
    uart1_history_index = 0;
    uart1_history_full = false;
    load_button_config();
    uart1_init();

    xTaskCreate(uart1_rx_task, "uart1_rx_task", 4096, NULL, 5, NULL);
    wifi_init_sta();
}
