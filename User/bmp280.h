/**
 * @file    bmp280.h
 * @brief   BMP280 温度/气压传感器驱动 (I2C1)
 * @note    I2C1 重映射: PB8(SCL), PB9(SDA)
 *          SDO 由 PB12 控制: 低=0x76, 高=0x77
 */

#ifndef __BMP280_H
#define __BMP280_H

#include "ch32v10x.h"

/* BMP280 SDO 控制引脚 (PB12) */
#define BMP280_SDO_PORT         GPIOB
#define BMP280_SDO_PIN          GPIO_Pin_12

/* BMP280 I2C 地址 */
#define BMP280_I2C_ADDR_LO      0x76    /* SDO 低电平 */
#define BMP280_I2C_ADDR_HI      0x77    /* SDO 高电平 */

/* BMP280 寄存器定义 */
#define BMP280_REG_ID           0xD0    /* 芯片ID, 应为 0x58 */
#define BMP280_REG_RESET        0xE0    /* 复位寄存器 */
#define BMP280_REG_STATUS       0xF3    /* 状态寄存器 */
#define BMP280_REG_CTRL_MEAS    0xF4    /* 测量控制寄存器 */
#define BMP280_REG_CONFIG       0xF5    /* 配置寄存器 */
#define BMP280_REG_PRESS_MSB    0xF7    /* 气压数据 MSB */
#define BMP280_REG_PRESS_LSB    0xF8    /* 气压数据 LSB */
#define BMP280_REG_PRESS_XLSB   0xF9    /* 气压数据 XLSB */
#define BMP280_REG_TEMP_MSB     0xFA    /* 温度数据 MSB */
#define BMP280_REG_TEMP_LSB     0xFB    /* 温度数据 LSB */
#define BMP280_REG_TEMP_XLSB    0xFC    /* 温度数据 XLSB */

/* BMP280 校准数据寄存器起始地址 */
#define BMP280_CALIB_START      0x88
#define BMP280_CALIB_LEN        26

/* 测量模式 */
#define BMP280_MODE_SLEEP       0x00
#define BMP280_MODE_FORCED      0x01
#define BMP280_MODE_NORMAL      0x03

/* 过采样设置 */
#define BMP280_OSRS_T_SKIP      0x00    /* 跳过温度测量 */
#define BMP280_OSRS_T_X1        0x20    /* ×1 */
#define BMP280_OSRS_T_X2        0x40    /* ×2 */
#define BMP280_OSRS_T_X4        0x60    /* ×4 */
#define BMP280_OSRS_T_X8        0x80    /* ×8 */
#define BMP280_OSRS_T_X16       0xA0    /* ×16 */

#define BMP280_OSRS_P_SKIP      0x00    /* 跳过气压测量 */
#define BMP280_OSRS_P_X1        0x04    /* ×1 */
#define BMP280_OSRS_P_X2        0x08    /* ×2 */
#define BMP280_OSRS_P_X4        0x0C    /* ×4 */
#define BMP280_OSRS_P_X8        0x10    /* ×8 */
#define BMP280_OSRS_P_X16       0x14    /* ×16 */

/* 滤波器设置 */
#define BMP280_FILTER_OFF       0x00
#define BMP280_FILTER_X2        0x04
#define BMP280_FILTER_X4        0x08
#define BMP280_FILTER_X8        0x0C
#define BMP280_FILTER_X16       0x10

/* 待机时间 (正常模式下) */
#define BMP280_STANDBY_0_5MS    0x00
#define BMP280_STANDBY_62_5MS   0x20
#define BMP280_STANDBY_125MS    0x40
#define BMP280_STANDBY_250MS    0x60
#define BMP280_STANDBY_500MS    0x80
#define BMP280_STANDBY_1000MS   0xA0
#define BMP280_STANDBY_2000MS   0xC0
#define BMP280_STANDBY_4000MS   0xE0

/* 校准数据结构体 */
typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
} BMP280_CalibData;

/* 函数声明 */
uint8_t BMP280_Init(void);
uint8_t BMP280_ReadID(void);
float   BMP280_ReadTemperature(void);
float   BMP280_ReadPressure(void);
void    BMP280_Reset(void);

#endif /* __BMP280_H */
