#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/ledc.h"

/* ESP32-S3-WROOM-1-N8R8 + TOP028RGB480V3 */
#define BOARD_LCD_H_RES 480
#define BOARD_LCD_V_RES 480

/* ST7701S 3-wire SPI */
#define BOARD_GPIO_LCD_SDA      1
#define BOARD_GPIO_LCD_SCK      2
#define BOARD_GPIO_LCD_CS       42


/* Shared I2C bus used by TCA9554 and other peripherals. */
#define BOARD_GPIO_I2C_SDA      15
#define BOARD_GPIO_I2C_SCL      7
#define BOARD_I2C_PORT          0
#define BOARD_I2C_FREQ_HZ  400000

/* TCA9554 address assumes A0/A1/A2 are tied low on the PCB. */
#define BOARD_TCA9554_ADDR   0x20

/* TCA9554 output numbers, verified from the schematic. */
#define BOARD_IOX_BL_ENABLE    1
#define BOARD_IOX_LCD_RESET    2
#define BOARD_IOX_TERM_ENABLE  6
#define BOARD_IOX_DISP_PWR     7

#define BOARD_IOX_TP_RESET   0
#define BOARD_GPIO_TP_INT    GPIO_NUM_16

/* Direct ESP32-S3 PWM pin */
#define BOARD_GPIO_BL_PWM              GPIO_NUM_6

/* TCA9554 output numbers */
#define BOARD_IOX_BL_ENABLE             1
#define BOARD_IOX_LCD_RESET             2
#define BOARD_IOX_DISP_PWR              7

/* LEDC configuration */
#define BOARD_BL_PWM_SPEED_MODE         LEDC_LOW_SPEED_MODE
#define BOARD_BL_PWM_TIMER              LEDC_TIMER_0
#define BOARD_BL_PWM_CHANNEL            LEDC_CHANNEL_0
#define BOARD_BL_PWM_FREQ_HZ            20000
#define BOARD_BL_PWM_RESOLUTION         LEDC_TIMER_10_BIT
#define BOARD_BL_PWM_RESOLUTION_BITS    10

/* RGB pins will be enabled in the next project stage. */
#define BOARD_GPIO_LCD_PCLK   GPIO_NUM_41
#define BOARD_GPIO_LCD_DE     GPIO_NUM_40
#define BOARD_GPIO_LCD_VSYNC  GPIO_NUM_39
#define BOARD_GPIO_LCD_HSYNC  GPIO_NUM_38

#define BOARD_GPIO_LCD_B1     GPIO_NUM_5
#define BOARD_GPIO_LCD_B2     GPIO_NUM_45
#define BOARD_GPIO_LCD_B3     GPIO_NUM_48
#define BOARD_GPIO_LCD_B4     GPIO_NUM_47
#define BOARD_GPIO_LCD_B5     GPIO_NUM_21

#define BOARD_GPIO_LCD_G0     GPIO_NUM_14
#define BOARD_GPIO_LCD_G1     GPIO_NUM_13
#define BOARD_GPIO_LCD_G2     GPIO_NUM_12
#define BOARD_GPIO_LCD_G3     GPIO_NUM_11
#define BOARD_GPIO_LCD_G4     GPIO_NUM_10
#define BOARD_GPIO_LCD_G5     GPIO_NUM_9

#define BOARD_GPIO_LCD_R1     GPIO_NUM_46
#define BOARD_GPIO_LCD_R2     GPIO_NUM_3
#define BOARD_GPIO_LCD_R3     GPIO_NUM_8
#define BOARD_GPIO_LCD_R4     GPIO_NUM_18
#define BOARD_GPIO_LCD_R5     GPIO_NUM_17