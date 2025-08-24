# Contributing to PIOKMBox

Thank you for your interest in contributing to PIOKMBox! This document provides guidelines and information for contributors.

## Development Setup

### Prerequisites

1. **Raspberry Pi Pico SDK** - Install the latest Pico SDK
2. **ARM Toolchain** - arm-none-eabi-gcc
3. **CMake** - Version 3.13 or higher
4. **Git** - For version control and submodules

### Setting up the Development Environment

```bash
# Clone the repository with submodules
git clone --recursive https://github.com/yourusername/RaspberryKMBox.git
cd RaspberryKMBox

# Install Pico SDK (if not already installed)
# Follow instructions at: https://github.com/raspberrypi/pico-sdk

# Set environment variable
export PICO_SDK_PATH=/path/to/pico-sdk

# Build the project
./build.sh
```

## Building and Testing

### Quick Build Commands

```bash
# Build for Pico (RP2040)
./build.sh pico

# Build for Pico 2 (RP2350)
./build.sh pico2

# Build both targets
./build.sh both

# Clean build
./build.sh both clean

# Debug build
./build.sh pico debug
```

### Testing Your Changes

1. **Build both targets** to ensure compatibility
2. **Test on hardware** - Flash the firmware and verify functionality
3. **Check serial output** - Monitor debug UART (GPIO 0/1) at 115200 baud
4. **Test KMBox commands** - Verify serial protocol on GPIO 5/6

## Code Style and Standards

### C Code Guidelines

- Follow existing code style and formatting
- Use meaningful variable and function names
- Add comments for complex logic
- Keep functions focused and reasonably sized
- Use proper error handling

### File Organization

- **Header files** (.h) - Declarations and public interfaces
- **Source files** (.c) - Implementation
- **PIO files** (.pio) - Programmable I/O assembly code
- **CMake** - Build configuration in CMakeLists.txt

### Key Modules

- `PIOKMbox.c` - Main entry point and application loop
- `usb_hid.*` - USB HID device implementation
- `kmbox_serial_handler.*` - KMBox protocol handling
- `led_control.*` - Status LED and NeoPixel control
- `watchdog.*` - System monitoring and recovery
- `state_management.*` - System state coordination

## Submitting Changes

### Pull Request Process

1. **Fork the repository** and create a feature branch
2. **Make your changes** following the code guidelines
3. **Test thoroughly** on both RP2040 and RP2350 if possible
4. **Update documentation** if needed (README, comments)
5. **Submit a pull request** with a clear description

### Pull Request Template

```markdown
## Description
Brief description of your changes.

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Breaking change
- [ ] Documentation update

## Testing
- [ ] Tested on RP2040 (Pico)
- [ ] Tested on RP2350 (Pico 2)
- [ ] Serial communication tested
- [ ] USB functionality verified

## Checklist
- [ ] Code follows project style guidelines
- [ ] Self-review completed
- [ ] Documentation updated if needed
- [ ] No build warnings or errors
```

## Development Guidelines

### Adding New Features

1. **Plan the feature** - Consider impact on existing code
2. **Update state management** - Integrate with existing state machine
3. **Add appropriate logging** - Use existing logging macros
4. **Consider timing** - Respect real-time constraints
5. **Test edge cases** - USB disconnect, power cycling, etc.

### Debugging Tips

1. **Serial Debug Output**
   - Monitor GPIO 0/1 at 115200 baud
   - Use existing LOG_* macros for consistent output
   - Enable verbose logging for development builds

2. **LED Status Indicators**
   - Onboard LED shows USB device status
   - NeoPixel shows overall system status
   - Use these for visual debugging

3. **Build Configurations**
   - `BUILD_CONFIG_DEBUG` - Maximum verbosity
   - `BUILD_CONFIG_DEVELOPMENT` - Standard development
   - `BUILD_CONFIG_PRODUCTION` - Minimal logging

### Common Issues

1. **Build Failures**
   - Check PICO_SDK_PATH environment variable
   - Ensure all submodules are initialized
   - Verify ARM toolchain installation

2. **USB Host Issues**
   - Check 5V power pin (GPIO 18)
   - Verify USB host pins (GPIO 16/17)
   - Monitor core1 heartbeat in debug output

3. **Serial Communication**
   - Check UART pin connections (GPIO 5/6)
   - Verify baud rate (115200)
   - Test with simple terminal program

## Release Process

Releases are automated through GitHub Actions:

1. **Tag a release** - Create a git tag starting with 'v' (e.g., v1.0.0)
2. **GitHub Actions** - Automatically builds both targets
3. **Release created** - Binaries uploaded to GitHub Releases

### Version Numbering

Follow semantic versioning (semver):

- **Major** - Breaking changes
- **Minor** - New features, backward compatible
- **Patch** - Bug fixes, backward compatible

## Getting Help

- **Issues** - Use GitHub Issues for bug reports and feature requests
- **Discussions** - Use GitHub Discussions for questions and ideas
- **Documentation** - Check README.md for usage and setup information

## License

By contributing to PIOKMBox, you agree that your contributions will be licensed under the same license as the project.
