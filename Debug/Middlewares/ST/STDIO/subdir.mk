################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Middlewares/ST/STDIO/stdin_usart.c \
../Middlewares/ST/STDIO/stdout_usart.c 

OBJS += \
./Middlewares/ST/STDIO/stdin_usart.o \
./Middlewares/ST/STDIO/stdout_usart.o 

C_DEPS += \
./Middlewares/ST/STDIO/stdin_usart.d \
./Middlewares/ST/STDIO/stdout_usart.d 


# Each subdirectory must supply rules for building sources it contributes
Middlewares/ST/STDIO/%.o Middlewares/ST/STDIO/%.su Middlewares/ST/STDIO/%.cyclo: ../Middlewares/ST/STDIO/%.c Middlewares/ST/STDIO/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m33 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32U585xx -DUSE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION=1 '-DMBEDTLS_CONFIG_FILE="mbedtls_config_ntz.h"' '-DLFS_CONFIG=lfs_config.h' -DLFS_USE_INTERNAL_NOR -DUSE_STSAFE=0 -c -I../Core/Inc -I../STDIO -I../Drivers/STM32U5xx_HAL_Driver/Inc -I../Drivers/STM32U5xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32U5xx/Include -I../Drivers/CMSIS/Include -I../Middlewares/Third_Party/AWS_FreeRTOS/coreJSON/source/include/ -I../Middlewares/Third_Party/lwIP_Network_lwIP/lwip/src/include/ -I../Common/net/lwip_port/include -I../Common/net/lwip_port/include/arch -I../Common/net/mxchip -I../Middlewares/Third_Party/FreeRTOS/Source/include/ -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure/ -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/ -I../Middlewares/Third_Party/CMSIS/RTOS2/Include/ -I../Middlewares/Third_Party/ARM_Security/include/ -I../Middlewares/Third_Party/ARM_Security/RTE/include/ -I../Middlewares/Third_Party/ARM_Security/RTE/configs -I../Middlewares/Third_Party/ARM_Security/library -I../Common/include -I../Common/config -I../Common/sys -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Middlewares-2f-ST-2f-STDIO

clean-Middlewares-2f-ST-2f-STDIO:
	-$(RM) ./Middlewares/ST/STDIO/stdin_usart.cyclo ./Middlewares/ST/STDIO/stdin_usart.d ./Middlewares/ST/STDIO/stdin_usart.o ./Middlewares/ST/STDIO/stdin_usart.su ./Middlewares/ST/STDIO/stdout_usart.cyclo ./Middlewares/ST/STDIO/stdout_usart.d ./Middlewares/ST/STDIO/stdout_usart.o ./Middlewares/ST/STDIO/stdout_usart.su

.PHONY: clean-Middlewares-2f-ST-2f-STDIO

