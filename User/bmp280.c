/**
 * @file    bmp280.c
 * @brief   BMP280 温度/气压传感器驱动实现 (I2C1)
 * @note    SDO 由 PB12 控制, 自动探测地址 0x76/0x77
 */

#include "bmp280.h"
#include "ch32v10x_i2c.h"
#include "ch32v10x_gpio.h"
#include "ch32v10x_rcc.h"

/* 全局校准数据 */
static BMP280_CalibData bmp280_calib;
static int32_t t_fine;          /* 温度补偿中间值 */
static uint8_t i2c_addr;        /* 当前 I2C 地址 (0x76 或 0x77) */

/* I2C 超时等待 */
#define I2C_TIMEOUT_MAX     0xFFFF

/* -------------------------------------------------------------------
 * I2C 底层读写函数
 * ------------------------------------------------------------------- */

static uint8_t BMP280_WriteReg(uint8_t reg, uint8_t value)
{
    uint32_t timeout = I2C_TIMEOUT_MAX;

    I2C_GenerateSTART(I2C1, ENABLE);
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    I2C_Send7bitAddress(I2C1, i2c_addr << 1, I2C_Direction_Transmitter);
    timeout = I2C_TIMEOUT_MAX;
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    I2C_SendData(I2C1, reg);
    timeout = I2C_TIMEOUT_MAX;
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    I2C_SendData(I2C1, value);
    timeout = I2C_TIMEOUT_MAX;
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    I2C_GenerateSTOP(I2C1, ENABLE);
    return 0;
}

static uint8_t BMP280_ReadRegs(uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint32_t timeout = I2C_TIMEOUT_MAX;

    I2C_GenerateSTART(I2C1, ENABLE);
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    I2C_Send7bitAddress(I2C1, i2c_addr << 1, I2C_Direction_Transmitter);
    timeout = I2C_TIMEOUT_MAX;
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    I2C_SendData(I2C1, reg);
    timeout = I2C_TIMEOUT_MAX;
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    /* 重复起始 */
    I2C_GenerateSTART(I2C1, ENABLE);
    timeout = I2C_TIMEOUT_MAX;
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    I2C_Send7bitAddress(I2C1, i2c_addr << 1, I2C_Direction_Receiver);
    timeout = I2C_TIMEOUT_MAX;
    while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED) == NoREADY) {
        if (--timeout == 0) return 1;
    }

    while (len) {
        if (len == 1) {
            I2C_AcknowledgeConfig(I2C1, DISABLE);
        }
        timeout = I2C_TIMEOUT_MAX;
        while (I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_RECEIVED) == NoREADY) {
            if (--timeout == 0) return 1;
        }
        *buf++ = I2C_ReceiveData(I2C1);
        len--;
    }

    I2C_AcknowledgeConfig(I2C1, ENABLE);
    I2C_GenerateSTOP(I2C1, ENABLE);
    return 0;
}

/* -------------------------------------------------------------------
 * SDO 控制 (PB12)
 * ------------------------------------------------------------------- */

static void BMP280_SDO_Set(uint8_t level)
{
    if (level) {
        GPIO_SetBits(BMP280_SDO_PORT, BMP280_SDO_PIN);
    } else {
        GPIO_ResetBits(BMP280_SDO_PORT, BMP280_SDO_PIN);
    }
}

/* -------------------------------------------------------------------
 * 校准数据 & 补偿计算 (不变)
 * ------------------------------------------------------------------- */

static void BMP280_ReadCalibData(void)
{
    uint8_t buf[BMP280_CALIB_LEN];
    BMP280_ReadRegs(BMP280_CALIB_START, buf, BMP280_CALIB_LEN);

    bmp280_calib.dig_T1 = (uint16_t)(buf[0]  | (buf[1] << 8));
    bmp280_calib.dig_T2 = (int16_t)( buf[2]  | (buf[3] << 8));
    bmp280_calib.dig_T3 = (int16_t)( buf[4]  | (buf[5] << 8));
    bmp280_calib.dig_P1 = (uint16_t)(buf[6]  | (buf[7] << 8));
    bmp280_calib.dig_P2 = (int16_t)( buf[8]  | (buf[9] << 8));
    bmp280_calib.dig_P3 = (int16_t)( buf[10] | (buf[11] << 8));
    bmp280_calib.dig_P4 = (int16_t)( buf[12] | (buf[13] << 8));
    bmp280_calib.dig_P5 = (int16_t)( buf[14] | (buf[15] << 8));
    bmp280_calib.dig_P6 = (int16_t)( buf[16] | (buf[17] << 8));
    bmp280_calib.dig_P7 = (int16_t)( buf[18] | (buf[19] << 8));
    bmp280_calib.dig_P8 = (int16_t)( buf[20] | (buf[21] << 8));
    bmp280_calib.dig_P9 = (int16_t)( buf[22] | (buf[23] << 8));
}

static int32_t BMP280_CompensateTemperature(int32_t adc_T)
{
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)bmp280_calib.dig_T1 << 1)))
            * ((int32_t)bmp280_calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1))
              * ((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1))) >> 12)
            * ((int32_t)bmp280_calib.dig_T3)) >> 14;
    t_fine = var1 + var2;
    return t_fine;
}

/* Bosch 官方 32 位定点气压补偿 (BMP280 datasheet §4.2.3) */
static uint32_t BMP280_CompensatePressure(int32_t adc_P)
{
    int32_t  var1, var2;
    uint32_t p;

    var1 = (((int32_t)t_fine) >> 1) - (int32_t)64000;
    var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * ((int32_t)bmp280_calib.dig_P6);
    var2 = var2 + ((var1 * ((int32_t)bmp280_calib.dig_P5)) << 1);
    var2 = (var2 >> 2) + (((int32_t)bmp280_calib.dig_P4) << 16);
    var1 = (((bmp280_calib.dig_P3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3)
           + ((((int32_t)bmp280_calib.dig_P2) * var1) >> 1)) >> 18;
    var1 = ((((32768 + var1)) * ((int32_t)bmp280_calib.dig_P1)) >> 15);
    if (var1 == 0) return 0;

    p = (((uint32_t)(((int32_t)1048576) - adc_P) >> 31) - var2) * 3125;
    if (p < 0x80000000)
        p = (p << 1) / ((uint32_t)var1);
    else
        p = (p / (uint32_t)var1) * 2;

    var1 = (((int32_t)bmp280_calib.dig_P9) * ((int32_t)(((p >> 3) * (p >> 3)) >> 13))) >> 12;
    var2 = (((int32_t)(p >> 2)) * ((int32_t)bmp280_calib.dig_P8)) >> 13;
    p = (uint32_t)((int32_t)p + ((var1 + var2 + bmp280_calib.dig_P7) >> 4));

    return p;
}

/* -------------------------------------------------------------------
 * 公开 API
 * ------------------------------------------------------------------- */

/**
 * @brief  用指定地址尝试读取芯片 ID
 * @return 芯片 ID (0x58=成功, 其他=失败)
 */
static uint8_t BMP280_TryReadID(uint8_t addr)
{
    uint8_t id = 0;
    i2c_addr = addr;
    BMP280_ReadRegs(BMP280_REG_ID, &id, 1);
    return id;
}

uint8_t BMP280_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    I2C_InitTypeDef   I2C_InitStructure;
    uint8_t           id;

    /* ---- 使能时钟 ---- */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);

    /* ---- 重映射 I2C1 到 PB8/PB9 ---- */
    GPIO_PinRemapConfig(GPIO_Remap_I2C1, ENABLE);

    /* ---- PB8(SCL), PB9(SDA): 复用开漏 ---- */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_8 | GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_OD;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* ---- PB12: 推挽输出, 控制 SDO ---- */
    GPIO_InitStructure.GPIO_Pin   = BMP280_SDO_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(BMP280_SDO_PORT, &GPIO_InitStructure);

    /* ---- 配置 I2C1 ---- */
    I2C_InitStructure.I2C_ClockSpeed          = 100000;
    I2C_InitStructure.I2C_Mode                = I2C_Mode_I2C;
    I2C_InitStructure.I2C_DutyCycle           = I2C_DutyCycle_2;
    I2C_InitStructure.I2C_OwnAddress1         = 0x00;
    I2C_InitStructure.I2C_Ack                 = I2C_Ack_Enable;
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2C1, &I2C_InitStructure);
    I2C_Cmd(I2C1, ENABLE);

    /* ---- 探测地址: 先试 0x76 (SDO 低) ---- */
    BMP280_SDO_Set(0);
    id = BMP280_TryReadID(BMP280_I2C_ADDR_LO);
    if (id == 0x58) {
        /* 地址 0x76 成功 */
        i2c_addr = BMP280_I2C_ADDR_LO;
    } else {
        /* 再试 0x77 (SDO 高) */
        BMP280_SDO_Set(1);
        id = BMP280_TryReadID(BMP280_I2C_ADDR_HI);
        if (id == 0x58) {
            i2c_addr = BMP280_I2C_ADDR_HI;
        } else {
            return 1;   /* 两个地址都失败 */
        }
    }

    /* ---- 复位并读校准数据 ---- */
    BMP280_Reset();
    BMP280_ReadCalibData();

    /* ---- 配置: 温度×1, 气压×1, 强制模式 ---- */
    BMP280_WriteReg(BMP280_REG_CTRL_MEAS,
                    BMP280_OSRS_T_X1 | BMP280_OSRS_P_X1 | BMP280_MODE_FORCED);
    BMP280_WriteReg(BMP280_REG_CONFIG,
                    BMP280_FILTER_OFF | BMP280_STANDBY_0_5MS);

    return 0;
}

uint8_t BMP280_ReadID(void)
{
    uint8_t id = 0;
    BMP280_ReadRegs(BMP280_REG_ID, &id, 1);
    return id;
}

void BMP280_Reset(void)
{
    BMP280_WriteReg(BMP280_REG_RESET, 0xB6);
    for (volatile uint32_t i = 0; i < 100000; i++);
}

float BMP280_ReadTemperature(void)
{
    uint8_t buf[3];
    int32_t adc_T;

    BMP280_WriteReg(BMP280_REG_CTRL_MEAS,
                    BMP280_OSRS_T_X1 | BMP280_OSRS_P_X1 | BMP280_MODE_FORCED);
    for (volatile uint32_t i = 0; i < 100000; i++);

    BMP280_ReadRegs(BMP280_REG_TEMP_MSB, buf, 3);
    adc_T = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    BMP280_CompensateTemperature(adc_T);
    return (float)(t_fine) / 5120.0f;
}

float BMP280_ReadPressure(void)
{
    uint8_t buf[3];
    int32_t adc_P;

    BMP280_ReadRegs(BMP280_REG_PRESS_MSB, buf, 3);
    adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    uint32_t pressure_pa = BMP280_CompensatePressure(adc_P);
    return (float)pressure_pa / 100.0f;
}
