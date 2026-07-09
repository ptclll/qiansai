/**
 * @file    sensor.h
 * @brief   传感器统一数据结构
 */

#ifndef __SENSOR_H
#define __SENSOR_H

#include <stdint.h>

/* 传感器数据汇总 */
typedef struct {
    float    temperature;    /* 温度 °C (BMP280)       */
    float    humidity;       /* 湿度 %  (BMP280 无此项，预留) */
    uint16_t co2;           /* CO2 浓度 ppm (TVOC-301) */
    float    pressure;       /* 气压 hPa (BMP280)       */
} SensorData;

/* 全局传感器数据实例 */
extern SensorData g_sensor;

#endif /* __SENSOR_H */
