################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Common/sys/hal_init.c \
../Common/sys/interrupt_handlers.c \
../Common/sys/mbedtls_freertos_port.c \
../Common/sys/newlibc_stubs.c 

OBJS += \
./Common/sys/hal_init.o \
./Common/sys/interrupt_handlers.o \
./Common/sys/mbedtls_freertos_port.o \
./Common/sys/newlibc_stubs.o 

C_DEPS += \
./Common/sys/hal_init.d \
./Common/sys/interrupt_handlers.d \
./Common/sys/mbedtls_freertos_port.d \
./Common/sys/newlibc_stubs.d 


# Each subdirectory must supply rules for building sources it contributes
Common/sys/%.o Common/sys/%.su Common/sys/%.cyclo: ../Common/sys/%.c Common/sys/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION=1 '-DMBEDTLS_CONFIG_FILE="mbedtls_config_ntz.h"' '-DLFS_CONFIG=lfs_config.h' -DLFS_USE_INTERNAL_NOR -DUSE_STSAFE=0 -c -I../Core/Inc -I../STDIO -I../Drivers/STM32U5xx_HAL_Driver/Inc -I../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../Drivers/CMSIS/Include -I../Middlewares/Third_Party/AWS_FreeRTOS/coreJSON/source/include/ -I../Middlewares/Third_Party/lwIP_Network_lwIP/lwip/src/include/ -I../Common/net/lwip_port/include -I../Common/net/lwip_port/include/arch -I../Common/net/mxchip -I../Middlewares/Third_Party/FreeRTOS/Source/include/ -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure/ -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/ -I../Middlewares/Third_Party/CMSIS/RTOS2/Include/ -I../Middlewares/Third_Party/ARM_Security/include/ -I../Middlewares/Third_Party/ARM_Security/RTE/include/ -I../Middlewares/Third_Party/ARM_Security/RTE/configs -I../Middlewares/Third_Party/ARM_Security/library -I../Common/include -I../Common/config -I../Common/sys -I../Common/ARM_Security -I../Common/hub75 -I../Common/hub75/Inc -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Common-2f-sys

clean-Common-2f-sys:
	-$(RM) ./Common/sys/hal_init.cyclo ./Common/sys/hal_init.d ./Common/sys/hal_init.o ./Common/sys/hal_init.su ./Common/sys/interrupt_handlers.cyclo ./Common/sys/interrupt_handlers.d ./Common/sys/interrupt_handlers.o ./Common/sys/interrupt_handlers.su ./Common/sys/mbedtls_freertos_port.cyclo ./Common/sys/mbedtls_freertos_port.d ./Common/sys/mbedtls_freertos_port.o ./Common/sys/mbedtls_freertos_port.su ./Common/sys/newlibc_stubs.cyclo ./Common/sys/newlibc_stubs.d ./Common/sys/newlibc_stubs.o ./Common/sys/newlibc_stubs.su

.PHONY: clean-Common-2f-sys

