/*
 * ak09911c.h — AK09911C 3-axis magnetometer driver (I2C)
 * SCL → GPIO 12, SDA → GPIO 11
 */
#ifndef AK09911C_H
#define AK09911C_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AK09911C_I2C_PORT        I2C_NUM_0
#define AK09911C_I2C_SCL_GPIO    GPIO_NUM_12
#define AK09911C_I2C_SDA_GPIO    GPIO_NUM_11
#define AK09911C_I2C_FREQ_HZ     50000

#define AK09911C_ADDR            0x0C

/* Registers */
#define AK09911C_REG_WIA1        0x00
#define AK09911C_REG_WIA2        0x01
#define AK09911C_REG_ST1         0x10
#define AK09911C_REG_HXL         0x11
#define AK09911C_REG_HXH         0x12
#define AK09911C_REG_HYL         0x13
#define AK09911C_REG_HYH         0x14
#define AK09911C_REG_HZL         0x15
#define AK09911C_REG_HZH         0x16
#define AK09911C_REG_ST2         0x18
#define AK09911C_REG_CNTL1       0x30
#define AK09911C_REG_CNTL2       0x31
#define AK09911C_REG_CNTL3       0x32

/* CNTL1 modes */
#define AK09911C_MODE_POWERDOWN  0x00
#define AK09911C_MODE_SINGLE     0x01
#define AK09911C_MODE_CONT_10HZ  0x02
#define AK09911C_MODE_CONT_20HZ  0x04
#define AK09911C_MODE_CONT_50HZ  0x06
#define AK09911C_MODE_CONT_100HZ 0x08
#define AK09911C_MODE_SELFTEST   0x10

#define AK09911C_SOFT_RESET      0x01

typedef struct {
    float mag_x, mag_y, mag_z;
    bool  data_valid;
    bool  overflow;
} ak09911c_data_t;

bool ak09911c_init(void);
bool ak09911c_read(ak09911c_data_t *data);
int  ak09911c_to_json(const ak09911c_data_t *data, char *json_buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
#endif
