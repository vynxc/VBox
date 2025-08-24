# PIOKMBox - Raspberry Pi Pico USB HID Device

A sophisticated USB HID device firmware for the Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) that provides mouse and keyboard emulation with KMBox serial command support.

## Features

- **Dual USB Stack**: Simultaneous USB device (HID) and USB host support using PIO USB
- **KMBox Serial Commands**: Compatible serial protocol for mouse/keyboard control
- **Dual Core Architecture**: Core 0 handles USB device tasks, Core 1 handles USB host tasks
- **Hardware Watchdog**: Robust system monitoring and recovery
- **Visual Status Indicators**: Onboard LED and WS2812 NeoPixel status display
- **Button Control**: Hardware button for USB stack reset
- **Multiple Targets**: Support for both RP2040 (Pico) and RP2350 (Pico 2)

## Hardware Requirements

### Supported Boards

- Raspberry Pi Pico (RP2040)
- Raspberry Pi Pico 2 (RP2350)
- Adafruit Feather RP2040 USB Host

### Pin Configuration

- **USB Host**: GPIO 16 (D+), GPIO 17 (D-)
- **USB Host Power**: GPIO 18
- **Status LED**: GPIO 13
- **Reset Button**: GPIO 7
- **NeoPixel Data**: GPIO 21
- **NeoPixel Power**: GPIO 20
- **KMBox UART**: GPIO 5 (TX), GPIO 6 (RX) @ 115200 baud
- **Debug UART**: GPIO 0 (TX), GPIO 1 (RX) @ 115200 baud

## Building

### Prerequisites

1. **Raspberry Pi Pico SDK**: Install the latest Pico SDK
2. **CMake**: Version 3.13 or higher
3. **GCC ARM Toolchain**: arm-none-eabi-gcc
4. **Ninja Build System** (recommended)

### Quick Build

```bash
# Build for Pico (RP2040)
./build.sh pico

# Build for Pico 2 (RP2350)
./build.sh pico2

# Build both targets
./build.sh both

# Clean build both targets
./build.sh both clean
```

### Manual Build

```bash
# Create build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build
make -j4

# Or use ninja for faster builds
ninja
```

### Build Outputs

The build process generates several files in the build directory:

- `PIOKMbox.uf2` - Main firmware file for flashing
- `PIOKMbox.elf` - ELF executable for debugging
- `PIOKMbox.bin` - Raw binary file
- `PIOKMbox.hex` - Intel HEX format file

## Installation

### Method 1: Automatic (with picotool)

If you have `picotool` installed, the build script will automatically attempt to flash the firmware:

```bash
./build.sh pico
```

### Method 2: Manual UF2 Flashing

1. Hold the BOOTSEL button while connecting the Pico to USB
2. The Pico will appear as a USB mass storage device (RPI-RP2)
3. Copy `PIOKMbox.uf2` to the mounted drive
4. The device will automatically reboot and run the firmware

### Method 3: Debug Probe Flashing

For development with a debug probe:

```bash
# Flash RP2040
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "program PIOKMbox.elf verify reset exit"

# Flash RP2350
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "program PIOKMbox.elf verify reset exit"
```

## Usage

### Serial Communication

Connect to the KMBox UART (GPIO 5/6) at 115200 baud to send commands:

```text
# Mouse movement
km.move(100, 50)

# Mouse click
km.left(1)    # Press left button
km.left(0)    # Release left button

# Mouse click with timing
km.click(0, 100)  # Click left button for 100ms

# Axis locking
km.lock.mx(1)  # Lock X axis
km.lock.my(1)  # Lock Y axis
```

### Debug Output

Connect to the debug UART (GPIO 0/1) at 115200 baud to view system logs and status information.

### Status Indicators

#### LED (GPIO 13)

- **Fast Blink (250ms)**: USB device mounted or resumed
- **Medium Blink (1000ms)**: USB device unmounted
- **Slow Blink (2500ms)**: USB device suspended

#### NeoPixel (GPIO 21)

- **Blue**: Booting
- **Green**: USB device only
- **Orange**: USB host only  
- **Cyan**: Both USB stacks active
- **Magenta**: Mouse connected
- **Yellow**: Keyboard connected
- **Pink**: Both HID devices connected
- **Red**: Error state
- **Purple**: Suspended

### Hardware Button

- **Short Press**: Reserved for future use
- **Long Press (3+ seconds)**: Reset USB stacks

## Development

### Project Structure

```text
PIOKMbox/
├── PIOKMbox.c              # Main firmware entry point
├── CMakeLists.txt          # Build configuration
├── build.sh               # Build script
├── defines.h              # Hardware and timing constants
├── config.h               # Configuration headers
├── timing_config.h        # Timing configuration
├── led_control.*          # LED and NeoPixel control
├── usb_hid.*             # USB HID device implementation
├── watchdog.*            # System watchdog implementation
├── init_state_machine.*  # Initialization state management
├── state_management.*    # System state management
├── kmbox_serial_handler.* # KMBox serial protocol handler
├── pio_uart.*            # PIO-based UART implementation
├── *.pio                 # PIO assembly files
└── lib/
    ├── kmbox-commands/    # KMBox command parser library
    └── Pico-PIO-USB/     # PIO USB library
```

### Key Components

- **Dual Core Architecture**: Core 0 handles USB device and main application loop, Core 1 dedicated to USB host tasks
- **Watchdog System**: Hardware and software watchdog with inter-core monitoring
- **State Management**: Centralized system state with proper initialization sequencing
- **KMBox Protocol**: Complete implementation of KMBox serial command protocol
- **PIO USB**: Software USB host implementation using Programmable I/O

### Build Configuration

The firmware supports multiple build configurations:

- `BUILD_CONFIG_DEVELOPMENT` (default): Full logging and debug features
- `BUILD_CONFIG_PRODUCTION`: Optimized for release with minimal logging
- `BUILD_CONFIG_TESTING`: Enhanced debugging for testing
- `BUILD_CONFIG_DEBUG`: Maximum verbosity for development

## Troubleshooting

### Common Issues

1. **Build Failures**: Ensure Pico SDK is properly installed and `PICO_SDK_PATH` is set
2. **USB Host Not Working**: Check 5V power supply and USB host pin connections
3. **Serial Commands Not Responding**: Verify UART connections and baud rate (115200)
4. **Device Not Recognized**: Try different USB cables or ports

### Debug Methods

1. **Serial Debug Output**: Monitor GPIO 0/1 at 115200 baud for system logs
2. **Status LEDs**: Observe LED and NeoPixel patterns for system state
3. **Watchdog Reports**: Check periodic watchdog status in debug output
4. **Build Traces**: Enable `BOOTTRACE` messages for initialization debugging

## License

This project is open source. Please refer to the license file for details.

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## Support

For issues and questions:

1. Check the troubleshooting section above
2. Review debug output from serial console
3. Open an issue with detailed description and logs
