# PIOKMBox - Raspberry Pi Pico USB HID Device

A sophisticated USB HID device firmware for the Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) that provides mouse and keyboard emulation with KMBox serial command support.

## Features

- **Dual USB Stack**: Simultaneous USB device (HID) and USB host support using PIO USB
- **Dynamic VID/PID Mirroring**: Automatically adopts the VID/PID of connected USB devices for transparent passthrough
- **KMBox Serial Commands**: Compatible serial protocol for mouse/keyboard control
- **Full HID Passthrough**: Complete mouse and keyboard input forwarding including side buttons and scroll wheels
- **Dual Core Architecture**: Core 0 handles USB device tasks, Core 1 handles USB host tasks
- **Hardware Watchdog**: Robust system monitoring and recovery
- **Visual Status Indicators**: Onboard LED and WS2812 NeoPixel status display
- **Button Control**: Hardware button for USB stack reset
- **Multiple Targets**: Support for both RP2040 (Pico) and RP2350 (Pico 2)

## Hardware Requirements

### Supported Boards

Targets the Adafruit Feather RP2040 USB Host by default, you can update the pin macros to use any pico or pico2

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

## Usage

### Dynamic VID/PID Mirroring

The PIOKMBox features intelligent device mirroring that automatically adopts the identity of connected USB HID devices:

#### How It Works

1. **Automatic Detection**: When you connect a mouse or keyboard to the USB host port, the device automatically detects the VID (Vendor ID) and PID (Product ID)
2. **String Descriptor Mirroring**: The device fetches and mirrors the manufacturer, product, and serial number strings from the connected device
3. **Dynamic Re-enumeration**: The PIOKMBox disconnects from the PC and re-enumerates with the detected VID/PID and string descriptors
4. **Transparent Passthrough**: To the PC, it appears as if the original device is directly connected with identical identity
5. **Full Compatibility**: All device features are preserved, including side buttons, scroll wheels, and special keys

#### Example

```bash
# Before connecting a device
$ lsusb
... 9981:4001 Hurricane PIOKM Box

# After connecting a Logitech mouse
$ lsusb  
... 046d:c539 Logitech Inc. USB Receiver

# System sees identical device identity
$ system_profiler SPUSBDataType | grep -A 5 "046d"
Product ID: 0xc539
Vendor ID: 0x046d  (Logitech Inc.)
Manufacturer: Logitech
```

#### Benefits

- **Complete Device Cloning**: Mirrors VID/PID, manufacturer, product name, and serial number for perfect identity matching
- **Anti-Detection**: The device appears as the original hardware to the PC and any detection software
- **Full Feature Support**: All buttons and features work exactly as the original device
- **Automatic Configuration**: No manual setup required - just plug and play
- **Wide Compatibility**: Works with any USB HID mouse or keyboard

#### Fallback Behavior

- If no device is connected, uses default VID:PID `9981:4001`
- If device detection fails, gracefully falls back to default identity
- Supports hot-plugging - VID/PID updates when devices are swapped

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
- **Dynamic USB Descriptors**: Runtime generation of device descriptors based on connected devices
- **Smart Re-enumeration**: Intelligent USB re-enumeration only when VID/PID changes to prevent enumeration loops
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
