/*
 * ak09911c.c — AK09911C magnetometer (ESP-IDF v6.0 I2C master)
 * SCL=GPIO12, SDA=GPIO11, 100kHz, 100Hz continuous, 0.15µT/LSB
 *
 * Avoids i2c_master_probe — incompatible with async I2C driver.
 * Instead: add device → try WHO_AM_I → keep or remove.
 */
#include "ak09911c.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ak09911c";

static i2c_master_bus_handle_t g_bus = NULL;
static i2c_master_dev_handle_t  g_dev = NULL;

/* ── Sync helpers ──────────────────────────────────────── */

static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    /* sync mode: no wait_all_done needed */
    return i2c_master_transmit(g_dev, buf, sizeof(buf), 50);
}

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    /* retry once on failure */
    esp_err_t ret = i2c_master_transmit_receive(g_dev, &reg, 1, data, len, 50);
    if (ret != ESP_OK) {
        ret = i2c_master_transmit_receive(g_dev, &reg, 1, data, len, 50);
    }
    return ret;
}

/* ── Try device at address (add → read WIA → keep/remove) ─ */

static bool try_addr(uint8_t addr, uint8_t *w1_out, uint8_t *w2_out)
{
    if (g_dev) { i2c_master_bus_rm_device(g_dev); g_dev = NULL; }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = AK09911C_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(g_bus, &dc, &g_dev) != ESP_OK) return false;

    uint8_t w1 = 0xFF, w2 = 0xFF;
    esp_err_t r = i2c_read_reg(AK09911C_REG_WIA1, &w1, 1);
    i2c_read_reg(AK09911C_REG_WIA2, &w2, 1);
    if (w1_out) *w1_out = w1;
    if (w2_out) *w2_out = w2;

    if (r != ESP_OK || w1 == 0x00 || w1 == 0xFF) {
        i2c_master_bus_rm_device(g_dev); g_dev = NULL;
        return false;
    }
    return true;
}

/* ── GPIO diagnostic ───────────────────────────────────── */

static void gpio_diag(void)
{
    gpio_set_direction(AK09911C_I2C_SCL_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(AK09911C_I2C_SDA_GPIO, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(5));
    int scl = gpio_get_level(AK09911C_I2C_SCL_GPIO);
    int sda = gpio_get_level(AK09911C_I2C_SDA_GPIO);
    ESP_LOGI(TAG, "GPIO: SCL=%d SDA=%d", scl, sda);
    if (!scl) ESP_LOGW(TAG, "SCL stuck LOW!");
    if (!sda) ESP_LOGW(TAG, "SDA stuck LOW!");
}

/* ── Init ──────────────────────────────────────────────── */

bool ak09911c_init(void)
{
    /* 1. GPIO check before touching I2C peripheral */
    gpio_diag();

    /* 2. Create bus — synchronous mode (trans_queue_depth=1) */
    i2c_master_bus_config_t bc = {
        .i2c_port = AK09911C_I2C_PORT,
        .sda_io_num = AK09911C_I2C_SDA_GPIO,
        .scl_io_num = AK09911C_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t r = i2c_new_master_bus(&bc, &g_bus);
    if (r != ESP_OK) { ESP_LOGE(TAG, "bus: %d", r); return false; }
    ESP_LOGI(TAG, "I2C SCL=%d SDA=%d @%dHz",
             AK09911C_I2C_SCL_GPIO, AK09911C_I2C_SDA_GPIO, AK09911C_I2C_FREQ_HZ);

    /* 3. Try 0x0C → 0x0F */
    const uint8_t ads[] = { 0x0C, 0x0D, 0x0E, 0x0F };
    uint8_t found = 0, w1 = 0, w2 = 0;
    for (int i = 0; i < sizeof(ads); i++) {
        ESP_LOGI(TAG, "try 0x%02X ...", ads[i]);
        if (try_addr(ads[i], &w1, &w2)) { found = ads[i]; break; }
    }
    if (!found) {
        ESP_LOGE(TAG, "not found on 0x0C-0x0F. Wiring/power ok?");
        return false;
    }
    ESP_LOGI(TAG, "WIA @0x%02X: 0x%02X 0x%02X", found, w1, w2);

    if (w1 != 0x48) ESP_LOGW(TAG, "WIA1 0x%02X != 0x48", w1);

    /* 4. Ensure power-down first (clean state) */
    r = i2c_write_reg(AK09911C_REG_CNTL2, AK09911C_MODE_POWERDOWN);
    if (r != ESP_OK) { ESP_LOGE(TAG, "pwr-down: %d", r); return false; }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 5. Read back CNTL2 to verify write is working */
    uint8_t cntl2_rd = 0xFF;
    r = i2c_read_reg(AK09911C_REG_CNTL2, &cntl2_rd, 1);
    ESP_LOGI(TAG, "CNTL2 rback after PD = 0x%02X", cntl2_rd);

    /* 6. Continuous 100Hz (CNTL2) */
    r = i2c_write_reg(AK09911C_REG_CNTL2, AK09911C_MODE_CONT_100HZ);
    if (r != ESP_OK) { ESP_LOGE(TAG, "CNTL2: %d", r); return false; }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 7. Read back CNTL2 to confirm mode was set */
    cntl2_rd = 0xFF;
    r = i2c_read_reg(AK09911C_REG_CNTL2, &cntl2_rd, 1);
    ESP_LOGI(TAG, "CNTL2 rback after set = 0x%02X (expect 0x%02X)",
             cntl2_rd, AK09911C_MODE_CONT_100HZ);
    if (cntl2_rd != AK09911C_MODE_CONT_100HZ) {
        ESP_LOGE(TAG, "CNTL2 write verify FAILED!");
    }

    /* 8. Wait for first measurement (100Hz → 10ms + margin) */
    vTaskDelay(pdMS_TO_TICKS(30));

    ESP_LOGI(TAG, "OK @0x%02X 100Hz", found);
    return true;
}

/* ── Read ──────────────────────────────────────────────── */

bool ak09911c_read(ak09911c_data_t *d)
{
    if (!d || !g_dev) return false;
    memset(d, 0, sizeof(*d));

    uint8_t st1 = 0;
    esp_err_t r = i2c_read_reg(AK09911C_REG_ST1, &st1, 1);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "ST1 read err %d", r);
        return false;
    }
    if (!(st1 & 0x01)) {
        /* DRDY not set — print every time for first 20 tries, then throttle */
        static int drdy_miss = 0;
        if (++drdy_miss <= 20 || drdy_miss % 50 == 0) {
            ESP_LOGW(TAG, "ST1=0x%02X DRDY not ready (miss #%d)", st1, drdy_miss);
        }
        return false;
    }

    uint8_t raw[8] = {0};
    r = i2c_read_reg(AK09911C_REG_HXL, raw, 8);
    if (r != ESP_OK) { ESP_LOGW(TAG, "data read err %d", r); return false; }
    if (raw[7] & 0x08) { d->overflow = true; return false; }

    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);

    d->mag_x = (float)x * 0.15f;
    d->mag_y = (float)y * 0.15f;
    d->mag_z = (float)z * 0.15f;
    d->data_valid = true;
    return true;
}

/* ── JSON ──────────────────────────────────────────────── */

int ak09911c_to_json(const ak09911c_data_t *d, char *buf, size_t sz)
{
    if (!d || !buf || !sz) return 0;
    return snprintf(buf, sz,
        "{\"type\":\"mag\",\"mag\":[%.4f,%.4f,%.4f],\"overflow\":%s}",
        d->mag_x, d->mag_y, d->mag_z, d->overflow ? "true" : "false");
}
