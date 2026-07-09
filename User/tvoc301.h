/**
 * @file    tvoc301.h
 * @brief   TVOC-301 二氧化碳传感器驱动 (USART3, 9600, 8N1)
 */

#ifndef __TVOC301_H
#define __TVOC301_H

#include "ch32v10x.h"

/* USART3 接收缓冲区大小 */
#define TVOC301_RX_BUF_SIZE     64

/* TVOC-301 返回码 */
#define TVOC301_OK              0
#define TVOC301_ERROR_TIMEOUT   1
#define TVOC301_ERROR_CHKSUM    2

/* 传感器数据 */
typedef struct {
    uint16_t co2;           /* CO2 浓度 (ppm) */
    uint8_t  dataReady;     /* 数据就绪标志 */
} TVOC301_Data;

/* 函数声明 */
void     TVOC301_Init(void);
void     TVOC301_ProcessByte(uint8_t ch);
uint8_t  TVOC301_GetCO2(uint16_t *co2);
void     TVOC301_USART3_IRQHandler_Callback(void);

extern TVOC301_Data g_tvoc301;

#endif /* __TVOC301_H */
