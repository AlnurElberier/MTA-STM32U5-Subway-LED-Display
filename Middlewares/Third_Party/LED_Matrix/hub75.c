/**
 * @file    hub75.c
 * @brief   HUB75 driver implementation with pre-encoded bitplanes
 */

#include "hub75.h"
#include <string.h>

/* ─── Double Framebuffers for Drawing ────────────────────────────────────── */
static HUB75_Pixel draw_fb[HUB75_ROWS][HUB75_COLS];

/* ─── Double Pre-Encoded Bitplane Buffers for Blazing Fast ISR Toggles ───── */
static uint32_t bitplane_decouple[2][HUB75_HALF_ROWS][HUB75_BCM_BITS][HUB75_COLS];
static uint32_t (* volatile front_bitplane)[HUB75_BCM_BITS][HUB75_COLS] = bitplane_decouple[0];
static uint32_t (* back_bitplane)[HUB75_BCM_BITS][HUB75_COLS]  = bitplane_decouple[1];

/* ─── Scan State Variables ───────────────────────────────────────────────── */
static volatile uint8_t cur_row = 0;   
static volatile uint8_t cur_bit = 0;   

/* ─── High-Speed Bit Manipulation Register Macros ───────────────────────── */
#define CLK_HI()  (HUB75_CTRL_PORT->BSRR = HUB75_CLK_PIN)
#define CLK_LO()  (HUB75_CTRL_PORT->BSRR = (uint32_t)HUB75_CLK_PIN << 16)
#define STB_HI()  (HUB75_CTRL_PORT->BSRR = HUB75_STB_PIN)
#define STB_LO()  (HUB75_CTRL_PORT->BSRR = (uint32_t)HUB75_STB_PIN << 16)
#define OE_ON()   (HUB75_CTRL_PORT->BSRR = (uint32_t)HUB75_OE_PIN << 16)
#define OE_OFF()  (HUB75_CTRL_PORT->BSRR = HUB75_OE_PIN)

static void gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    /* clock enables */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* Output - No Pull - Very High Speed */
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    /* Data output group setup (PA0-PA5) */
    g.Pin   = HUB75_DATA_MASK;
    HAL_GPIO_Init(HUB75_DATA_PORT, &g);

    /* Address output group setup (PC0-PC2) */
    g.Pin = HUB75_ADDR_MASK;
    HAL_GPIO_Init(HUB75_ADDR_PORT, &g);

    /* Control output pin group setup (PD0-PD2) */
    g.Pin = HUB75_CTRL_MASK;
    HAL_GPIO_Init(HUB75_CTRL_PORT, &g);

    /* Default safe offline configuration */
    HUB75_DATA_PORT->BSRR = (uint32_t)HUB75_DATA_MASK << 16;  
    HUB75_ADDR_PORT->BSRR = (uint32_t)HUB75_ADDR_MASK << 16;  
    CLK_LO();
    STB_LO();
    OE_OFF();   
}

static inline void set_row_addr(uint8_t row)
{
    /* Distribute row bits [2:0] safely across PC0, PC2, and PC4 */
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
 * @brief  Converts standard canvas pixels into atomic BSRR bit patterns
 * and performs an instant pointer swap.
 */
void HUB75_SwapBuffers(void)
{
    uint32_t clear_mask = (uint32_t)HUB75_DATA_MASK << 16;

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

                /* Explicitly align color bits to their exact GPIOE pin positions */
                uint32_t word = (r1 << 0)  |   /* PE0  */
                                (g1 << 7)  |   /* PE7  */
                                (b1 << 12) |   /* PE12 */
                                (r2 << 13) |   /* PE13 */
                                (g2 << 14) |   /* PE14 */
                                (b2 << 15);    /* PE15 */

                back_bitplane[row][bit][col] = clear_mask | word;
            }
        }
    }

    /* Perform the atomic double-buffer pointer toggle */
    uint32_t (*tmp)[HUB75_BCM_BITS][HUB75_COLS] = front_bitplane;
    front_bitplane = back_bitplane;
    back_bitplane  = tmp;
}

/* ─── Pipelined Hardware Interrupt Routine ─────────────────────────────────── */
void HUB75_ISR(void)
{
    // 1. Instantly blank the display to prevent ghosting artifacts
    OE_OFF();

    // 2. Commit the column data shifted in during the previous cycle
    STB_HI();
    __NOP(); __NOP(); 
    STB_LO();

    // 3. Switch the active row select address lines
    set_row_addr(cur_row);

    // 4. Re-enable output illumination
    OE_ON();

    // 5. Update Timer ARR for exponential BCM scaling duration
    htim2.Instance->ARR = BASE_PERIOD_TICKS << cur_bit;

    // 6. Transition state trackers to map the NEXT frame slice
    if (++cur_bit >= HUB75_BCM_BITS) {
        cur_bit = 0;
        if (++cur_row >= HUB75_HALF_ROWS) {
            cur_row = 0;
        }
    }

    // 7. Shift data into registers while the current row illuminates
    uint32_t *p_next_line = &front_bitplane[cur_row][cur_bit][0];
    for (uint8_t col = 0; col < HUB75_COLS; col++) {
        HUB75_DATA_PORT->BSRR = p_next_line[col];
        CLK_HI();
        CLK_LO();
    }
}
