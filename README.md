# MTA STM32U5 MXCHIP Wi-Fi Subway LED Display

## Technical Summary

Firmware for an STM32U585-based embedded display system that integrates FreeRTOS, a HUB75 LED matrix, MXCHIP Wi-Fi networking, lwIP, mbedTLS, and a custom MTA GTFS-Realtime protobuf client. The project initializes MCU peripherals, manages Wi-Fi connectivity and SNTP time sync, fetches encrypted MTA route updates, decodes rail arrival data, and renders a real-time C/G train board on a 32x16 LED panel.

## System Architecture

- `Core/Src/main.c` initializes HAL, clocks, peripherals, and starts the CMSIS-RTOS scheduler.
- `Core/Src/app_freertos.c` creates the runtime task topology, including heartbeat, LED matrix UI, network engine, logging consumer, and MTA API client.
- `Common/net/mxchip/mx_netconn.c` implements the MXCHIP Wi-Fi control plane, lwIP interface registration, DHCP, and network state event handling.
- `Core/Src/mta_task.c` contains the MTA API client, TLS connection logic, HTTP request/response handling, and custom protobuf parsing for C/G train updates.
- `Core/Src/led_matrix_task.c` renders the current time and positional train arrival minutes to the HUB75 LED panel.
- `Common/hub75/` contains the display abstraction, pixel framebuffer, bitplane DMA generation, and text/scroll rendering macros.
- `Core/Src/logging.c` provides FreeRTOS-safe message buffering and UART log output.

## Core Components / Modules

- `main.c`: HAL/clock/power initialization and RTOS startup.
- `app_freertos.c`: RTOS task creation, event synchronization, SNTP initialization, and launch order.
- `mta_task.c`: HTTPS client, mbedTLS session lifecycle, protobuf parsing, route-specific arrival extraction, and stream buffer dispatch.
- `mx_netconn.c`: Wi-Fi module initialization, connection state machine, netif control, DHCP, and reconnect logic.
- `led_matrix_task.c`: stream buffer consumption, RTC header formatting, vertical scrolling of C/G times, and HUB75 framebuffer updates.
- `hub75.c` + headers: low-level GPIO control, double-buffered bitplane generation, pixel drawing primitives, and buffer swap.
- `logging.c`: early and runtime logging paths, ISR-safe message buffering, UART transmission.
- `sntp_sync.c`: SNTP-to-hardware RTC update and event notification.

## Data Flow / Control Flow

- Startup: `main()` configures MCU peripherals then initializes FreeRTOS.
- Task orchestration: `app_freertos.c` launches heartbeat, `vLedMatrixTask`, `net_main`, `vLoggingConsumerTask`, and after network/time ready, `vMtaApiTask`.
- Network initialization: `net_main()` initializes lwIP, starts MXCHIP dataplane and control plane tasks, and registers the netif.
- Wi-Fi control: `mx_netconn.c` manages MXCHIP SPI/GPIO handshake, monitors module status notifications, and drives DHCP/IP acquisition.
- Time sync: once network connectivity is established, SNTP is configured and `EVT_MASK_TIME_SYNCED` is awaited before starting the MTA client.
- MTA feed pipeline: `vMtaApiTask` fetches two hard-coded GTFS-Realtime feed URIs, parses protobuf payloads, extracts stop arrival events for targeted route IDs, sorts arrival timestamps, computes minutes-to-arrival, and writes a 6-byte positional packet into `xMtaTimBuf`.
- Display pipeline: `vLedMatrixTask` reads the 6-byte stream buffer, maintains separate C/G vertical scrollers, and renders the RTC header plus two train columns to the HUB75 panel at ~20 FPS.
- Logging: asynchronous log producer functions enqueue messages to `xLogMBuf`; `vLoggingConsumerTask` flushes them to UART.

## Hardware / Software Stack

- MCU: STM32U585 family.
- Board: B-U585I-IOT02A (STM32U5 Discovery kit) via CubeMX-generated project config.
- RTOS: FreeRTOS with CMSIS-RTOS V2 wrapper.
- Networking: lwIP stack, MXCHIP SPI/GPIO Wi-Fi module driver, DHCP, SNTP.
- Security: mbedTLS TLS client for HTTPS over lwIP.
- Display: HUB75 32x16 RGB LED panel driver with platform-specific pin mapping.
- Peripherals: SPI2, USART1, RTC, RNG, TIM2, GPIO, ICACHE, DCACHE, GPDMA.
- Build: GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`).
- Debug / console: `USART1` on PA9/PA10 routed to ST-LINK VCP and `Middlewares/ST/STDIO/stdout_usart.c` retargets `printf`/`fputc` to UART.
- RNG: hardware RNG provides entropy to mbedTLS through `Core/Src/crypto/rng_alt_stm32.c`.
- Display refresh: `TIM2` drives `HUB75_ISR()` at a 1250-tick base period, giving bitplane timed PWM and row multiplexing.

## Physical Interface and Connectivity Stack

### LED matrix timing and HUB75

- `Common/hub75/Inc/hub75.h` defines the B-U585I-IOT02A pin mapping for a 32x16 HUB75 panel.
- `Common/hub75/hub75.c` initializes Port E data/color lines, Port C row address lines, and Port D control lines.
- `TIM2` is started in `HUB75_Init()` and its overflow callback in `Core/Src/tim.c` calls `HUB75_ISR()`.
- `HUB75_ISR()` blanks the panel, latches the row, updates the address multiplex lines, and steps through the BCM bitplane timing by updating `htim2.Instance->ARR`.
- `HUB75_SwapBuffers()` pre-encodes the active/clock phases into a double-buffered stream before the ISR consumes it.

### Wi-Fi stack: MXCHIP over SPI + lwIP

- The MXCHIP EMW3080 Wi-Fi module is connected over `SPI2` using the HAL SPI interface on PD1/PD3/PD4.
- `Core/Src/spi.c` configures `SPI2` with DMA for both TX and RX and links GPDMA1 channels 4/5.
- GPIO mappings in `Core/Inc/main.h` and `Common/net/mxchip/mx_gpio.c` define `MXCHIP_NSS`, `MXCHIP_RESET`, `MXCHIP_FLOW`, and `MXCHIP_NOTIFY`.
- `Common/net/mxchip/mx_dataplane.c` uses `HAL_SPI_TransmitReceive_DMA()` for all module transfers and waits for DMA completion via FreeRTOS task notifications.
- The module notify/flow callbacks wake the dataplane task when new data is ready or when the module can accept more traffic.
- `Common/net/mxchip/mx_lwip.c` registers the lwIP `netif` and implements `linkoutput` and packet receive hooks, packaging Ethernet frames with the MXCHIP bypass header and routing them to lwIP.

### TLS / mbedTLS / FreeRTOS coordination

- The TLS client uses lwIP socket APIs wrapped by `Common/config/tls_transport_lwip.h` and `Common/include/mbedtls_transport.h` so mbedTLS operates on the embedded IP stack.
- `Core/Src/mta_task.c` allocates `mbedtls_ssl_context`, `mbedtls_ctr_drbg_context`, `mbedtls_entropy_context`, and establishes an HTTPS session to `api-endpoint.mta.info` port `443`.
- `Common/sys/mbedtls_freertos_port.c` provides heap allocation and threading primitives for mbedTLS using FreeRTOS memory and mutex wrappers.
- `Core/Src/crypto/rng_alt_stm32.c` implements `mbedtls_hardware_poll()` using the STM32 RNG peripheral and IRQ-driven completion to supply entropy to the TLS stack.

### RTC / SNTP time synchronization

- The RTC is initialized in `Core/Src/rtc.c` using `LSE` as the clock source.
- After the network is up, `app_freertos.c` configures SNTP to Google Anycast NTP server `216.239.35.0` and starts `sntp_init()` under the lwIP core lock.
- `sntp_sync.c` receives epoch seconds, applies a fixed UTC-4 offset, converts to `struct tm`, and writes the time and date into the STM32 hardware RTC.
- The display task reads the RTC through `HAL_RTC_GetTime()` and `HAL_RTC_GetDate()` to show the current header timestamp on the LED panel.

### USART and debug output

- `Core/Src/usart.c` initializes `USART1` on PA9/PA10 at 115200 baud, no parity, 8N1.
- `Middlewares/ST/STDIO/stdout_usart.c` hooks `_write()` to `HAL_UART_Transmit()` so `printf()` and logging output are visible over ST-LINK VCP.
- The logger is initialized early in `app_freertos.c` so runtime status, network events, SNTP sync, and MTA feed parsing are emitted to the console.

## CubeMX / STM32CubeIDE Integration

- This repository includes the STM32CubeMX project config file `mtaTimes.ioc` at the project root.
- Open `mtaTimes.ioc` in STM32CubeMX or STM32CubeIDE to inspect the B-U585I-IOT02A pin/peripheral configuration and regenerate code if pins are changed.
- The project config sets up SPI2, USART1, RTC, RNG, TIM2, and GPIOs used by MXCHIP and the HUB75 panel.
- For CubeIDE debugging, import/open `mtaTimes.ioc`, build the generated project, and use the ST-LINK debug configuration targeting `mtaTimes.elf`.

## Build and Setup Instructions

1. Clone the repository:
   ```bash
   git clone https://github.com/AlnurElberier/U5_mxchip_FreeRTOS
   cd mxchipFreertos
   ```

2. Open STM32CubeIDE and import the project:
   - **File** → **Import** → **Existing Projects into Workspace**
   - Select the cloned `mxchipFreertos` folder
   - Click **Finish**

3. Build the project:
   - Right-click the project in Project Explorer → **Build Project**
   - The output ELF `mtaTimes.elf` will be generated in the `Debug/` folder

4. Debug / Flash:
   - Connect ST-LINK to the board
   - Right-click the project → **Debug As** → **STM32 C/C++ Application**
   - CubeIDE will flash and launch the debugger

## Configuration Details

- Wi-Fi credentials are hard-coded in `Core/Inc/main.h`:
  - `SSID "Alnur"`
  - `PSWD "testtesttest"`
  - Edit these directly and rebuild to change Wi-Fi connection details.

- MTA feed endpoints are defined in `Core/Src/mta_task.c`:
  - `/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-ace` (C/A/E trains)
  - `/Dataservice/mtagtfsfeeds/nyct%2Fgtfs-g` (G train)

- **Stop IDs and Route configuration** in `Core/Src/mta_task.c`:
  - **Stop IDs**: Hardcoded at line ~170 as `"A44N"` (Clinton-Washington Avs) and `"G35N"` (Clinton-Washington Avs)
    - To change, modify the `strcmp()` conditions in the `prvParseStopTimeUpdate()` function
    - Example: replace `strcmp(cStopId, "A44N")` with your target stop ID
  - **Route IDs**: Hardcoded at lines ~222 and ~232 as `"C"` and `"G"`
    - To add/change routes, modify the MTA feed endpoints above and update the route comparisons in `prvParseTripUpdate()`
    - The display expects a 6-byte stream: 3 minutes for the first route, 3 minutes for the second route
 - Route and stop information can be found in the MTA [Developer Resourse Guide](https://www.mta.info/developers)

- The `led_matrix_task` expects a 6-byte stream buffer packet structured as three C-route minute values followed by three G-route values.
- `Common/include/sys_evt.h` defines runtime event bits for network and time synchronization.
- `Common/config/lwipopts.h` and `Common/net/lwip_port/include/lwipopts_freertos.h` configure lwIP behavior.
- The HUB75 driver pin definitions are centralized in `Common/hub75/Inc/hub75.h`; changing to a different 16x32 display requires remapping these GPIO definitions to your physical wiring.

## Key Design Decisions

- Explicit RTOS task separation: UI, networking, logging, heartbeat, and MTA client run in dedicated FreeRTOS tasks.
- Hardware abstraction for MXCHIP Wi-Fi is isolated in `Common/net/mxchip/` and decoupled from application logic.
- Custom protobuf parsing in `mta_task.c` avoids generated protobuf libraries and extracts only targeted route/stop fields for C and G service.
- The display subsystem uses a double-buffered bitplane strategy and DMA-based HUB75 rendering for efficient LED refresh.
- Event groups synchronize critical startup phases: network readiness (`EVT_MASK_NET_CONNECTED`) and SNTP time sync (`EVT_MASK_TIME_SYNCED`).

## Runtime / Execution Notes

- On reset, the firmware boots peripherals, starts FreeRTOS, and first brings up the LED matrix and network engine.
- The network task monitors MXCHIP status changes and brings up the lwIP interface.
- After DHCP and SNTP sync, `vMtaApiTask` begins polling MTA endpoints every 20 seconds.
- `vLedMatrixTask` continuously refreshes the display every 50 ms.
- `Error_Handler()` disables interrupts and loops on fatal HAL failures.

## Debugging / Testing Approach

- Runtime diagnostics are output over `USART1` via `logging.c`.
- `vApplicationMallocFailedHook`, `vApplicationStackOverflowHook`, and `vApplicationIdleHook` are implemented in `app_freertos.c`.
- No unit-test harness or automated tests are present in the repository; debugging is expected via UART logs and hardware state.

## Repository Structure Overview

- `Core/Inc` and `Core/Src`: application firmware, RTOS init, HAL peripheral drivers, display task, MTA client, and system startup.
- `Common/net/mxchip`: MXCHIP Wi-Fi driver, IPC, lwIP glue, and SPI/GPIO control.
- `Common/hub75`: HUB75 LED matrix drivers and font rendering primitives.
- `Common/sys`: FreeRTOS integration helpers, HAL init, interrupt handlers, and mbedTLS RTOS port.
- `Middlewares/Third_Party`: bundled FreeRTOS, lwIP, ARM Security (mbedTLS/PSA), and STDIO support.
- `Drivers/STM32U5xx_HAL_Driver`: STM32 HAL sources.
- `STM32U585AIIXQ_FLASH.ld` / `STM32U585AIIXQ_RAM.ld`: linker scripts.

## Future Improvements

- No explicit test framework is included.
- Credential management is currently hard-coded and should be moved to a secure configuration store.
- Protobuf parsing is minimal and route/stop-specific; a generated schema parser could improve maintainability.
- Hardware RTC offset handling is fixed to UTC-4 in `sntp_sync.c`; timezone support is not generalized.
- Add a system watchdog and stale-data detection path so the LED board does not display expired arrival values.
- Add a configuration interface for route and stop selection, either via BLE or AT-style command handling, instead of hard-coded MTA route IDs.
- Extend the UI to show weather or a secondary data source in the top header area instead of only date/time.
- Custom PCB design in KiCad should be feasible by preserving the MXCHIP SPI/GPIO wiring and HUB75 pin mappings from this repo.
