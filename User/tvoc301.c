/**
 * @file    tvoc301.c
 * @brief   TVOC-301 二氧化碳传感器驱动实现
 * @note    使用 USART3, PB10(RX), PB11(TX), 9600, 8N1
 *
 * 常见的 CO2 UART 传感器协议帧 (9 字节):
 *   [0]    起始字节 (0xFF)
 *   [1]    传感器类型 (0x86 = CO2)
 *   [2]    数据高位
 *   [3]    数据低位
 *   [4]    (保留)
 *   [5]    (保留)
 *   [6]    (保留)
 *   [7]    (保留)
 *   [8]    校验和 (前 8 字节和取低 8 位取反+1)
 *
 * 如果实际传感器协议不同，请根据数据手册修改解析逻辑。
 */

#include "tvoc301.h"
#include "ch32v10x_usart.h"
#include "ch32v10x_gpio.h"
#include "ch32v10x_rcc.h"
#include <string.h>

/* 全局传感器数据 */
TVOC301_Data g_tvoc301 = {0, 0};

/* 帧解析状态机 */
#define TVOC_STATE_IDLE         0
#define TVOC_STATE_GOT_HEADER   1
#define TVOC_STATE_RECEIVING    2

static uint8_t  parse_state = TVOC_STATE_IDLE;
static uint8_t  frame_buf[9];
static uint8_t  frame_idx = 0;

/**
 * @brief  初始化 TVOC-301 (USART3)
 */
void TVOC301_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef  NVIC_InitStructure;

    /* ---- 使能时钟 ---- */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    /* ---- 配置 PB10 (TX) ---- */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* ---- 配置 PB11 (RX) ---- */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_11;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* ---- 配置 USART3 ---- */
    USART_InitStructure.USART_BaudRate            = 9600;
    USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity              = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART3, &USART_InitStructure);

    /* ---- 使能接收中断 ---- */
    USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);

    /* ---- 配置 NVIC ---- */
    NVIC_InitStructure.NVIC_IRQChannel                   = USART3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    /* ---- 使能 USART3 ---- */
    USART_Cmd(USART3, ENABLE);
}

/**
 * @brief  处理接收到的字节（在中断中调用）
 */
void TVOC301_ProcessByte(uint8_t ch)
{
    switch (parse_state) {
    case TVOC_STATE_IDLE:
        /* 等待帧头 0xFF */
        if (ch == 0xFF) {
            frame_buf[0] = ch;
            frame_idx = 1;
            parse_state = TVOC_STATE_RECEIVING;
        }
        break;

    case TVOC_STATE_RECEIVING:
        frame_buf[frame_idx++] = ch;
        if (frame_idx >= 9) {
            /* 接收完整一帧，校验 */
            uint8_t checksum = 0;
            for (uint8_t i = 0; i < 8; i++) {
                checksum += frame_buf[i];
            }
            checksum = (~checksum) + 1;

            if (checksum == frame_buf[8]) {
                /* 校验通过，提取 CO2 浓度 */
                g_tvoc301.co2 = ((uint16_t)frame_buf[2] << 8) | frame_buf[3];
                g_tvoc301.dataReady = 1;
            }

            /* 回到空闲状态 */
            parse_state = TVOC_STATE_IDLE;
            frame_idx = 0;
        }
        break;

    default:
        parse_state = TVOC_STATE_IDLE;
        frame_idx = 0;
        break;
    }
}

/**
 * @brief  获取 CO2 浓度
 * @param  co2: 输出 CO2 值 (ppm)
 * @return 0: 有新数据, 1: 无新数据
 */
uint8_t TVOC301_GetCO2(uint16_t *co2)
{
    if (g_tvoc301.dataReady) {
        *co2 = g_tvoc301.co2;
        g_tvoc301.dataReady = 0;
        return 0;
    }
    return 1;
}

/**
 * @brief  USART3 中断回调 (在 ch32v10x_it.c 中调用)
 */
void TVOC301_USART3_IRQHandler_Callback(void)
{
    if (USART_GetITStatus(USART3, USART_IT_RXNE) != RESET) {
        uint8_t ch = (uint8_t)USART_ReceiveData(USART3);
        TVOC301_ProcessByte(ch);
        USART_ClearITPendingBit(USART3, USART_IT_RXNE);
    }
}
