/**
 * @file    dht11.c
 * @brief   DHT11 温湿度传感器驱动 (单总线, PB15)
 * @note    安全方案: 不关全局中断, 用 GPIO_Init 切换模式,
 *          读取出错靠校验和检测, 绝不导致系统崩溃
 */

#include "dht11.h"
#include "ch32v10x_gpio.h"
#include "ch32v10x_rcc.h"

/* 超时 (72MHz 下 ~10ms) */
#define TMO     100000

static void PB15_Out(void)
{
    GPIO_InitTypeDef s;
    GPIO_StructInit(&s);
    s.GPIO_Pin   = DHT11_PIN;
    s.GPIO_Speed = GPIO_Speed_50MHz;
    s.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(DHT11_PORT, &s);
}

static void PB15_In(void)
{
    GPIO_InitTypeDef s;
    GPIO_StructInit(&s);
    s.GPIO_Pin   = DHT11_PIN;
    s.GPIO_Mode  = GPIO_Mode_IPU;    /* 内部上拉 */
    GPIO_Init(DHT11_PORT, &s);
}

static uint8_t ReadBit(void)
{
    uint32_t t;

    t = TMO;
    while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) == Bit_RESET)
        if (--t == 0) return 0xFF;

    t = TMO;
    uint32_t c = 0;
    while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) == Bit_SET) {
        c++;
        if (c > 500) break;
        if (--t == 0) return 0xFF;
    }
    return (c > 80) ? 1 : 0;
}

/* ================================================================ */

uint8_t DHT11_Init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    PB15_Out();
    GPIO_SetBits(DHT11_PORT, DHT11_PIN);
    return DHT11_OK;
}

/**
 * @brief  读 DHT11 (不关中断, 错误靠校验和检测)
 */
uint8_t DHT11_Read(float *temperature, float *humidity)
{
    uint8_t buf[5] = {0, 0, 0, 0, 0};
    uint8_t i, j;

    /* 1. 起始: 推挽拉低 18ms, 拉高 30us */
    PB15_Out();
    GPIO_ResetBits(DHT11_PORT, DHT11_PIN);
    Delay_Ms(18);
    GPIO_SetBits(DHT11_PORT, DHT11_PIN);
    Delay_Us(30);

    /* 2. 切输入上拉, 等应答 */
    PB15_In();

    /* 应答低 */
    uint32_t t = TMO;
    while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) == Bit_RESET)
        if (--t == 0) goto fail;
    /* 应答高 */
    t = TMO;
    while (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_PIN) == Bit_SET)
        if (--t == 0) goto fail;

    /* 3. 读 40 位 */
    for (j = 0; j < 5; j++)
        for (i = 0; i < 8; i++) {
            buf[j] <<= 1;
            uint8_t b = ReadBit();
            if (b == 0xFF) goto fail;
            buf[j] |= b;
        }

    /* 4. 校验 */
    if ((uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]) != buf[4])
        goto fail;

    *humidity    = (float)buf[0];
    *temperature = (float)buf[2];

    /* 恢复输出高 */
    PB15_Out();
    GPIO_SetBits(DHT11_PORT, DHT11_PIN);
    return DHT11_OK;

fail:
    PB15_Out();
    GPIO_SetBits(DHT11_PORT, DHT11_PIN);
    return DHT11_ERROR;
}
