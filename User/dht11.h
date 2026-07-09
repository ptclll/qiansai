/**
 * @file    dht11.h
 * @brief   DHT11 温湿度传感器驱动 (单总线, PB15)
 */

#ifndef __DHT11_H
#define __DHT11_H

#include "ch32v10x.h"

/* DHT11 数据引脚 */
#define DHT11_PORT      GPIOB
#define DHT11_PIN       GPIO_Pin_15

/* DHT11 返回状态 */
#define DHT11_OK        0
#define DHT11_ERROR     1

/* 函数声明 */
uint8_t DHT11_Init(void);
uint8_t DHT11_Read(float *temperature, float *humidity);

#endif /* __DHT11_H */
