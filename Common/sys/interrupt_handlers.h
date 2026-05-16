#include <stdint.h>

typedef void ( * GPIOInterruptCallback_t ) ( void * pvContext );

void GPIO_EXTI_Register_Callback( uint16_t usGpioPinMask, GPIOInterruptCallback_t pvCallback, void * pvContext );
void HAL_GPIO_EXTI_Falling_Callback( uint16_t usGpioPinMask );
void HAL_GPIO_EXTI_Rising_Callback( uint16_t usGpioPinMask );
