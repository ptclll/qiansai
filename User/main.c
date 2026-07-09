/**
 * @file    main.c
 * @brief   主程序 — 多传感器数据采集与上报
 * @note    硬件配置:
 *          - USART1 (PA9):  调试串口, 115200
 *          - USART2 (PA2/PA3): 接 ESP32, 115200, 发送 JSON
 *          - USART3 (PB10/PB11): 接 TVOC-301 CO2 传感器, 9600
 *          - I2C1 (PB8/PB9): 接 BMP280 温度气压传感器
 */

#include "debug.h"
#include "ch32v10x_usart.h"
#include "ch32v10x_gpio.h"
#include "ch32v10x_rcc.h"
#include "bmp280.h"
#include "tvoc301.h"
#include "dht11.h"
#include "sensor.h"
#include <stdio.h>
#include <string.h>

/* 全局传感器数据 */
SensorData g_sensor;

/* 数据上报间隔 (ms) */
#define REPORT_INTERVAL_MS      2000

/* DHT11 开关: 1=启用, 0=禁用 (调试用) */
#define DHT11_ENABLED           1

/* JSON 缓冲区 */
static char json_buf[256];

/* ===================================================================
 * USART2 发送函数 (ESP32 通信)
 * =================================================================== */

/**
 * @brief  通过 USART2 发送单个字符
 */
static void USART2_PutChar(char ch)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
    USART_SendData(USART2, (uint16_t)ch);
}

/**
 * @brief  通过 USART2 发送字符串
 */
static void USART2_SendString(const char *str)
{
    while (*str) {
        USART2_PutChar(*str++);
    }
}

/**
 * @brief  初始化 USART2 (接 ESP32, 115200, 8N1)
 *         PA2 = TX, PA3 = RX
 */
static void USART2_ESP32_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;

    /* 使能时钟 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    /* PA2 - USART2_TX (复用推挽输出) */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* PA3 - USART2_RX (浮空输入) */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* 配置 USART2 */
    USART_InitStructure.USART_BaudRate            = 115200;
    USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity              = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART2, &USART_InitStructure);

    /* 使能 USART2 */
    USART_Cmd(USART2, ENABLE);
}

/* ===================================================================
 * 传感器数据读取
 * =================================================================== */

/**
 * @brief  读取所有传感器数据
 */
static void ReadAllSensors(void)
{
    /* BMP280: 温度 + 气压 */
    g_sensor.temperature = BMP280_ReadTemperature();
    g_sensor.pressure    = BMP280_ReadPressure();

#if DHT11_ENABLED
    /* DHT11 (PB15): 湿度 */
    {
        float dht11_temp;
        if (DHT11_Read(&dht11_temp, &g_sensor.humidity) != DHT11_OK) {
            g_sensor.humidity = 0.0f;
        }
    }
#else
    g_sensor.humidity = 0.0f;
#endif

    /* TVOC-301: CO2 */
    uint16_t co2_val;
    if (TVOC301_GetCO2(&co2_val) == 0) {
        g_sensor.co2 = co2_val;
    }
}

/* ===================================================================
 * JSON 数据上报
 * =================================================================== */

/**
 * @brief  构建并发送 JSON 数据包到 ESP32
 *         格式: [{Humidity:xx.x,Temperature:xx.x,co2:xxxx,pressure:xxxx.x}]
 * @note   使用整数运算避免 printf 浮点依赖 (newlib-nano 默认不支持 %f)
 */
static void SendJsonReport(void)
{
    int temp_int   = (int)g_sensor.temperature;
    int temp_dec   = (int)(g_sensor.temperature * 10.0f) % 10;
    int press_int  = (int)g_sensor.pressure;
    int press_dec  = (int)(g_sensor.pressure * 10.0f) % 10;
    int humi_int   = (int)g_sensor.humidity;
    int humi_dec   = (int)(g_sensor.humidity * 10.0f) % 10;

    /* 构建 JSON (cJSON 要求双引号键名) */
    snprintf(json_buf, sizeof(json_buf),
             "[{\"Humidity\":%d.%d,\"Temperature\":%d.%d,\"co2\":%d,\"pressure\":%d.%d}]\r\n",
             humi_int,  (humi_dec  < 0 ? -humi_dec  : humi_dec),
             temp_int,  (temp_dec  < 0 ? -temp_dec  : temp_dec),
             g_sensor.co2,
             press_int, (press_dec < 0 ? -press_dec : press_dec));

    /* 通过 USART2 发送 */
    USART2_SendString(json_buf);

    /* 调试输出 (避免 %f) */
    printf("[SENSOR] Temp=%d.%d*C  Humi=%d.%d%%  Press=%d.%dhPa  CO2=%dppm\r\n",
           temp_int,  (temp_dec  < 0 ? -temp_dec  : temp_dec),
           humi_int,  (humi_dec  < 0 ? -humi_dec  : humi_dec),
           press_int, (press_dec < 0 ? -press_dec : press_dec),
           g_sensor.co2);
}

/* ===================================================================
 * 主函数
 * =================================================================== */

int main(void)
{
    /* ---- 系统初始化 ---- */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    /* ---- 调试串口 USART1 (PA9) ---- */
    USART_Printf_Init(115200);
    printf("\r\n========================================\r\n");
    printf("  Daowei Multi-Sensor System v1.0\r\n");
    printf("  SystemClk: %d Hz\r\n", SystemCoreClock);
    printf("  ChipID:    %08x\r\n", DBGMCU_GetCHIPID());
    printf("========================================\r\n\r\n");

    /* ---- USART2 接 ESP32 (PA2/PA3, 115200) ---- */
    printf("[INIT] USART2 (ESP32) ... ");
    USART2_ESP32_Init();
    printf("OK\r\n");

    /* ---- USART3 接 TVOC-301 (PB10/PB11, 9600) ---- */
    printf("[INIT] USART3 (TVOC-301) ... ");
    TVOC301_Init();
    printf("OK\r\n");

    /* ---- DHT11 温湿度 (PB15) ---- */
#if DHT11_ENABLED
    printf("[INIT] DHT11 (PB15) ... ");
    DHT11_Init();
    printf("OK\r\n");
    Delay_Ms(1500);   /* 上电稳定 */
#else
    printf("[INIT] DHT11 (PB15) ... DISABLED\r\n");
#endif

    /* ---- I2C1 接 BMP280 (PB8/PB9, SDO=PB12) ---- */
    printf("[INIT] I2C1 (BMP280) ... ");
    if (BMP280_Init() == 0) {
        printf("OK (ID=0x%02X)\r\n", BMP280_ReadID());
    } else {
        printf("FAILED! Check wiring & SDO(PB12)\r\n");
        printf("[WARN] BMP280 not found, temp/pressure will be 0\r\n");
    }

    printf("\r\n[INFO] System ready, reporting every %d ms\r\n\r\n", REPORT_INTERVAL_MS);

    /* ---- 主循环 ---- */
    while (1)
    {
        /* 读取传感器 */
        ReadAllSensors();

        /* 发送 JSON 到 ESP32 */
        SendJsonReport();

        /* 延时 */
        Delay_Ms(REPORT_INTERVAL_MS);
    }
}
