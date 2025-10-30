# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Project Overview

VBox is a dual-role USB HID firmware for Raspberry Pi Pico boards that forwards mouse and keyboard input from a USB device (host port) to a PC (device port) while accepting KMBox-compatible serial commands for automation. It uses TinyUSB for native device mode and PIO-USB for host mode, running concurrently on a dual-core architecture.

## Build Commands

### Prerequisites Setup (Windows)
The project uses the Pico SDK managed by VS Code extension:
- SDK path: `$env:USERPROFILE\.pico-sdk\sdk\2.2.0`
- Toolchain: `$env:USERPROFILE\.pico-sdk\toolchain\RISCV_ZCB_RPI_2_2_0_3`
- CMake: `$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin\cmake`

These paths are automatically set in the terminal by `.vscode\settings.json`.

### Build Process

**Standard build:**
```pwsh
mkdir build
cd build
cmake ..
ninja
```

**Clean rebuild:**
```pwsh
rm -r build
mkdir build
cd build
cmake ..
ninja
```

**Build outputs** (in `build/` directory):
- `vbox.uf2` - Primary firmware for drag-and-drop flashing
- `vbox.elf` - ELF for debugging
- `vbox.bin`, `vbox.hex` - Alternative images

### Flashing

**Manual UF2 method:**
1. Hold BOOTSEL button while plugging in USB
2. Device mounts as RPI-RP2
3. Copy `build/vbox.uf2` to the mounted drive

**With picotool (if available):**
```pwsh
picotool load -f build/vbox.uf2
```

### Configuration

**Board target:** Set in `CMakeLists.txt` line 26:
```cmake
set(PICO_BOARD waveshare_rp2350_usb_a CACHE STRING "Board type")
```

**Build configuration:** Set via CMake definitions (line 93):
- `BUILD_CONFIG_DEVELOPMENT` (default)
- `BUILD_CONFIG_PRODUCTION`
- `BUILD_CONFIG_TESTING`
- `BUILD_CONFIG_DEBUG`

## Architecture

### Dual-Core Design

**Core 0 (USB Device + Main Loop):**
- Runs TinyUSB device stack (native hardware USB controller)
- Main application loop in `vbox.c`
- HID device task processing (~125 FPS at 8ms intervals)
- LED/NeoPixel visual feedback
- Watchdog monitoring
- KMBox serial command processing (UART1)

**Core 1 (USB Host):**
- Runs TinyUSB host stack on PIO-USB (software USB via PIO)
- Launched via `multicore_launch_core1()` in `vbox.c`
- Runs `core1_task_loop()` - continuously calls `tuh_task()`
- Heartbeat updates for watchdog system
- Communicates with Core 0 via shared state structures

### Key Subsystems

**USB HID (`usb_hid.c/h`):**
- Dynamic identity mirroring: adopts VID/PID and string descriptors from attached HID device
- Fallback defaults: VID:PID `0x9981:0x4001`, Manufacturer "Hurricane", Product "PIOKM Box"
- HID report descriptor generation and processing
- Mouse and keyboard report handling
- USB re-enumeration when VID/PID changes
- Error detection and stack reset capabilities

**LED Control (`led_control.c/h`):**
- Status LED blinking with configurable intervals
- WS2812 NeoPixel control via PIO (ws2812.pio)
- Visual status indicators:
  - Blue: booting
  - Green: device only
  - Orange: host only
  - Cyan: both stacks active
  - Magenta: mouse connected
  - Yellow: keyboard connected
  - Pink: mouse + keyboard
  - Red: error
  - Purple: suspended

**Watchdog (`watchdog.c/h`):**
- Hardware watchdog (90s timeout)
- Inter-core heartbeat monitoring (30s timeout between cores)
- System health status tracking
- Automatic reset on core failure

**Init State Machine (`init_state_machine.c/h`):**
- Manages startup sequencing through defined states
- Handles retry logic for USB initialization
- Coordinates Core 0 and Core 1 initialization

**KMBox Serial Handler (`kmbox_serial_handler.c/h`):**
- UART1 interface (GPIO 5/6, 115200 baud)
- Parses KMBox protocol commands from `lib/kmbox-commands/`
- Supports: movement injection, button press/release, timed clicks, wheel, axis locks

**State Management (`state_management.c/h`):**
- Shared system state between cores
- Thread-safe state access patterns

### Configuration Headers

**`defines.h`:**
Centralized configuration for:
- GPIO pin assignments (USB host DP/DM, LED, button, NeoPixel, UART)
- Timing constants (delays, intervals, timeouts)
- CPU frequency settings (240 MHz for RP2040, 120 MHz for RP2350)
- USB descriptors and endpoints
- Build configuration presets

**`timing_config.h`:**
Fine-tuned timing parameters for system tasks

**`tusb_config.h`:**
TinyUSB stack configuration for device and host modes

**`config.h`:**
Project-wide configuration settings

## Development Guidelines

### USB Host Pin Configuration
When modifying USB host pins, update in **three** places:
1. `defines.h` - `PIN_USB_HOST_DP` and `PIN_USB_HOST_DM`
2. `CMakeLists.txt` - compile definitions at line 97-98
3. `core1_main()` in `vbox.c` - PIO USB configuration

### Adding New HID Report Types
1. Add report ID enum to `usb_hid.h`
2. Update report descriptor in `usb_hid.c`
3. Add processing function similar to `process_kbd_report()` or `process_mouse_report()`
4. Update `tuh_hid_report_received_cb()` to handle new report type

### Timing Modifications
All timing constants are in `defines.h` - modify there rather than hard-coding values. Key intervals:
- `HID_DEVICE_TASK_INTERVAL_MS` (8ms) - affects USB polling rate
- `WATCHDOG_TASK_INTERVAL_MS` (100ms) - watchdog check frequency
- `VISUAL_TASK_INTERVAL_MS` (50ms) - LED update rate

### Debug Output
- Debug UART: UART0 (GPIO 0/1, 115200 baud)
- Enabled via `pico_enable_stdio_uart(vbox 1)` in CMakeLists.txt
- Use standard `printf()` for debug output
- Disable in production by setting `pico_enable_stdio_uart(vbox 0)`

### Mouse Coordinate Handling
16-bit mouse deltas from HID reports are scaled to 8-bit range (-127 to 127). Adjust scaling logic in `process_mouse_report()` if sensitivity needs tuning.

### Watchdog System
- Hardware watchdog automatically resets if not updated within 90 seconds
- Inter-core monitoring resets if Core 1 stops responding within 30 seconds
- Call `watchdog_core0_heartbeat()` and `watchdog_core1_heartbeat()` regularly from respective cores
- Disable during development/debugging by setting `WATCHDOG_ENABLE_HARDWARE 0` in defines.h

## Common Issues

**Re-enumeration loops:**
Firmware only re-enumerates when VID/PID changes. Check that attached device provides stable descriptors.

**USB host not working:**
- Verify 5V power to USB host port
- Confirm DP/DM wiring matches GPIO 16/17 (or your custom pins)
- Check Core 1 is launching successfully via debug UART

**Build errors with Pico SDK:**
Ensure `PICO_SDK_PATH` environment variable is set correctly. The VS Code extension should handle this automatically.

**PIO USB timing issues:**
CPU must run at 240 MHz for RP2040 or 120 MHz for RP2350. Clock setting happens in `initialize_system()` via `set_sys_clock_khz(CPU_FREQ, true)`.
