/**
 * @file    hub75.c
 * @brief   Optimized Parallel Synchronous HUB75 Driver via Single-Port Clock Interleaving
 * @note    Target System: STM32U585 running at 160MHz
 */

#include "hub75.h"
#include <string.h>

/* External handle for the BCM master timer */
extern TIM_HandleTypeDef htim2;

/* ─── Double Framebuffers for Drawing ────────────────────────────────────── */
static HUB75_Pixel draw_fb[HUB75_ROWS][HUB75_COLS];

/* ─── Double Pre-Encoded Bitplane Buffers for Single-Port Interleaving ───── */
/* Each line allocation is 64 entries wide to store alternating clock phases */
static uint32_t bitplane_decouple[2][HUB75_HALF_ROWS][HUB75_BCM_BITS][DMA_SAMPLES_PER_ROW];

static uint32_t (* volatile front_bitplane)[HUB75_BCM_BITS][DMA_SAMPLES_PER_ROW] = bitplane_decouple[0];
static uint32_t (* back_bitplane)[HUB75_BCM_BITS][DMA_SAMPLES_PER_ROW]  = bitplane_decouple[1];

/* ─── Scan State Machine Variables ───────────────────────────────────────── */
static volatile uint8_t cur_row = 0;   
static volatile uint8_t cur_bit = 0;   

/* ─── Control Line Bit Manipulation Macros (Port D Assembly) ─────────────── */
#define STB_HI()  (HUB75_CTRL_PORT->BSRR = HUB75_STB_PIN)
#define STB_LO()  (HUB75_CTRL_PORT->BSRR = (uint32_t)HUB75_STB_PIN << 16)
#define OE_ON()   (HUB75_CTRL_PORT->BSRR = (uint32_t)HUB75_OE_PIN << 16)
#define OE_OFF()  (HUB75_CTRL_PORT->BSRR = HUB75_OE_PIN)

/**
 * @brief  Initializes GPIO Ports C, D, and E for high-speed matrix driving.
 */
static void gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    /* Enable peripheral bus clocks */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* Shared Configuration: Push-Pull, No Pull, Maximum Frequency Gate Slew */
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    /* 1. Unified Data & Clock Output Group (Port E) */
    g.Pin   = HUB75_DATA_MASK;
    HAL_GPIO_Init(HUB75_DATA_PORT, &g);

    /* 2. Row Multiplexer Address Output Group (Port C) */
    g.Pin   = HUB75_ADDR_MASK;
    HAL_GPIO_Init(HUB75_ADDR_PORT, &g);

    /* 3. Latch and Blanking Control Group (Port D) */
    g.Pin   = HUB75_CTRL_MASK;
    HAL_GPIO_Init(HUB75_CTRL_PORT, &g);

    /* Enforce safe default state (Screen blanked, lines low) */
    HUB75_DATA_PORT->BSRR = (uint32_t)HUB75_DATA_MASK << 16;  
    HUB75_ADDR_PORT->BSRR = (uint32_t)HUB75_ADDR_MASK << 16;  
    STB_LO();
    OE_OFF();   
}

/**
 * @brief  Updates the physical row address lines on Port C.
 */
static inline void set_row_addr(uint8_t row)
{
    /* Distribute row address vectors across PC0, PC2, and PC4 */
    uint32_t set_bits = ((row & 1u) << 0)         |   /* Bit 0 -> PC0 */
                        (((row >> 1) & 1u) << 2)  |   /* Bit 1 -> PC2 */
                        (((row >> 2) & 1u) << 4);     /* Bit 2 -> PC4 */

    uint32_t clr_bits = (uint32_t)HUB75_ADDR_MASK << 16;
    HUB75_ADDR_PORT->BSRR = clr_bits | set_bits;
}

/* ─── Driver Lifecycles ──────────────────────────────────────────────────── */

void HUB75_Init(void)
{
    memset(draw_fb, 0, sizeof(draw_fb));
    memset(bitplane_decouple, 0, sizeof(bitplane_decouple));
    gpio_init();
    
    /* Override CubeMX default ARR value with our operational base ticks constant */
    htim2.Instance->ARR = BASE_PERIOD_TICKS;
    HAL_TIM_Base_Start_IT(&htim2);
}

void HUB75_SetPixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= HUB75_COLS || y >= HUB75_ROWS) return;
    draw_fb[y][x].r = r;
    draw_fb[y][x].g = g;
    draw_fb[y][x].b = b;
}

void HUB75_Fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint8_t y = 0; y < HUB75_ROWS; y++) {
        for (uint8_t x = 0; x < HUB75_COLS; x++) {
            draw_fb[y][x].r = r;
            draw_fb[y][x].g = g;
            draw_fb[y][x].b = b;
        }
    }
}

void HUB75_Clear(void)
{
    memset(draw_fb, 0, sizeof(draw_fb));
}

/**
 * @brief  Encodes the standard RGB workspace canvas into alternating
 * setup/shift atomic bit patterns, then triggers a pointer swap.
 */
void HUB75_SwapBuffers(void)
{
    /* Clears color lines AND safely forces the PE4 clock pin low */
    uint32_t clear_mask = (uint32_t)(HUB75_DATA_MASK) << 16;

    for (uint8_t row = 0; row < HUB75_HALF_ROWS; row++) {
        uint8_t bot_row = row + HUB75_HALF_ROWS;
        
        for (uint8_t bit = 0; bit < HUB75_BCM_BITS; bit++) {
            for (uint8_t col = 0; col < HUB75_COLS; col++) {
                
                const HUB75_Pixel *T = &draw_fb[row][col];
                const HUB75_Pixel *B = &draw_fb[bot_row][col];

                uint32_t r1 = (T->r >> bit) & 1u;
                uint32_t g1 = (T->g >> bit) & 1u;
                uint32_t b1 = (T->b >> bit) & 1u;
                uint32_t r2 = (B->r >> bit) & 1u;
                uint32_t g2 = (B->g >> bit) & 1u;
                uint32_t b2 = (B->b >> bit) & 1u;

                /* Shift active components to exact Port E pin targets */
                uint32_t color_word = (r1 << 0)  |   /* PE0  */
                                      (g1 << 7)  |   /* PE7  */
                                      (b1 << 12) |   /* PE12 */
                                      (r2 << 13) |   /* PE13 */
                                      (g2 << 14) |   /* PE14 */
                                      (b2 << 15);    /* PE15 */

                uint8_t sample_idx_low  = col * 2;
                uint8_t sample_idx_high = sample_idx_low + 1;

                /* PHASE 1: Setup Window (Clock line PE4 driven LOW via clear_mask) */
                back_bitplane[row][bit][sample_idx_low] = clear_mask | color_word;

                /* PHASE 2: Shift Window (Clock line PE4 driven HIGH via atomic bitmask) */
                back_bitplane[row][bit][sample_idx_high] = clear_mask | color_word | HUB75_CLK_PIN;
            }
        }
    }

    /* Perform the thread-safe double-buffer pointer address toggle */
    uint32_t (*tmp)[HUB75_BCM_BITS][DMA_SAMPLES_PER_ROW] = front_bitplane;
    front_bitplane = back_bitplane;
    back_bitplane  = tmp;
}

/**
 * @brief  Pipelined Hardware Interrupt Routine managed by Timer 2.
 * Dumps pre-encoded slices using a hyper-optimized single-port stream loop.
 */
void HUB75_ISR(void)
{
    /* 1. Blank display to terminate ghosting artifacts */
    OE_OFF();

    /* 2. Commit column data shifted in during the previous transaction */
    STB_HI();
    __NOP(); __NOP(); 
    STB_LO();

    /* 3. Update physical multiplex lines */
    set_row_addr(cur_row);

    /* 4. Turn illumination back on for active row channel */
    OE_ON();

    /* 5. Scale the next countdown period exponentially for BCM color weights */
    htim2.Instance->ARR = BASE_PERIOD_TICKS << cur_bit;

    /* Advance frame tracker state machine indexes */
    if (++cur_bit >= HUB75_BCM_BITS) {
        cur_bit = 0;
        if (++cur_row >= HUB75_HALF_ROWS) {
            cur_row = 0;
        }
    }

    /* 6. High-Speed Interleaved GPIO Blast:
     * Pulls data from the 64-word pre-calculated stream. The NOP padding
     * throttles the clock down to ~15 MHz, staying cleanly inside
     * the panel's silicon propagation boundaries. */
    uint32_t *p_next_line = &front_bitplane[cur_row][cur_bit][0];
    for (uint8_t sample = 0; sample < DMA_SAMPLES_PER_ROW; sample++)
    {
        HUB75_DATA_PORT->BSRR = p_next_line[sample];
        __NOP();
    }
}
