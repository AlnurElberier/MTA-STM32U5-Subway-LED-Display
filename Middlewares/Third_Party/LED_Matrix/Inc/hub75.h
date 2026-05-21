/**
 * @file    hub75.h
 * @brief   Master HUB75 Driver Header for B-U585I-IOT2A
 */

#ifndef HUB75_H
#define HUB75_H

#include "stm32u5xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* ─── Panel Geometry ─────────────────────────────────────────────────────── */
#define HUB75_COLS       32
#define HUB75_ROWS       16
#define HUB75_HALF_ROWS  (HUB75_ROWS / 2)
#define HUB75_BCM_BITS   8

#define BASE_PERIOD_TICKS 180 

/* ─── Hardware Pin Routing Configuration ─────────────────────────────────── */
#define HUB75_DATA_PORT         GPIOE
#define HUB75_DATA_CLK_ENABLE() __HAL_RCC_GPIOE_CLK_ENABLE()
#define HUB75_R1_PIN           GPIO_PIN_0
#define HUB75_G1_PIN           GPIO_PIN_7
#define HUB75_B1_PIN           GPIO_PIN_12
#define HUB75_R2_PIN           GPIO_PIN_13
#define HUB75_G2_PIN           GPIO_PIN_14
#define HUB75_B2_PIN           GPIO_PIN_15
#define HUB75_DATA_MASK        (HUB75_R1_PIN | HUB75_G1_PIN | HUB75_B1_PIN | \
                                HUB75_R2_PIN | HUB75_G2_PIN | HUB75_B2_PIN)

#define HUB75_ADDR_PORT        GPIOC
#define HUB75_ADDR_CLK_ENABLE() __HAL_RCC_GPIOC_CLK_ENABLE()
#define HUB75_A_PIN            GPIO_PIN_0
#define HUB75_B_PIN            GPIO_PIN_2
#define HUB75_C_PIN            GPIO_PIN_4
#define HUB75_ADDR_MASK        (HUB75_A_PIN | HUB75_B_PIN | HUB75_C_PIN)

#define HUB75_CTRL_PORT        GPIOD
#define HUB75_CTRL_CLK_ENABLE() __HAL_RCC_GPIOD_CLK_ENABLE()
#define HUB75_STB_PIN          GPIO_PIN_9
#define HUB75_OE_PIN           GPIO_PIN_8
#define HUB75_CLK_PIN          GPIO_PIN_15
#define HUB75_CTRL_MASK        (HUB75_CLK_PIN | HUB75_OE_PIN | HUB75_STB_PIN)

extern TIM_HandleTypeDef htim2;

typedef struct {
    uint8_t r, g, b;
} HUB75_Pixel;

/* ─── Driver Core API Functions ──────────────────────────────────────────── */
void HUB75_Init(void);
void HUB75_SetPixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);
void HUB75_Fill(uint8_t r, uint8_t g, uint8_t b);
void HUB75_Clear(void);
void HUB75_SwapBuffers(void);
void HUB75_ISR(void);

#endif /* HUB75_H */
