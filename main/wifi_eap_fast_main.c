/*
 * ESP32-S3 WiFi WebSocket Serial Bridge
 * UART0: Console log + remote IP configuration
 * UART1: TX=39, RX=38, baud=9600, 8N1 — data channel
 * WiFi: 0986 / 12345678
 * WebSocket client → FastAPI server  (tcp_transport)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "nmea_parser.h"
#include "ak09911c.h"

/* ---------- WiFi ---------- */
#define WIFI_SSID "0986"
#define WIFI_PASS "12345678"

/* ---------- UART0 : console ---------- */
#define UART0_PORT          UART_NUM_0
#define UART0_TX_GPIO       GPIO_NUM_43
#define UART0_RX_GPIO       GPIO_NUM_44
#define UART0_BAUD_RATE     115200

/* ---------- UART1 : data ---------- */
#define UART1_PORT          UART_NUM_1
#define UART1_TX_GPIO       GPIO_NUM_39
#define UART1_RX_GPIO       GPIO_NUM_38
#define UART1_BAUD_RATE     9600

#define UART_RX_BUF_SIZE    512
#define UART_TX_BUF_SIZE    256

/* ---------- WebSocket server ---------- */
#define WS_SERVER_IP_DEFAULT    "8.148.208.229"
#define WS_SERVER_PORT_DEFAULT  8010
#define WS_SERVER_PATH          "/ws/esp32"

static const char *TAG = "wifi_serial";

static EventGroupHandle_t wifi_event_group;
static esp_netif_t *sta_netif = NULL;
static const int CONNECTED_BIT = BIT0;

static SemaphoreHandle_t ws_cfg_mutex;
static SemaphoreHandle_t ws_write_mutex;  /* serialise ws_send across tasks */
static char ws_server_ip[16];
static uint16_t ws_server_port;

static esp_transport_handle_t ws_transport = NULL;
static volatile bool ws_connected = false;
static bool tasks_started = false;



/*============================================================
 *  WebSocket server address helpers (thread‑safe)
 *============================================================*/
static void ws_cfg_set(const char *ip, uint16_t port)
{
    xSemaphoreTake(ws_cfg_mutex, portMAX_DELAY);
    strlcpy(ws_server_ip, ip, sizeof(ws_server_ip));
    ws_server_port = port;
    xSemaphoreGive(ws_cfg_mutex);
}

static void ws_cfg_get(char *ip, size_t len, uint16_t *port)
{
    xSemaphoreTake(ws_cfg_mutex, portMAX_DELAY);
    strlcpy(ip, ws_server_ip, len);
    *port = ws_server_port;
    xSemaphoreGive(ws_cfg_mutex);
}

static void ws_cfg_print(void)
{
    char ip[16];
    uint16_t port;
    ws_cfg_get(ip, sizeof(ip), &port);
    ESP_LOGI(TAG, "WS server = %s:%u", ip, port);
}

/*============================================================
 *  WebSocket start / stop  (using tcp_transport)
 *============================================================*/
static void ws_start(void)
{
    if (ws_transport) return;

    char ip[16];
    uint16_t port;
    ws_cfg_get(ip, sizeof(ip), &port);

    /* 1. create TCP transport */
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (!tcp) {
        ESP_LOGE(TAG, "esp_transport_tcp_init failed");
        return;
    }

    /* 2. wrap with WebSocket */
    ws_transport = esp_transport_ws_init(tcp);
    if (!ws_transport) {
        ESP_LOGE(TAG, "esp_transport_ws_init failed");
        esp_transport_destroy(tcp);
        return;
    }
    esp_transport_ws_set_path(ws_transport, WS_SERVER_PATH);

    /* 3. connect (TCP + WS upgrade handshake) */
    int ret = esp_transport_connect(ws_transport, ip, (int)port, 10000);
    if (ret != 0) {
        ESP_LOGE(TAG, "WS connect failed (err=%d)", ret);
        esp_transport_destroy(ws_transport);
        ws_transport = NULL;
        return;
    }

    ws_connected = true;
    ESP_LOGI(TAG, "WS connected to %s:%u%s", ip, port, WS_SERVER_PATH);
}

static void ws_stop(void)
{
    if (!ws_transport) return;

    ws_connected = false;
    esp_transport_destroy(ws_transport);
    ws_transport = NULL;
    ESP_LOGI(TAG, "WS stopped");
}

/*============================================================
 *  Send text frame (best‑effort)
 *============================================================*/
static int ws_send(const char *data, size_t len)
{
    if (!ws_transport || !ws_connected) return -1;

    /* Serialise writes — uart1_rx_task and ak09911c_task may
     * call ws_send concurrently, which would corrupt the WS stream
     * and trigger CLOSE frame 1002 from the server. */
    xSemaphoreTake(ws_write_mutex, portMAX_DELAY);

    int ret = esp_transport_write(ws_transport, data, (int)len, pdMS_TO_TICKS(500));
    if (ret <= 0) {
        ws_connected = false;
    }

    xSemaphoreGive(ws_write_mutex);
    return ret;
}

/*============================================================
 *  WebSocket → UART1  receive task
 *============================================================*/
static void ws_rx_task(void *pvParameters)
{
    uint8_t buf[UART_RX_BUF_SIZE];
    TickType_t last_rx_tick = 0;

    /* wait for WiFi */
    while (!(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    last_rx_tick = xTaskGetTickCount();

    while (1) {
        /* ── (Re)connect if offline ── */
        if (!ws_transport || !ws_connected) {
            ws_start();   /* retry every loop — safe: returns early if already connected */
            if (!ws_transport || !ws_connected) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            last_rx_tick = xTaskGetTickCount();
        }

        /* ── Keepalive: no data for 60 s → force reconnect ── */
        if ((xTaskGetTickCount() - last_rx_tick) > pdMS_TO_TICKS(60000)) {
            ESP_LOGW(TAG, "WS idle 60 s, forcing reconnect…");
            ws_connected = false;
            ws_stop();
            continue;   /* will retry ws_start at top of loop */
        }

        /* ── Poll for incoming data ── */
        int poll_ret = esp_transport_poll_read(ws_transport, 500);
        if (poll_ret < 0) {
            ESP_LOGW(TAG, "WS poll error %d, reconnecting…", poll_ret);
            ws_connected = false;
            ws_stop();
            continue;
        }
        if (poll_ret == 0) {
            /* No data within 500 ms — loop re-checks keepalive */
            continue;
        }

        /* ── Drain all queued frames (rapid‑fire CMDs won't pile up) ── */
        while (1) {
            int len = esp_transport_read(ws_transport, (char *)buf,
                                         sizeof(buf) - 1, 50);
            if (len > 0) {
                last_rx_tick = xTaskGetTickCount();
                buf[len] = '\0';

                /* CMD: prefix → log to esp_log, skip UART1 */
                if (len >= 4 && memcmp(buf, "CMD:", 4) == 0) {
                    ESP_LOGI(TAG, "CMD: %s", (char *)buf + 4);
                } else {
                    uart_write_bytes(UART1_PORT, (const char *)buf, len);
                    ESP_LOGI(TAG, "WS→UART1 %d B", len);
                }
            } else if (len < 0) {
                ESP_LOGW(TAG, "WS read error %d, reconnecting…", len);
                ws_connected = false;
                ws_stop();
                break;   /* exit drain loop, reconnect at top */
            } else {
                break;   /* len == 0: no more data, back to poll */
            }
        }
    }
}

/*============================================================
 *  UART1 RX → WebSocket  (with NMEA parsing)
 *============================================================*/
static void uart1_rx_task(void *pvParameters)
{
    uint8_t buf[UART_RX_BUF_SIZE];
    int send_fail_count = 0;

    /* wait for WiFi */
    while (!(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    nmea_reset();

    while (1) {
        int len = uart_read_bytes(UART1_PORT, buf, sizeof(buf) - 1,
                                  100 / portTICK_PERIOD_MS);
        if (len > 0) {
            /* ── Feed each byte into NMEA parser ── */
            for (int i = 0; i < len; i++) {
                nmea_feed_byte((char)buf[i]);
            }

            /* ── Forward raw data to server (for frontend display) ── */
            if (ws_send((const char *)buf, len) >= 0) {
                send_fail_count = 0;
            } else {
                send_fail_count++;
                if (send_fail_count > 10) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    send_fail_count = 0;
                }
            }

            /* ── Send parsed GPS data if a new fix cycle completed ── */
            gps_data_t gps;
            if (nmea_get_data(&gps)) {
                char json[384];
                int json_len = nmea_to_json(&gps, json, sizeof(json));
                if (json_len > 0 && json_len < (int)sizeof(json)) {
                    ws_send(json, (size_t)json_len);
                    /* Also log summary to UART0 */
                    ESP_LOGI(TAG, "GPS fix=%d sat_used=%d/%d SNRmax=%d",
                             gps.fix_quality, gps.satellites_used,
                             gps.satellites_in_view, gps.max_snr);
                }
            }
        }
    }
}

/*============================================================
 *  Kalman filter — 1D azimuth smoothing
 *  State: angle [0, 360)°, process noise Q, measurement noise R
 *============================================================*/

/* ── Tune these to balance responsiveness vs smoothness ── */
#define KF_Q  2.0f    /* process noise  (deg²) — higher → trust prediction less */
#define KF_R  10.0f   /* measurement noise (deg²) — higher → trust sensor less */

typedef struct {
    float angle;       /* filtered azimuth [0, 360) */
    float P;           /* error covariance */
    bool  ready;
} kalman1d_t;

static kalman1d_t kf_az;

/* Normalize angle innovation to [-180, 180] */
static float angle_diff(float a, float b)
{
    float d = a - b;
    while (d > 180.0f)  d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static void kalman1d_init(kalman1d_t *kf, float first_measurement)
{
    kf->angle = first_measurement;
    kf->P     = 100.0f;   /* start uncertain */
    kf->ready = true;
}

static float kalman1d_update(kalman1d_t *kf, float z)
{
    if (!kf->ready) {
        kalman1d_init(kf, z);
        return kf->angle;
    }

    /* ── Predict ── */
    float x_prior = kf->angle;          /* constant model (no gyro) */
    float P_prior = kf->P + KF_Q;

    /* ── Innovation (angle‑aware) ── */
    float y = angle_diff(z, x_prior);   /* z - x_prior, wrapped */

    /* ── Update ── */
    float K  = P_prior / (P_prior + KF_R);
    float x_post = x_prior + K * y;
    float P_post = (1.0f - K) * P_prior;

    /* Normalise to [0, 360) */
    while (x_post >= 360.0f) x_post -= 360.0f;
    while (x_post < 0.0f)    x_post += 360.0f;

    kf->angle = x_post;
    kf->P     = P_post;
    return kf->angle;
}

/*============================================================
 *  AK09911C 10Hz read → WebSocket task  (azimuth + Kalman)
 *============================================================*/
static void ak09911c_task(void *pvParameters)
{
    /* wait for WiFi */
    while (!(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!ak09911c_init()) {
        ESP_LOGE(TAG, "AK09911C init failed — mag task exiting");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "AK09911C OK, reading @ 10Hz");

    ak09911c_data_t mag;
    char json[256];
    int mag_log_cnt = 0;

    while (1) {
        /* 10Hz = 100ms period */
        vTaskDelay(pdMS_TO_TICKS(500));

        if (ak09911c_read(&mag)) {
            /* ── Compute azimuth from mag X/Y ── */
            float az_raw = (float)(atan2(mag.mag_y, mag.mag_x) * 180.0 / M_PI);
            if (az_raw < 0.0f) az_raw += 360.0f;

            /* ── Kalman filter ── */
            float az_filtered = kalman1d_update(&kf_az, az_raw);

            /* ── WebSocket upload with azimuth ── */
            if (ws_transport && ws_connected) {
                int len = snprintf(json, sizeof(json),
                    "{\"type\":\"mag\",\"mag\":[%.4f,%.4f,%.4f],"
                    "\"azimuth\":%.2f,\"azimuth_raw\":%.2f,"
                    "\"overflow\":%s}",
                    mag.mag_x, mag.mag_y, mag.mag_z,
                    az_filtered, az_raw,
                    mag.overflow ? "true" : "false");
                if (len > 0 && len < (int)sizeof(json)) {
                    ws_send(json, (size_t)len);
                }
            }

            /* console print every 1s */
            if (++mag_log_cnt >= 10) {
                mag_log_cnt = 0;
                ESP_LOGW(TAG, "MAG x=%.1f y=%.1f z=%.1f µT  az=%.1f° (raw=%.1f°)",
                         mag.mag_x, mag.mag_y, mag.mag_z,
                         az_filtered, az_raw);
            }
        } else {
            /* read failed — print diag every 2s */
            static int fail_cnt = 0;
            if (++fail_cnt >= 20) {
                fail_cnt = 0;
                ESP_LOGW(TAG, "MAG read fail");
            }
        }
    }
}

/*============================================================
 *  WiFi event handler
 *============================================================*/
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        ws_stop();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected  SSID=" WIFI_SSID "  IP=" IPSTR,
                     IP2STR(&ip.ip));
            ws_cfg_print();
        }

        ws_start();

        if (!tasks_started) {
            tasks_started = true;
            xTaskCreate(uart1_rx_task, "uart1_rx_task", 4096, NULL, 5, NULL);
            xTaskCreate(ws_rx_task, "ws_rx_task", 4096, NULL, 5, NULL);
            xTaskCreate(ak09911c_task, "ak09911c_task", 4096, NULL, 5, NULL);
        }
    }
}

/*============================================================
 *  WiFi init (STA mode)
 *============================================================*/
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_LOGI(TAG, "Connecting to SSID %s", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/*============================================================
 *  UART init
 *============================================================*/
static void uart0_init(void)
{
    uart_driver_delete(UART0_PORT);

    uart_config_t cfg = {
        .baud_rate  = UART0_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART0_PORT, UART_RX_BUF_SIZE * 2,
                                        0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART0_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART0_PORT, UART0_TX_GPIO, UART0_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void uart1_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART1_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART1_PORT, UART_RX_BUF_SIZE * 2,
                                        UART_TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART1_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART1_PORT, UART1_TX_GPIO, UART1_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/*============================================================
 *  app_main
 *============================================================*/
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    ws_cfg_mutex = xSemaphoreCreateMutex();
    if (!ws_cfg_mutex) {
        ESP_LOGE(TAG, "Failed to create cfg mutex");
        return;
    }

    ws_write_mutex = xSemaphoreCreateMutex();
    if (!ws_write_mutex) {
        ESP_LOGE(TAG, "Failed to create write mutex");
        return;
    }

    ws_cfg_set(WS_SERVER_IP_DEFAULT, WS_SERVER_PORT_DEFAULT);

    uart0_init();
    uart1_init();

    wifi_init_sta();
}
